#include "mips_dispatcher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sceKernelThread.h"

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

extern uint32_t g_binary_segment_start;
extern uint32_t g_binary_segment_end;

void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu) {
    if (disp == NULL || cpu == NULL) return;

    while (__builtin_expect(!cpu->exit_requested, 1)) {

        // Sentinelas de controle de fluxo que NAO sao codigo real (ver sceKernelThread.c/.h
        // e main_v35.c::interpreter_step, que trata cada um deles ANTES de cair no
        // interpretador de verdade):
        //   0x08000100 -> "thread terminou" (return address trap)
        //   0x08000200 -> CB_RETURN_HACK_ADDR, trampolim de retorno de callback
        //   0x08000300 -> endereco de idle do scheduler (nenhuma thread READY)
        // Os tres precisam ficar fora da checagem de limites, senao qualquer callback
        // que retorna ou qualquer momento de idle e' confundido com um salto para
        // memoria invalida.
        if (cpu->pc != 0x08000100 &&
            cpu->pc != CB_RETURN_HACK_ADDR &&
            cpu->pc != 0x08000300 &&
            (cpu->pc < g_binary_segment_start || cpu->pc >= g_binary_segment_end)) {
            printf("[HLE EXCEPTION INTERCEPTED] CPU saltou para fora dos limites legitimos!\n");
            printf("  -> Limites legitimos do binario: 0x%08X - 0x%08X\n", g_binary_segment_start, g_binary_segment_end);
            printf("  -> PC invalido: 0x%08X\n", cpu->pc);
            cpu->bad_vaddr = cpu->pc;
            cpu->exit_requested = true;
            break;
        }

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
