#ifndef MIPS_CPU_H
#define MIPS_CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MIPS_POSTMORTEM_SIZE 64

typedef struct {
    uint32_t pc;
    uint32_t next_pc;
    uint32_t inst;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t bad_vaddr;
} MIPS_PostMortemEntry;

/*
 * PSP Memory Map Constants
 */
#define PSP_RAM_SIZE        (64 * 1024 * 1024)   // 64MB (Slim RAM capacity to support God of War)
#define PSP_VRAM_SIZE       (2 * 1024 * 1024)    // 2MB VRAM
#define PSP_SCRATCH_SIZE    (16 * 1024)          // 16KB Scratchpad
#define PSP_USER_LOAD_BASE  0x08804000U

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
    uint32_t last_fault_pc;
    uint32_t last_fault_inst;
    uint32_t last_fault_access;
    uint32_t memory_fault_count;
    uint32_t trace_current_pc;
    uint32_t trace_current_inst;
    uint32_t trace_next_pc_override_value;
    bool trace_enabled;
    bool trace_memory_enabled;
    bool trace_next_pc_override;
    uint32_t scheduler_saved_v0;
    bool scheduler_saved_v0_valid;
    MIPS_PostMortemEntry postmortem[MIPS_POSTMORTEM_SIZE];
    uint32_t postmortem_next;
    uint64_t postmortem_total;
} MIPS_CPU;

// Initialize the CPU state, allocating RAM, VRAM, and Scratchpad.
int mips_cpu_init(MIPS_CPU *cpu);

// Free all resources allocated for the CPU.
void mips_cpu_free(MIPS_CPU *cpu);

// Reset registers and pointers (without freeing memory blocks).
void mips_cpu_reset(MIPS_CPU *cpu);
void mips_trace_init(MIPS_CPU *cpu);
void mips_trace_instruction(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc, uint32_t inst);
void mips_trace_instruction_at(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc);
void mips_trace_hle(MIPS_CPU *cpu, uint32_t caller_pc, uint32_t syscall_id,
                    const uint32_t args[4], uint32_t result);
void mips_trace_memory_write(MIPS_CPU *cpu, uint32_t address, uint32_t width,
                             uint32_t old_value, uint32_t value);
void mips_trace_memory_read(MIPS_CPU *cpu, uint32_t address, uint32_t width,
                            uint32_t value);
void mips_trace_memory_fault(MIPS_CPU *cpu, uint32_t address, uint32_t access);

/*
 * Fast Memory Translation Helpers (ARM64 optimized)
 * Avoids complex translation tables. Uses fast range checks and direct pointer arithmetic.
 */
static inline void* mips_get_ptr(MIPS_CPU *cpu, uint32_t addr) {
    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // 1. Resolve o Scratchpad PRIMEIRO (Evita que o bias envie acessos para a RAM executável)
    if (physical_addr >= 0x00010000 && physical_addr < 0x00014000) {
        return (void*)(cpu->scratchpad + (physical_addr & PSP_SCRATCH_MASK));
    }

    // 2. Resolve the physical VRAM and its three PSP memory-map mirrors.
    if (physical_addr >= 0x04000000 && physical_addr < 0x04800000) {
        return (void*)(cpu->vram + (physical_addr & PSP_VRAM_MASK));
    }

    // 3. Resolve RAM principal apenas por enderecos virtuais PSP validos.
    if (physical_addr >= 0x08000000 && physical_addr < 0x0C000000) {
        return (void*)(cpu->ram + (physical_addr & PSP_RAM_MASK));
    }

    return NULL;
}

/*
 * Translate a contiguous range.  mips_get_ptr() intentionally only checks
 * the first byte; callers such as file I/O also need to ensure that a buffer
 * does not run past the end of an emulated memory bank.
 */
static inline void* mips_get_ptr_range(MIPS_CPU *cpu, uint32_t addr, size_t size) {
    uint32_t physical_addr = addr & 0x0FFFFFFF;
    uint64_t end = (uint64_t)physical_addr + (uint64_t)size;

    if (physical_addr >= 0x00010000 && physical_addr < 0x00014000) {
        if (end <= 0x00014000) {
            return (void*)(cpu->scratchpad + (physical_addr & PSP_SCRATCH_MASK));
        }
        return NULL;
    }

    if (physical_addr >= 0x04000000 && physical_addr < 0x04800000) {
        uint64_t mirror_end =
            (uint64_t)(physical_addr & ~(uint32_t)PSP_VRAM_MASK) +
            PSP_VRAM_SIZE;
        if (end <= mirror_end) {
            return (void*)(cpu->vram + (physical_addr & PSP_VRAM_MASK));
        }
        return NULL;
    }

    if (physical_addr >= 0x08000000 && physical_addr < 0x0C000000 &&
        end <= 0x0C000000) {
        return (void*)(cpu->ram + (physical_addr & PSP_RAM_MASK));
    }

    return NULL;
}


// Inline read/write macros for absolute maximum compiler optimization
static inline uint32_t mips_read32(MIPS_CPU *cpu, uint32_t addr) {
    uint32_t *ptr = (uint32_t*)mips_get_ptr_range(cpu, addr, sizeof(uint32_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        uint32_t value = *ptr;
        if (cpu->trace_memory_enabled && addr != cpu->trace_current_pc) {
            mips_trace_memory_read(cpu, addr, 4, value);
        }
        return value;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled
        ? cpu->trace_current_inst
        : (mips_get_ptr_range(cpu, cpu->pc, sizeof(uint32_t)) != NULL
            ? *(uint32_t *)mips_get_ptr_range(cpu, cpu->pc, sizeof(uint32_t))
            : 0);
    cpu->last_fault_access = 0x100 | sizeof(uint32_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
    return 0;
}

static inline uint16_t mips_read16(MIPS_CPU *cpu, uint32_t addr) {
    uint16_t *ptr = (uint16_t*)mips_get_ptr_range(cpu, addr, sizeof(uint16_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        uint16_t value = *ptr;
        if (cpu->trace_memory_enabled) mips_trace_memory_read(cpu, addr, 2, value);
        return value;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled ? cpu->trace_current_inst : 0;
    cpu->last_fault_access = 0x100 | sizeof(uint16_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
    return 0;
}

static inline uint8_t mips_read8(MIPS_CPU *cpu, uint32_t addr) {
    uint8_t *ptr = (uint8_t*)mips_get_ptr_range(cpu, addr, sizeof(uint8_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        uint8_t value = *ptr;
        if (cpu->trace_memory_enabled) mips_trace_memory_read(cpu, addr, 1, value);
        return value;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled ? cpu->trace_current_inst : 0;
    cpu->last_fault_access = 0x100 | sizeof(uint8_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
    return 0;
}

static inline void mips_write32(MIPS_CPU *cpu, uint32_t addr, uint32_t val) {
    uint32_t *ptr = (uint32_t*)mips_get_ptr_range(cpu, addr, sizeof(uint32_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        if (cpu->trace_memory_enabled) {
            uint32_t old_value = *ptr;
            *ptr = val;
            mips_trace_memory_write(cpu, addr, 4, old_value, val);
        } else {
            *ptr = val;
        }
        return;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled ? cpu->trace_current_inst : 0;
    cpu->last_fault_access = 0x200 | sizeof(uint32_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
}

static inline void mips_write16(MIPS_CPU *cpu, uint32_t addr, uint16_t val) {
    uint16_t *ptr = (uint16_t*)mips_get_ptr_range(cpu, addr, sizeof(uint16_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        if (cpu->trace_memory_enabled) {
            uint16_t old_value = *ptr;
            *ptr = val;
            mips_trace_memory_write(cpu, addr, 2, old_value, val);
        } else {
            *ptr = val;
        }
        return;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled ? cpu->trace_current_inst : 0;
    cpu->last_fault_access = 0x200 | sizeof(uint16_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
}

static inline void mips_write8(MIPS_CPU *cpu, uint32_t addr, uint8_t val) {
    uint8_t *ptr = (uint8_t*)mips_get_ptr_range(cpu, addr, sizeof(uint8_t));
    if (__builtin_expect(ptr != NULL, 1)) {
        if (cpu->trace_memory_enabled) {
            uint8_t old_value = *ptr;
            *ptr = val;
            mips_trace_memory_write(cpu, addr, 1, old_value, val);
        } else {
            *ptr = val;
        }
        return;
    }
    cpu->bad_vaddr = addr;
    cpu->last_fault_pc = cpu->trace_enabled ? cpu->trace_current_pc : cpu->pc;
    cpu->last_fault_inst = cpu->trace_enabled ? cpu->trace_current_inst : 0;
    cpu->last_fault_access = 0x200 | sizeof(uint8_t);
    cpu->memory_fault_count++;
    if (cpu->trace_enabled) mips_trace_memory_fault(cpu, addr, cpu->last_fault_access);
}

void mips_postmortem_record(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc);
void mips_postmortem_dump(const MIPS_CPU *cpu, const char *reason);

#endif // MIPS_CPU_H