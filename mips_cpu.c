#include "mips_cpu.h"
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif

int mips_cpu_init(MIPS_CPU *cpu) {
    if (cpu == NULL) {
        return -1;
    }

    // Initialize all fields to 0
    memset(cpu, 0, sizeof(MIPS_CPU));

    // Allocate RAM (64MB)
    // Using posix_memalign or aligned_alloc ensures ARM64 hardware vectors (NEON)
    // can load/store values with optimal performance.
    int ret;
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->ram = (uint8_t*)_aligned_malloc(PSP_RAM_SIZE, 4096);
    ret = (cpu->ram == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->ram, 4096, PSP_RAM_SIZE);
#endif
    if (ret != 0) {
        return -2;
    }
    memset(cpu->ram, 0, PSP_RAM_SIZE);

    // Allocate VRAM (2MB)
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->vram = (uint8_t*)_aligned_malloc(PSP_VRAM_SIZE, 4096);
    ret = (cpu->vram == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->vram, 4096, PSP_VRAM_SIZE);
#endif
    if (ret != 0) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
#else
        free(cpu->ram);
#endif
        cpu->ram = NULL;
        return -3;
    }
    memset(cpu->vram, 0, PSP_VRAM_SIZE);

    // Allocate Scratchpad (16KB)
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->scratchpad = (uint8_t*)_aligned_malloc(PSP_SCRATCH_SIZE, 4096);
    ret = (cpu->scratchpad == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->scratchpad, 4096, PSP_SCRATCH_SIZE);
#endif
    if (ret != 0) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
        _aligned_free(cpu->vram);
#else
        free(cpu->ram);
        free(cpu->vram);
#endif
        cpu->ram = NULL;
        cpu->vram = NULL;
        return -4;
    }
    memset(cpu->scratchpad, 0, PSP_SCRATCH_SIZE);

    // Initial reset of register values
    mips_cpu_reset(cpu);

    return 0;
}

void mips_cpu_free(MIPS_CPU *cpu) {
    if (cpu == NULL) return;

    if (cpu->ram) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
#else
        free(cpu->ram);
#endif
        cpu->ram = NULL;
    }
    if (cpu->vram) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->vram);
#else
        free(cpu->vram);
#endif
        cpu->vram = NULL;
    }
    if (cpu->scratchpad) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->scratchpad);
#else
        free(cpu->scratchpad);
#endif
        cpu->scratchpad = NULL;
    }
}

void mips_cpu_reset(MIPS_CPU *cpu) {
    if (cpu == NULL) return;

    // Reset GPR, FPU and VFPU to 0 (except memory pointers, cycles and exit flag)
    memset(cpu->gpr, 0, sizeof(cpu->gpr));
    memset(cpu->fpr, 0, sizeof(cpu->fpr));
    memset(cpu->vfpu, 0, sizeof(cpu->vfpu));
    memset(cpu->vcr, 0, sizeof(cpu->vcr));
    memset(cpu->cop0, 0, sizeof(cpu->cop0));
    
    cpu->pc = 0x08804000; // Standard PSP EBOOT entry point virtual address
    cpu->next_pc = 0x08804000;
    cpu->hi = 0;
    cpu->lo = 0;
    cpu->fcr31 = 0;
    cpu->ic_state = 0;
    cpu->bad_vaddr = 0;
    cpu->cycles = 0;
    cpu->exit_requested = false;

    // Ensure R0 is always 0
    cpu->gpr[0] = 0;
}
