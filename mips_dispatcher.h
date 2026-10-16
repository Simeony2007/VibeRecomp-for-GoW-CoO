#ifndef MIPS_DISPATCHER_H
#define MIPS_DISPATCHER_H

#include "mips_cpu.h"
#include <stddef.h>

/*
 * The MIPS AOT Dispatcher
 * This is the heart of the execution loop in our AOT engine.
 */
typedef void (*AOTBlockFunction)(MIPS_CPU *cpu);
typedef void (*MIPS_DispatcherService)(MIPS_CPU *cpu);

/* Returns true for values that are known data sentinels, never code targets. */
bool mips_aot_indirect_target_is_data(uint32_t target);
void mips_aot_trace_control_window(MIPS_CPU *cpu, uint32_t relative_pc);

/*
 * The generated AOT image is specific to the GOW executable.  Its blocks are
 * registered by address, so another PSP image loaded at an overlapping
 * address could otherwise execute unrelated translated code.
 */
static inline bool mips_dispatcher_aot_image_matches(MIPS_CPU *cpu) {
    static const uint32_t signature[] = {
        0x10000003, 0x34840001, 0x2405FFFE, 0x00852024, 0xAE440138
    };
    const uint32_t signature_addr = PSP_USER_LOAD_BASE + 0x001000E8;
    if (cpu == NULL) return false;
    for (size_t i = 0; i < sizeof(signature) / sizeof(signature[0]); i++) {
        if (mips_read32(cpu, signature_addr + (uint32_t)(i * sizeof(uint32_t))) !=
            signature[i]) {
            return false;
        }
    }
    return true;
}

typedef struct {
    // A flat pointer lookup array covering the entire 64MB RAM virtual range [0x08000000 - 0x0BFFFFFF].
    // Shifted right by 2 to account for 4-byte alignment.
    AOTBlockFunction *table;

    // A fallback function pointer pointing to the Interpreter's entry point
    AOTBlockFunction fallback_interpreter;
    MIPS_DispatcherService service_callback;
} MIPS_Dispatcher;

int mips_dispatcher_init(MIPS_Dispatcher *disp);
void mips_dispatcher_free(MIPS_Dispatcher *disp);

// Register a compiled AOT block function at a specific PSP virtual address
static inline void mips_dispatcher_register(MIPS_Dispatcher *disp, uint32_t addr, AOTBlockFunction func) {
    if (disp == NULL || disp->table == NULL || func == NULL) {
        return;
    }
    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // Relocatable Load Bias Virtualization:
    // PPSSPP loads relocatable PSP modules at the default user-RAM base 0x08804000.
    // Keep the AOT registry aligned to that base, otherwise translated code lands 0x4000
    // away from the code the game actually loaded and the GE matrices diverge.
    if (physical_addr < 0x04000000) {
        physical_addr += PSP_USER_LOAD_BASE;
    }

    if (physical_addr >= 0x08000000 && physical_addr < 0x0C000000) {
        uint32_t index = (physical_addr & PSP_RAM_MASK) >> 2;
        disp->table[index] = func;
    }
}

// Get the compiled function for a given virtual Program Counter (PC).
static inline AOTBlockFunction mips_dispatcher_lookup(MIPS_Dispatcher *disp, uint32_t addr) {
    if (disp == NULL) {
        return NULL;
    }

    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // Relocatable Load Bias Virtualization:
    // Align the virtualized lookup address with the actual PSP load base used by PPSSPP,
    // which starts at 0x08804000 for executable modules in user RAM.
    if (physical_addr < 0x04000000) {
        physical_addr += PSP_USER_LOAD_BASE;
    }

    if (__builtin_expect(physical_addr >= 0x08000000 && physical_addr < 0x0C000000, 1)) {
        uint32_t index = (physical_addr & PSP_RAM_MASK) >> 2;
        if (disp->table != NULL) {
            AOTBlockFunction func = disp->table[index];
            if (__builtin_expect(func != NULL, 1)) {
                return func;
            }
        }

        /*
         * A valid instruction address is not required to have an AOT block.
         * Returning the interpreter here is important for sparse/partial
         * recompiler output (and avoids treating a normal fallback as a bad
         * PC in the dispatcher loop).
        */
        return disp->fallback_interpreter;
    }
    return NULL;
}

// Main execution loop: drives the CPU using the registered AOT blocks
void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu);

#endif // MIPS_DISPATCHER_H