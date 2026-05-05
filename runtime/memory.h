/**
 * runtime/memory.h
 * Macros de acesso à memória do PSP (Otimizado para ARM64)
 */

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PSP_MEM_SIZE 0x02000000 // 32MB de RAM (Mude para 0x04000000 se for PSP Slim)
#define PSP_RAM_BASE 0x08000000
#define PSP_STACK_TOP 0x09FFF000 // Topo da memória RAM menos uma margem de segurança

/* ── Tradução de endereço ────────────────────────────────────────────────── */
/* Transforma endereços virtuais do PSP (0x08XXXXXX ou 0x88XXXXXX) 
   em um offset seguro (0 a 32MB) para o nosso array. */
static inline uint32_t psp_translate_addr(uint32_t vaddr) {
    return vaddr & 0x01FFFFFF;
}

/* ── Macros de Acesso Direto (MÁXIMA PERFORMANCE) ────────────────────────── */
/* A máscara & 0x01FFFFFF aqui substitui o "if(fora do limite)" e impede Segfault.
   É uma instrução de bitwise AND (1 ciclo de clock) em vez de um branch. */
#define MEM_PTR(mem, addr) (&mem[(addr) & 0x01FFFFFF])

/* Leituras */
#define MEM_R8(mem, addr)  (*(uint8_t*)MEM_PTR(mem, addr))
#define MEM_R16(mem, addr) (*(uint16_t*)MEM_PTR(mem, addr))
#define MEM_R32(mem, addr) (*(uint32_t*)MEM_PTR(mem, addr))

/* Escritas */
#define MEM_W8(mem, addr, val)  (*(uint8_t*)MEM_PTR(mem, addr) = (val))
#define MEM_W16(mem, addr, val) (*(uint16_t*)MEM_PTR(mem, addr) = (val))
#define MEM_W32(mem, addr, val) (*(uint32_t*)MEM_PTR(mem, addr) = (val))


/* ── Gerenciamento da memória ────────────────────────────────────────────── */
uint8_t *mem_init(void);
void     mem_load_segment(uint8_t *mem, const uint8_t *data, uint32_t vaddr, uint32_t size);
void     mem_free(uint8_t *mem);