#ifndef MIPS_DISPATCHER_H
#define MIPS_DISPATCHER_H

#include "mips_cpu.h"
#include <stddef.h>

/*
 * The MIPS AOT Dispatcher
 * This is the heart of the execution loop in our AOT engine.
 */
typedef void (*AOTBlockFunction)(MIPS_CPU *cpu);

typedef struct {
    // A flat pointer lookup array covering the entire 64MB RAM virtual range [0x08000000 - 0x0BFFFFFF].
    // Shifted right by 2 to account for 4-byte alignment.
    AOTBlockFunction *table;

    // A fallback function pointer pointing to the Interpreter's entry point
    AOTBlockFunction fallback_interpreter;
} MIPS_Dispatcher;

int mips_dispatcher_init(MIPS_Dispatcher *disp);
void mips_dispatcher_free(MIPS_Dispatcher *disp);

// Register a compiled AOT block function at a specific PSP virtual address
static inline void mips_dispatcher_register(MIPS_Dispatcher *disp, uint32_t addr, AOTBlockFunction func) {
    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // Relocatable Load Bias Virtualization:
    // If the recompiled python JIT function was mapped starting at base offset 0x00000000,
    // dynamically virtualize and register it directly at the 0x08800000 RAM base offset!
    if (physical_addr < 0x04000000) {
        physical_addr += 0x08800000;
    }

    if (physical_addr >= 0x08000000 && physical_addr < 0x0C000000) {
        uint32_t index = (physical_addr & PSP_RAM_MASK) >> 2;
        disp->table[index] = func;
    }
}

// Get the compiled function for a given virtual Program Counter (PC).
static inline AOTBlockFunction mips_dispatcher_lookup(MIPS_Dispatcher *disp, uint32_t addr) {
    uint32_t physical_addr = addr & 0x0FFFFFFF;

    // Relocatable Load Bias Virtualization:
    // If the PC lookup address is pointing to un-relocated offset ranges below 0x04000000,
    // virtualize it to point back to the user RAM space.
    if (physical_addr < 0x04000000) {
        physical_addr += 0x08800000;
    }

    if (__builtin_expect(physical_addr >= 0x08000000 && physical_addr < 0x0C000000, 1)) {
        uint32_t index = (physical_addr & PSP_RAM_MASK) >> 2;
        AOTBlockFunction func = disp->table[index];
        if (__builtin_expect(func != NULL, 1)) {
            return func;
        }
    }
    return disp->fallback_interpreter;
}

// Main execution loop: drives the CPU using the registered AOT blocks
void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu);

#endif // MIPS_DISPATCHER_H