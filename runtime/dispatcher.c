#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include "../out/func_table.h"

// Variável estática para guardar o endereço real de onde o Kratos nasce
static uint32_t main_thread_pc = 0;

// Função auxiliar para comparação na busca binária
int compare_addrs(const void *a, const void *b) {
    uint32_t addr_a = *(uint32_t*)a;
    uint32_t addr_b = *(uint32_t*)b;
    return (addr_a < addr_b) ? -1 : (addr_a > addr_b);
}

static uint32_t find_dispatch_address(uint32_t target) {
    if ((target & 3u) != 0) {
        target &= ~3u;
    }

    uint32_t *item = bsearch(&target, psp_func_addrs, PSP_FUNC_COUNT,
                             sizeof(uint32_t), compare_addrs);
    if (item) {
        return target;
    }

    uint32_t fallback = target;
    while (fallback >= 4) {
        fallback -= 4;
        item = bsearch(&fallback, psp_func_addrs, PSP_FUNC_COUNT,
                        sizeof(uint32_t), compare_addrs);
        if (item) {
            return fallback;
        }
    }
    return 0;
}

void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {
    
    /* ═══════════════════════════════════════════════════════════════
     * AUTO-RELOCAÇÃO DINÂMICA (O COLETE À PROVA DE BALAS)
     * ═══════════════════════════════════════════════════════════════ */
    // Se o jogo tentar pular para um offset não realocado, consertamos na hora!
    if (target > 0 && target < 0x08000000) {
        target += 0x08800000;
    }

    /* ═══════════════════════════════════════════════════════════════
     * SEQUESTRO DE KERNEL (THREAD HIJACKING)
     * ═══════════════════════════════════════════════════════════════ */
    // 1. O jogo avisa a versão do SDK. Nós apenas sorrimos e acenamos.
    if (target == 0x08B01DF4) { 
        printf("\n[HLE] sceKernelSetCompiledSdkVersion interceptado! (Ignorando)\n");
        cpu->v0 = 0;          // Sucesso
        cpu->pc = cpu->ra;    // Volta pra função anterior
        return;
    }

    // 2. O jogo cria a Thread. O registrador 'a1' tem o endereço real do motor do GOW!
    if (target == 0x08B01D64) { 
        uint32_t entry_point = cpu->a1;
        
        // Se o endereço for um 'offset' não realocado (menor que a RAM do PSP), somamos a base
        if (entry_point < 0x08000000) {
            entry_point += 0x08800000;
        }
        
        main_thread_pc = entry_point; // ROUBAMOS O ENDEREÇO CORRETO!
        
        printf("[HLE] sceKernelCreateThread interceptado!\n");
        printf("[HLE] -> Endereco do Motor do GOW sequestrado e corrigido: 0x%08X\n", main_thread_pc);
        cpu->v0 = 1;          // Retorna 1 (Fingindo ser o ID da Thread)
        cpu->pc = cpu->ra;    // Volta pra inicialização
        return;
    }

    // 3. O jogo dá o comando de iniciar a Thread.
    if (target == 0x08B01D14) { 
        printf("[HLE] sceKernelStartThread interceptado! Acordando o Kratos...\n\n");
        cpu->v0 = 0;               // Sucesso
        cpu->pc = main_thread_pc;  // PULO ABSOLUTO PRO JOGO REAL!
        return;
    }

    /* ═══════════════════════════════════════════════════════════════
     * DISPATCHER NORMAL
     * ═══════════════════════════════════════════════════════════════ */

    // printf("[DISPATCH] Executando em 0x%08X\n", target);
    // printf("[DISPATCH] PC: 0x%08X | SP: 0x%08X\n", target, cpu->sp); // Comentado para limpar o log

    uint32_t mapped = find_dispatch_address(target);
    if (mapped != 0) {
        uint32_t *item = bsearch(&mapped, psp_func_addrs, PSP_FUNC_COUNT,
                                 sizeof(uint32_t), compare_addrs);
        // Calcula o índice baseado no ponteiro retornado
        int index = item - psp_func_addrs;
        psp_func_table[index](cpu, mem);
    } else {
        printf("[ERRO] Salto para endereço não descompilado: 0x%08X\n", target);
        cpu->running = 0;
    }
}