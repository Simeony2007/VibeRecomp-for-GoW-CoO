/*
 * PSP Allegrex HLE SysMemUserForUser Memory Manager
 * Implements sceKernelTotalFreeMemSize, sceKernelMaxFreeMemSize,
 * sceKernelAllocPartitionMemory, sceKernelGetBlockHeadAddr, sceKernelFreePartitionMemory
 */

#include "sceSysMem.h"
#include "mips_cpu.h"
#include "mips_hle.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HLE_MemBlock g_mem_blocks[MAX_MEM_BLOCKS] = {0};
// Kernel object UIDs do PSP não começam em 1; mantenha os IDs de usuário
// fora dos valores baixos reservados pelo kernel.
int g_next_block_id = 0x10;
uint32_t g_next_partition_alloc_addr = 0x09000000;

typedef struct {
    uint32_t address;
    uint32_t size;
    bool active;
} HLE_MeAllocation;

static HLE_MeAllocation g_me_allocations[MAX_MEM_BLOCKS];

static uint32_t allocate_me_memory(MIPS_CPU *cpu, uint32_t size, bool clear) {
    if (cpu == NULL || size == 0) return 0x800200D9; // MEMBLOCK_ALLOC_FAILED

    int slot = -1;
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (!g_me_allocations[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0x800200D9;

    uint64_t aligned_address = ((uint64_t)g_next_partition_alloc_addr + 63u) & ~UINT64_C(63);
    uint64_t aligned_size = ((uint64_t)size + 63u) & ~UINT64_C(63);
    if (aligned_address + aligned_size > UINT32_MAX) return 0x800200D9;

    uint32_t address = (uint32_t)aligned_address;
    uint8_t *memory = (uint8_t *)mips_get_ptr_range(cpu, address, size);
    if (memory == NULL) return 0x800200D9;

    if (clear) memset(memory, 0, size);
    g_me_allocations[slot].address = address;
    g_me_allocations[slot].size = size;
    g_me_allocations[slot].active = true;
    g_next_partition_alloc_addr = (uint32_t)(aligned_address + aligned_size);
    printf("[HLE sceMeMemory] Allocated %u bytes at 0x%08X (zeroed=%s)\n",
           size, address, clear ? "yes" : "no");
    return address;
}

uint32_t hle_sceMeMalloc(MIPS_CPU *cpu, uint32_t size) {
    return allocate_me_memory(cpu, size, false);
}

uint32_t hle_sceMeCalloc(MIPS_CPU *cpu, uint32_t size, uint32_t count) {
    uint64_t total = (uint64_t)size * count;
    if (total == 0 || total > UINT32_MAX) return 0x800200D9;
    return allocate_me_memory(cpu, (uint32_t)total, true);
}

uint32_t hle_sceMeFree(uint32_t address) {
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (g_me_allocations[i].active &&
            g_me_allocations[i].address == address) {
            g_me_allocations[i].active = false;
            printf("[HLE sceMeMemory] Freed allocation at 0x%08X\n", address);
            return 0;
        }
    }
    return 0x800200D3; // ILLEGAL_ADDR
}

void hle_sceKernelTotalFreeMemSize(MIPS_CPU *cpu) {
    uint32_t free_mem = 24 * 1024 * 1024; // 24 MB free RAM for user space
    printf("[HLE SysMem] sceKernelTotalFreeMemSize (NID 0x342061E5) -> Returning %u bytes (24MB)\n", free_mem);
    cpu->gpr[0x02] = free_mem;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelMaxFreeMemSize(MIPS_CPU *cpu) {
    uint32_t max_free = 24 * 1024 * 1024; // 24 MB free contiguous RAM
    printf("[HLE SysMem] sceKernelMaxFreeMemSize -> Returning %u bytes (24MB)\n", max_free);
    cpu->gpr[0x02] = max_free;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelAllocPartitionMemory(MIPS_CPU *cpu) {
    uint32_t partition_id = cpu->gpr[0x04]; // $a0
    uint32_t name_ptr = cpu->gpr[0x05];      // $a1
    uint32_t type = cpu->gpr[0x06];          // $a2
    uint32_t size = cpu->gpr[0x07];          // $a3
    const char *name = (const char *)mips_get_ptr(cpu, name_ptr);

    int slot = -1;
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (!g_mem_blocks[i].is_active) {
            slot = i;
            break;
        }
    }

    if (slot != -1) {
        uint32_t aligned_size = (size + 255) & ~255;
        if (aligned_size == 0) aligned_size = 256;

        // NOVO: valida que o bloco cabe dentro da RAM antes de entregar o endereco
        if (g_next_partition_alloc_addr + aligned_size > 0x0C000000) { // ajuste pro fim real da RAM
            printf("[HLE SysMem Error] sceKernelAllocPartitionMemory: sem espaco - endereco 0x%08X + %u estouraria a RAM\n",
                g_next_partition_alloc_addr, aligned_size);
            cpu->gpr[0x02] = 0x800200D9;
            cpu->pc = cpu->gpr[0x1F];
            cpu->next_pc = cpu->pc + 4;
            return;
        }
        
        g_mem_blocks[slot].id = g_next_block_id++;
        g_mem_blocks[slot].partition_id = partition_id;
        g_mem_blocks[slot].type = type;
        g_mem_blocks[slot].size = size;
        g_mem_blocks[slot].addr = g_next_partition_alloc_addr;
        if (name) {
            strncpy(g_mem_blocks[slot].name, name, 31);
            g_mem_blocks[slot].name[31] = '\0';
        } else {
            strcpy(g_mem_blocks[slot].name, "unnamed");
        }
        g_mem_blocks[slot].is_active = true;

        g_next_partition_alloc_addr += aligned_size;
        uint32_t block_id = g_mem_blocks[slot].id;

        printf("[HLE SysMem] sceKernelAllocPartitionMemory(part=%u, name='%s', type=%u, size=%u) -> Allocated Block ID %d at RAM 0x%08X (aligned %u bytes)\n",
               partition_id, g_mem_blocks[slot].name, type, size, block_id, g_mem_blocks[slot].addr, aligned_size);
        cpu->gpr[0x02] = block_id;
    } else {
        printf("[HLE SysMem Error] sceKernelAllocPartitionMemory: Out of memory block slots!\n");
        cpu->gpr[0x02] = 0x800200D9; // SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED
    }
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelGetBlockHeadAddr(MIPS_CPU *cpu) {
    uint32_t block_id = cpu->gpr[0x04]; // $a0
    uint32_t head_addr = 0;
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (g_mem_blocks[i].is_active && g_mem_blocks[i].id == (int)block_id) {
            head_addr = g_mem_blocks[i].addr;
            break;
        }
    }
    if (head_addr != 0) {
        printf("[HLE SysMem] sceKernelGetBlockHeadAddr(block=%u) -> RAM 0x%08X\n", block_id, head_addr);
        cpu->gpr[0x02] = head_addr;
    } else {
        printf("[HLE SysMem Warning] sceKernelGetBlockHeadAddr(block=%u): Unknown block ID\n", block_id);
        cpu->gpr[0x02] = 0x800200CB; // SCE_KERNEL_ERROR_UNKNOWN_UID
    }
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelFreePartitionMemory(MIPS_CPU *cpu) {
    uint32_t block_id = cpu->gpr[0x04]; // $a0
    bool freed = false;
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (g_mem_blocks[i].is_active && g_mem_blocks[i].id == (int)block_id) {
            g_mem_blocks[i].is_active = false;
            
            // NOVO: aviso de que o endereco liberado nao volta pro pool -
            // o alocador atual e' bump-only (so anda pra frente).
            printf("[HLE SysMem Info] Bloco %u ('%s') liberado, mas endereco 0x%08X NAO sera reaproveitado (alocador bump-only)\n",
            g_mem_blocks[i].id, g_mem_blocks[i].name, g_mem_blocks[i].addr);

            freed = true;
            printf("[HLE SysMem] sceKernelFreePartitionMemory(block=%u, '%s') -> PSP_OK\n", block_id, g_mem_blocks[i].name);
            break;
        }
    }
    if (!freed) {
        printf("[HLE SysMem Warning] sceKernelFreePartitionMemory(block=%u): Block ID not found\n", block_id);
    }
    cpu->gpr[0x02] = 0; // PSP_OK
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

static bool read_guest_string(MIPS_CPU *cpu, uint32_t address,
                              char *buffer, size_t capacity) {
    if (capacity == 0) return false;

    for (size_t i = 0; i + 1 < capacity; i++) {
        const char *byte = (const char *)mips_get_ptr_range(cpu, address + (uint32_t)i, 1);
        if (byte == NULL) {
            buffer[i] = '\0';
            return false;
        }
        buffer[i] = *byte;
        if (buffer[i] == '\0') return true;
    }
    buffer[capacity - 1] = '\0';
    return false;
}

static uint32_t get_printf_argument(MIPS_CPU *cpu, unsigned int index) {
    if (index < 3) return cpu->gpr[5 + index];
    return mips_read32(cpu, cpu->gpr[29] + 16 + (index - 3) * 4);
}

void hle_sceKernelPrintf(MIPS_CPU *cpu) {
    char format[512];
    char output[1024];
    size_t output_length = 0;
    unsigned int argument_index = 0;
    bool supported = true;

    if (!read_guest_string(cpu, cpu->gpr[4], format, sizeof(format))) {
        printf("[HLE Kernel Printf Error] Invalid or unterminated format string at 0x%08X\n",
               cpu->gpr[4]);
        cpu->gpr[2] = (uint32_t)-1;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    for (size_t i = 0; format[i] != '\0' && output_length + 1 < sizeof(output);) {
        if (format[i] != '%') {
            output[output_length++] = format[i++];
            continue;
        }

        i++;
        if (format[i] == '%') {
            output[output_length++] = '%';
            i++;
            continue;
        }

        char conversion[8] = "%";
        size_t conversion_length = 1;
        if (format[i] == '0' && format[i + 1] == '8' &&
            (format[i + 2] == 'x' || format[i + 2] == 'X')) {
            conversion[conversion_length++] = format[i++];
            conversion[conversion_length++] = format[i++];
            conversion[conversion_length++] = format[i++];
        } else if (format[i] == 'd' || format[i] == 'i' ||
                   format[i] == 'u' || format[i] == 'x' ||
                   format[i] == 'X' || format[i] == 'p' ||
                   format[i] == 's' || format[i] == 'c') {
            conversion[conversion_length++] = format[i++];
        } else {
            supported = false;
            break;
        }
        conversion[conversion_length] = '\0';

        uint32_t argument = get_printf_argument(cpu, argument_index++);
        char value[512];
        int written;
        if (conversion[conversion_length - 1] == 's') {
            if (argument == 0) {
                memcpy(value, "(null)", sizeof("(null)"));
            } else if (!read_guest_string(cpu, argument, value, sizeof(value))) {
                supported = false;
                break;
            }
            written = snprintf(output + output_length, sizeof(output) - output_length,
                               "%s", value);
        } else if (conversion[conversion_length - 1] == 'c') {
            written = snprintf(output + output_length, sizeof(output) - output_length,
                               "%c", (int)(argument & 0xFF));
        } else if (conversion[conversion_length - 1] == 'd' ||
                   conversion[conversion_length - 1] == 'i') {
            written = snprintf(output + output_length, sizeof(output) - output_length,
                               "%d", (int32_t)argument);
        } else if (conversion[conversion_length - 1] == 'p') {
            written = snprintf(output + output_length, sizeof(output) - output_length,
                               "%08" PRIX32, argument);
        } else {
            written = snprintf(value, sizeof(value), conversion, argument);
            if (written >= 0) {
                size_t value_length = (size_t)written;
                if (value_length >= sizeof(output) - output_length) {
                    value_length = sizeof(output) - output_length - 1;
                }
                memcpy(output + output_length, value, value_length);
            }
        }

        if (written < 0) {
            supported = false;
            break;
        }
        if (conversion[conversion_length - 1] == 's' ||
            conversion[conversion_length - 1] == 'c' ||
            conversion[conversion_length - 1] == 'd' ||
            conversion[conversion_length - 1] == 'i' ||
            conversion[conversion_length - 1] == 'p') {
            size_t remaining = sizeof(output) - output_length;
            output_length += (size_t)written < remaining
                ? (size_t)written : remaining - 1;
        } else {
            size_t remaining = sizeof(output) - output_length;
            output_length += (size_t)written < remaining
                ? (size_t)written : remaining - 1;
        }
    }

    output[output_length] = '\0';
    if (supported) {
        printf("[HLE Kernel Printf] %s\n", output);
    } else {
        printf("[HLE Kernel Printf Warning] Unsupported format: %s\n", format);
    }
    cpu->gpr[2] = 0;
    cpu->pc = cpu->gpr[31];
    cpu->next_pc = cpu->pc + 4;
}
