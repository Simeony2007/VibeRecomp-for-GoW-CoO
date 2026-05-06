#include "cpu.h"
#include <stdio.h>
#include <string.h>

// Variáveis de controle de Threads migradas do dispatcher
static uint32_t main_thread_pc = 0;
static int thread_count = 0;
static int started_threads = 0;

void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    // Log limpo no terminal para vermos a máquina funcionando!
    fprintf(stderr, "[SYSCALL] %s | a0=0x%08X a1=0x%08X\n", name, cpu->a0, cpu->a1);

    if (strcmp(name, "sceKernelCreateThread") == 0) {
        uint32_t entry_point = cpu->a1;
        if (entry_point < 0x08000000) entry_point += 0x08800000;
        
        thread_count++;
        if (thread_count == 1) {
            main_thread_pc = entry_point;
            printf("[HLE] -> Endereco do Motor do GOW sequestrado: 0x%08X\n", main_thread_pc);
        }
        cpu->v0 = thread_count; // Retorna um ID fake da Thread
    }
    else if (strcmp(name, "sceKernelStartThread") == 0) {
        started_threads++;
        if (started_threads == 1) {
            printf("[HLE] Acordando o Kratos (Pulo Absoluto)...\n\n");
            cpu->v0 = 0;
            cpu->pc = main_thread_pc; // AQUI A SYSCALL MUDA O PC E ROUBA O FLUXO!
        } else {
            cpu->v0 = 0; // Outras threads não roubam o PC
        }
    }
    else if (strcmp(name, "sceIoDread") == 0) {
        cpu->v0 = 0; // Finge que a leitura do diretório acabou
    }
    else if (strcmp(name, "sceKernelSetCompiledSdkVersion370") == 0) {
        cpu->v0 = 0; // Apenas sorrimos e acenamos
    }
    // E a partir daqui você pode adicionar INFINITAS Syscalls facilmente!
}

void psp_syscall_mips(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    // O motor do jogo usa isto pontualmente para debug ou breakpoints (ex: BREAK).
    // Podemos apenas ignorar ou imprimir silenciosamente.
    // fprintf(stderr, "[MIPS HARDWARE] Syscall nativa interceptada: 0x%05X\n", code);
}