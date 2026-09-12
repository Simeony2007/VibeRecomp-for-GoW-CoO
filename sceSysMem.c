/*
 * PSP Allegrex HLE SysMemUserForUser Memory Manager
 * Implements sceKernelTotalFreeMemSize, sceKernelMaxFreeMemSize,
 * sceKernelAllocPartitionMemory, sceKernelGetBlockHeadAddr, sceKernelFreePartitionMemory
 */

#include "sceSysMem.h"
#include "mips_cpu.h"
#include "mips_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HLE_MemBlock g_mem_blocks[MAX_MEM_BLOCKS] = {0};
int g_next_block_id = 1;
uint32_t g_next_partition_alloc_addr = 0x09000000;

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
        cpu->gpr[0x02] = 0x800200D6; // SCE_KERNEL_ERROR_UNKNOWN_UID (ou codigo de erro equivalente que voce ja usa em outro lugar)
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
