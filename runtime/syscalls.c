/**
 * runtime/syscalls.c
 * Stubs das syscalls do PSP (sceKernel, sceGe, sceDisplay, etc.)
 *
 * COMO FUNCIONA:
 * No PSP, syscalls são chamadas via instrução SYSCALL com um código de 20 bits.
 * O código real mapeia para funções da firmware Sony (sceXxx).
 *
 * Para o recomp funcionar no R36S, cada syscall precisa ser reimplementada
 * usando as APIs disponíveis no Linux/Android do R36S (SDL2, OpenGL ES, etc.)
 *
 * Por enquanto, todas as syscalls têm stubs que apenas logam a chamada.
 * Implemente as que forem necessárias conforme o jogo exigir.
 *
 * REFERÊNCIA DE SYSCALLS DO PSP:
 *   https://uofw.github.io/upspd/docs/software/PSP_OS/
 */

#include "cpu.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>

/* ── Tabela de nomes (para debugging) ───────────────────────────────────── */
typedef struct {
    uint32_t    code;
    const char *name;
} SyscallEntry;

/* Códigos mais comuns do God of War: Chains of Olympus */
static const SyscallEntry SYSCALL_TABLE[] = {
    /* sceKernelLibc */
    { 0x2150,  "sceKernelLibcTime"         },
    { 0x2151,  "sceKernelLibcClock"        },
    /* sceDisplay */
    { 0x6000,  "sceDisplaySetMode"         },
    { 0x6001,  "sceDisplaySetFrameBuf"     },
    { 0x6002,  "sceDisplayWaitVblankStart" },
    { 0x6003,  "sceDisplayGetFrameBuf"     },
    /* sceGe_user (GPU commands) */
    { 0x4700,  "sceGeListEnQueue"          },
    { 0x4701,  "sceGeListSync"             },
    { 0x4702,  "sceGeDrawSync"             },
    /* sceCtrl (input) */
    { 0x5000,  "sceCtrlReadBufferPositive" },
    { 0x5001,  "sceCtrlSetSamplingCycle"   },
    { 0x5002,  "sceCtrlSetSamplingMode"    },
    /* sceAudio */
    { 0x8000,  "sceAudioChReserve"         },
    { 0x8001,  "sceAudioChRelease"         },
    { 0x8002,  "sceAudioOutputBlocking"    },
    /* sceKernelThread */
    { 0x3000,  "sceKernelCreateThread"     },
    { 0x3001,  "sceKernelStartThread"      },
    { 0x3002,  "sceKernelExitThread"       },
    { 0x3003,  "sceKernelSleepThread"      },
    { 0x3004,  "sceKernelDelayThread"      },
    /* sceKernelMemory */
    { 0x2000,  "sceKernelAllocPartitionMemory" },
    { 0x2001,  "sceKernelFreePartitionMemory"  },
    { 0x2002,  "sceKernelGetBlockHeadAddr"     },
    { 0,       NULL }
};

static const char *syscall_name(uint32_t code) {
    for (int i = 0; SYSCALL_TABLE[i].name; i++) {
        if (SYSCALL_TABLE[i].code == code)
            return SYSCALL_TABLE[i].name;
    }
    return "???";
}

/* ── Dispatcher principal ────────────────────────────────────────────────── */
void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    (void)mem;

    fprintf(stderr, "[SYSCALL] code=0x%05X (%s) a0=0x%08X a1=0x%08X\n",
            code, syscall_name(code), cpu->a0, cpu->a1);

    /* 
     * ── IMPLEMENTE AS SYSCALLS AQUI ──────────────────────────────────────
     *
     * Exemplo de como implementar sceDisplayWaitVblankStart com SDL2:
     *
     * case 0x6002:  // sceDisplayWaitVblankStart
     *     SDL_Delay(16);   // ~60fps
     *     cpu->v0 = 0;     // retorno = 0 (sucesso)
     *     return;
     *
     * Exemplo de sceCtrlReadBufferPositive:
     *
     * case 0x5000: {  // sceCtrlReadBufferPositive
     *     // a0 = ponteiro para SceCtrlData na memória do PSP
     *     // a1 = número de amostras
     *     uint32_t buf_vaddr = cpu->a0;
     *     SDL_PumpEvents();
     *     const uint8_t *keys = SDL_GetKeyboardState(NULL);
     *     uint32_t buttons = 0;
     *     if (keys[SDL_SCANCODE_X])     buttons |= 0x4000;  // Botão X
     *     if (keys[SDL_SCANCODE_Z])     buttons |= 0x2000;  // Círculo
     *     if (keys[SDL_SCANCODE_UP])    buttons |= 0x0010;  // D-pad up
     *     ...
     *     MEM_W32(mem, buf_vaddr + 4, buttons);
     *     cpu->v0 = 1;
     *     return;
     * }
     */

    switch (code) {
        /* Retorno padrão de sucesso para syscalls não implementadas */
        default:
            cpu->v0 = 0;
            break;
    }
}
