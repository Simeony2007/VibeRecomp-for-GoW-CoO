#include "cpu.h"
#include <stdlib.h>
#include "../out/func_table.h"

// Função auxiliar para comparação na busca binária
int compare_addrs(const void *a, const void *b) {
    uint32_t addr_a = *(uint32_t*)a;
    uint32_t addr_b = *(uint32_t*)b;
    return (addr_a < addr_b) ? -1 : (addr_a > addr_b);
}

void cpu_dispatch(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {
    
    printf("[DISPATCH] Executando em 0x%08X\n", target);
    printf("[DISPATCH] PC: 0x%08X | SP: 0x%08X\n", target, cpu->sp);
    // Busca binária na tabela de endereços
    uint32_t *item = bsearch(&target, psp_func_addrs, PSP_FUNC_COUNT, 
                             sizeof(uint32_t), compare_addrs);

    if (item) {
        // Calcula o índice baseado no ponteiro retornado
        int index = item - psp_func_addrs;
        psp_func_table[index](cpu, mem);
    } else {
        printf("[ERRO] Salto para endereço não descompilado: 0x%08X\n", target);
        cpu->running = 0;
    }
}