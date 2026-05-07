#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../out/func_table.h"
#include "hle_router.h"

extern uint32_t saved_main_pc;
extern MIPS_CPU main_thread_snapshot;

int compare_addrs(const void *a, const void *b) {
    uint32_t addr_a = *(uint32_t*)a;
    uint32_t addr_b = *(uint32_t*)b;
    return (addr_a < addr_b) ? -1 : (addr_a > addr_b);
}

static uint32_t find_dispatch_address(uint32_t target) {
    if ((target & 3u) != 0) target &= ~3u;
    uint32_t *item = bsearch(&target, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
    if (item) return target;
    
    // Fallback para blocos internos
    uint32_t fallback = target;
    while (fallback >= 4 && fallback > 0x08000000) { 
        fallback -= 4;
        item = bsearch(&fallback, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
        if (item) return fallback;
    }
    return 0;
}

void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {
    // 1. Gestão de threads via Sentinel RA
    if (target == 0x0FFFFFFF) {
        printf("[HLE] Retorno de Thread detectado. Destruindo thread atual...\n");
        
        // Chama o NOVO escalonador para matar a Thread 2 e voltar pra Thread 1
        extern void scheduler_exit_thread(MIPS_CPU *cpu);
        scheduler_exit_thread(cpu);
        
        // O escalonador alterou o cpu->pc internamente para retomar a Thread 1.
        // Damos um return para o main.c chamar o dispatcher novamente com o PC correto!
        return; 
    }

    /* --- LÓGICA DE SOMA (Auto-Relocação de Offsets) --- */
    if (target > 0 && target < 0x08000000) {
        target += 0x08800000; 
    }

    /* --- MEMORY HOOK: PROTEÇÃO CONTRA PONTEIROS NÃO REALOCADOS --- */
    if (target == 0x464C457F) {
        // O MIPS tentou ler o cabeçalho ELF devido à falta de PRX Relocation.
        // Simulamos o retorno para quebrar o loop do iterador.
        cpu->pc = cpu->ra; 
        return;
    }

    /* --- PC TRACE --- */
    // Agora o print vem ANTES do intercept!
    printf("[TRACE] Executando bloco: 0x%08X | RA: 0x%08X\n", target, cpu->ra);

    /* --- INTERCEPTAÇÃO HLE ABSOLUTA --- */
    uint32_t pc_before_hle = cpu->pc; // Guarda o PC antes de chamar a função do sistema

    if (intercept_hle(cpu, mem, target)) {
        // Se a syscall HLE NÃO mudou o PC (como CreateThread, SetSDK, IoOpen), 
        // significa que foi só uma chamada de sistema normal. 
        // Então nós simulamos o retorno do Stub (voltando pro RA).
        if (cpu->pc == pc_before_hle) {
            cpu->pc = cpu->ra; 
        } 
        // Se a syscall MUDOU o PC (como o StartThread fez), nós NÃO encostamos!
        // Deixamos o PC seguir o fluxo para a nova Thread.
        
        return;
    }

    // 2. Execução
    uint32_t mapped = find_dispatch_address(target);
    if (mapped != 0) {
        uint32_t *item = bsearch(&mapped, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
        int index = item - psp_func_addrs;
        psp_func_table[index](cpu, mem);
    } else {
        printf("[ERRO] PC corrompido ou fora de alcance: 0x%08X (RA: 0x%08X)\n", target, cpu->ra);
        cpu->running = 0;
    }
}