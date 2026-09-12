#ifndef SCE_SYSMEM_H
#define SCE_SYSMEM_H

#include "mips_cpu.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_MEM_BLOCKS 64

typedef struct {
    int id;
    char name[32];
    uint32_t partition_id;
    uint32_t type;
    uint32_t size;
    uint32_t addr;
    bool is_active;
} HLE_MemBlock;

extern HLE_MemBlock g_mem_blocks[MAX_MEM_BLOCKS];
extern int g_next_block_id;
extern uint32_t g_next_partition_alloc_addr;

// HLE Syscall Handlers for SysMemUserForUser
void hle_sceKernelTotalFreeMemSize(MIPS_CPU *cpu);
void hle_sceKernelMaxFreeMemSize(MIPS_CPU *cpu);
void hle_sceKernelAllocPartitionMemory(MIPS_CPU *cpu);
void hle_sceKernelGetBlockHeadAddr(MIPS_CPU *cpu);
void hle_sceKernelFreePartitionMemory(MIPS_CPU *cpu);

#endif // SCE_SYSMEM_H
