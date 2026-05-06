#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include "../out/func_table.h"

// Funções de busca binária intocadas...
int compare_addrs(const void *a, const void *b) {
    uint32_t addr_a = *(uint32_t*)a;
    uint32_t addr_b = *(uint32_t*)b;
    return (addr_a < addr_b) ? -1 : (addr_a > addr_b);
}

static uint32_t find_dispatch_address(uint32_t target) {
    if ((target & 3u) != 0) target &= ~3u;
    uint32_t *item = bsearch(&target, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
    if (item) return target;
    uint32_t fallback = target;
    while (fallback >= 4) {
        fallback -= 4;
        item = bsearch(&fallback, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
        if (item) return fallback;
    }
    return 0;
}

void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {
    
    // 1. O "Colete à Prova de Balas" contra tentativa de executar arquivo na RAM
    if (target == 0x464C457F || target == 0x7F454C46) {
        cpu->pc = cpu->ra;
        return;
    }

    // 2. O Desbloqueio do Spinlock (Injeção na Memória que fizemos no último passo)
    if (target == 0x0899F498) {
        uint32_t lock_addr = (cpu->s1 - 6864) & 0x01FFFFFF;
        mem[lock_addr] = 0; 
    }

    // 3. A Auto-Relocação Dinâmica
    if (target > 0 && target < 0x08000000) {
        target += 0x08800000;
    }

    // Adicione o Heartbeat aqui!
    static uint64_t heartbeat = 0;
    heartbeat++;
    if (heartbeat % 1000000 == 0) {
        printf("[HEARTBEAT] Kratos preso no PC: 0x%08X\n", target);
    }

    // 4. Execução Pura e Limpa!
    uint32_t mapped = find_dispatch_address(target);
    if (mapped != 0) {
        uint32_t *item = bsearch(&mapped, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
        int index = item - psp_func_addrs;
        psp_func_table[index](cpu, mem);
    } else {
        printf("[ERRO] Salto para endereco nao descompilado: 0x%08X\n", target);
        cpu->running = 0;
    }
}