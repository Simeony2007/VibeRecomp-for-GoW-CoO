#include "mips_dispatcher.h"
#include <stdlib.h>
#include <string.h>

int mips_dispatcher_init(MIPS_Dispatcher *disp) {
    if (disp == NULL) {
        return -1;
    }

    // Determine number of elements in table (64MB / 4 bytes alignment = 16M elements)
    uint32_t elements = PSP_RAM_SIZE >> 2;

    // Allocate 128MB flat direct-mapped pointer table
    disp->table = (AOTBlockFunction*)calloc(elements, sizeof(AOTBlockFunction));
    if (disp->table == NULL) {
        return -2;
    }

    disp->fallback_interpreter = NULL; // Must be bound by the interpreter module later
    return 0;
}

void mips_dispatcher_free(MIPS_Dispatcher *disp) {
    if (disp == NULL) return;

    if (disp->table) {
        free(disp->table);
        disp->table = NULL;
    }
}

void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu) {
    if (disp == NULL || cpu == NULL) return;

    while (__builtin_expect(!cpu->exit_requested, 1)) {
        AOTBlockFunction func = mips_dispatcher_lookup(disp, cpu->pc);
        
        if (__builtin_expect(func != NULL, 1)) {
            // Executing pre-compiled block or interpreter fallback
            func(cpu);
        } else {
            // To prevent spinlocking, raise bad PC register error and stop
            cpu->bad_vaddr = cpu->pc;
            cpu->exit_requested = true;
            break;
        }
    }
}
