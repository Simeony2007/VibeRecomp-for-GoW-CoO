#include "cpu.h"
#include <stdlib.h>
#include "../out/func_table.h"

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
    
    printf("[DISPATCH] Executando em 0x%08X\n", target);
    printf("[DISPATCH] PC: 0x%08X | SP: 0x%08X\n", target, cpu->sp);

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