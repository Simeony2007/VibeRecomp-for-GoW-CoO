#ifndef MIPS_CPU_H
#define MIPS_CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * PSP Memory Map Constants
 */
#define PSP_RAM_SIZE        (64 * 1024 * 1024)   // 64MB (Slim RAM capacity to support God of War)
#define PSP_VRAM_SIZE       (2 * 1024 * 1024)    // 2MB VRAM
#define PSP_SCRATCH_SIZE    (16 * 1024)          // 16KB Scratchpad

#define PSP_RAM_MASK        (PSP_RAM_SIZE - 1)
#define PSP_VRAM_MASK       (PSP_VRAM_SIZE - 1)
#define PSP_SCRATCH_MASK    (PSP_SCRATCH_SIZE - 1)

/*
 * MIPS Allegrex CPU State
 * Tailored for ultra-high performance static recompilation on ARM64.
 * Both PSP (MIPS Little-Endian) and RK3326 (ARM64 Little-Endian) share the same endianness.
 * This allows direct memory casting without byte-swapping overhead!
 */
typedef struct {
    // General Purpose Registers (GPR). r0 is hardwired to 0.
    // Aligned to 16 bytes for vector/SIMD operations on ARM64 if needed.
    uint32_t gpr[32] __attribute__ ((aligned(16)));

    uint32_t pc;       // Program Counter
    uint32_t hi;       // Multiplication/Division High Register
    uint32_t lo;       // Multiplication/Division Low Register

    // Floating Point Unit (FPU - Coprocessor 1)
    float fpr[32] __attribute__ ((aligned(16)));
    uint32_t fcr31;    // FPU Control/Status Register

    // Vector Floating Point Unit (VFPU - Coprocessor 2)
    // The PSP's unique VFPU consists of 128 32-bit floats.
    // It can be addressed as matrices (4x4), vectors (4), or scalars.
    // A flat array is optimal for direct compiler mapping and memory efficiency.
    float vfpu[128] __attribute__ ((aligned(16)));
    uint32_t vcr[4];   // VFPU Control Registers (VCC, etc.)

    // Coprocessor 0 (COP0) Control Registers & Custom Allegrex State
    // Used for low-level system configuration and hardware control parameters
    uint32_t cop0[32];     // COP0 Control registers
    uint32_t ic_state;     // Interrupt Controller state (used by mfic/mtic)

    // Memory Base Pointers
    // In our AOT engine, we allocate contiguous virtual blocks for absolute speed.
    uint8_t *ram;          // Pointer to 64MB allocated RAM
    uint8_t *vram;         // Pointer to 2MB allocated VRAM
    uint8_t *scratchpad;   // Pointer to 16KB Scratchpad

    // Emulator Control State
    uint64_t cycles;       // Executed cycle counter
    bool exit_requested;   // Flag to request CPU thread termination
    uint32_t next_pc;      // Target PC for branch delay-slot resolution

    // Debug & Exception Handling
    uint32_t bad_vaddr;    // Coprocessor 0 BadVAddr (for memory fault diagnostics)
} MIPS_CPU;

// Initialize the CPU state, allocating RAM, VRAM, and Scratchpad.
int mips_cpu_init(MIPS_CPU *cpu);

// Free all resources allocated for the CPU.
void mips_cpu_free(MIPS_CPU *cpu);

// Reset registers and pointers (without freeing memory blocks).
void mips_cpu_reset(MIPS_CPU *cpu);

/*
 * Fast Memory Translation Helpers (ARM64 optimized)
 * Avoids complex translation tables. Uses fast range checks and direct pointer arithmetic.
 */
static inline void* mips_get_ptr(MIPS_CPU *cpu, uint32_t addr) {
    // Fold cached (0x08000000), uncached (0x48000000), and kernel (0x88000000) addresses
    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // Relocatable Load Bias Virtualization:
    // If the binary tries to access virtual address spaces starting at 0x00000000,
    // dynamically virtualize and shift them directly into the emulated user RAM base space (0x08800000).
    if (physical_addr < 0x04000000) {
        physical_addr += 0x08800000;
    }

    // RAM Range: 0x08000000 - 0x0BFFFFFF (64MB)
    if (physical_addr >= 0x08000000 && physical_addr < 0x0C000000) {
        return (void*)(cpu->ram + (physical_addr & PSP_RAM_MASK));
    }
    // VRAM Range: 0x04000000 - 0x041FFFFF (2MB)
    if (physical_addr >= 0x04000000 && physical_addr < 0x04200000) {
        return (void*)(cpu->vram + (physical_addr & PSP_VRAM_MASK));
    }
    // Scratchpad Range: 0x00010000 - 0x00013FFF (16KB)
    if (physical_addr >= 0x00010000 && physical_addr < 0x00014000) {
        return (void*)(cpu->scratchpad + (physical_addr & PSP_SCRATCH_MASK));
    }

    // Fallback/Invalid Address
    return NULL;
}

// Inline read/write macros for absolute maximum compiler optimization
static inline uint32_t mips_read32(MIPS_CPU *cpu, uint32_t addr) {
    uint32_t *ptr = (uint32_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        return *ptr;
    }
    cpu->bad_vaddr = addr;
    return 0;
}

static inline uint16_t mips_read16(MIPS_CPU *cpu, uint32_t addr) {
    uint16_t *ptr = (uint16_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        return *ptr;
    }
    cpu->bad_vaddr = addr;
    return 0;
}

static inline uint8_t mips_read8(MIPS_CPU *cpu, uint32_t addr) {
    uint8_t *ptr = (uint8_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        return *ptr;
    }
    cpu->bad_vaddr = addr;
    return 0;
}

static inline void mips_write32(MIPS_CPU *cpu, uint32_t addr, uint32_t val) {
    uint32_t *ptr = (uint32_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        *ptr = val;
        return;
    }
    cpu->bad_vaddr = addr;
}

static inline void mips_write16(MIPS_CPU *cpu, uint32_t addr, uint16_t val) {
    uint16_t *ptr = (uint16_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        *ptr = val;
        return;
    }
    cpu->bad_vaddr = addr;
}

static inline void mips_write8(MIPS_CPU *cpu, uint32_t addr, uint8_t val) {
    uint8_t *ptr = (uint8_t*)mips_get_ptr(cpu, addr);
    if (__builtin_expect(ptr != NULL, 1)) {
        *ptr = val;
        return;
    }
    cpu->bad_vaddr = addr;
}

#endif // MIPS_CPU_H