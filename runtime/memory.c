/**
 * runtime/memory.c
 * Aloca e gerencia o espaço de memória do PSP emulado.
 */

#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

uint8_t *mem_init(void) {
    uint8_t *mem = (uint8_t *)calloc(PSP_MEM_SIZE, 1);
    if (!mem) {
        fprintf(stderr, "[MEM] Falha ao alocar %u bytes\n", PSP_MEM_SIZE);
        exit(1);
    }
    printf("[MEM] %u MB de memória alocados\n", PSP_MEM_SIZE / (1024 * 1024));
    return mem;
}

void mem_load_segment(uint8_t *mem, const uint8_t *data,
                      uint32_t vaddr, uint32_t size) {
    uint32_t off = psp_translate_addr(vaddr);
    if (off + size > PSP_MEM_SIZE) {
        fprintf(stderr, "[MEM] Segmento 0x%08X+0x%X fora de range!\n", vaddr, size);
        return;
    }
    memcpy(mem + off, data, size);
    printf("[MEM] Segmento carregado: vaddr=0x%08X size=0x%X\n", vaddr, size);
}



void mem_free(uint8_t *mem) {
    free(mem);
}
