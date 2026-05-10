/**
 * runtime/memory.h
 * Macros de acesso à memória do PSP com MMU Virtual para PRX
 */

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PSP_MEM_SIZE 0x02000000 // 32MB de RAM
#define PSP_RAM_BASE 0x08000000
#define PSP_STACK_TOP 0x09FFF000 // Topo da memória RAM menos margem

#define CLEAN_ADDR(addr) ((addr) & 0x1FFFFFFF)

/* ── Tradução de endereço (MMU VIRTUAL) ──────────────────────────────────── */
static inline uint32_t psp_translate_addr(uint32_t addr) {
    addr = CLEAN_ADDR(addr); // Mantém sua limpeza de bits

    // 1. Placa de Vídeo (VRAM - Joga para o final da RAM alocada - Offset 30MB)
    if ((addr & 0x0F000000) == 0x04000000) {
        return (addr & 0x001FFFFF) + 0x01E00000;
    }
    
    // 2. RAM do Sistema (0x08800000 até 0x09FFFFFF) -> Tem 24MB de espaço!
    if (addr >= 0x08800000 && addr <= 0x09FFFFFF) {
        return addr - 0x08800000;
    }
    
    // 3. Acesso Direto (Ponteiros crus não-relocados gerados pelo Loader)
    if (addr < 0x02000000) {
        return addr;
    }

    // 4. FALLBACK BLINDADO (O Freio do Segfault)
    // Se o jogo tentar acessar uma memória desconhecida, nós forçamos 
    // ela a ficar dentro do limite máximo de 32MB (0x01FFFFFF) via máscara.
    // Isso evita o Segfault e permite que o jogo continue ou acuse erro de lógica.
    return addr & 0x01FFFFFF;
}

// --- Funções para Acesso de Memória Desalinhada (LWL/LWR/SWL/SWR) ---

#define MEM_R32_DEBUG(mem, addr, gp_hint) do { \
    uint32_t _val = *(uint32_t*)&mem[psp_translate_addr(addr)]; \
    if ((_val & 0xFFFF0000) == 0 && _val != 0) { \
        printf("[MEM DEBUG] Leitura suspeita em 0x%08X: valor=0x%08X (gp_hint=0x%08X)\n", \
               addr, _val, gp_hint); \
    } \
} while(0)

#define MEM_R32(mem, addr) (*(uint32_t*)&mem[psp_translate_addr(addr)])
#define MEM_W32(mem, addr, val) (*(uint32_t*)&mem[psp_translate_addr(addr)] = (val))

// --- SÓ AGORA COLOQUE AS FUNÇÕES QUE MANDEI ---
static inline uint32_t psp_lwl(uint8_t *mem, uint32_t addr, uint32_t rt) {
    uint32_t val = MEM_R32(mem, addr & ~3);
    uint32_t shift = (addr & 3) << 3;
    uint32_t mask = 0xFFFFFFFF << (24 - shift);
    return (rt & ~mask) | (val << (24 - shift));
}

static inline uint32_t psp_lwr(uint8_t *mem, uint32_t addr, uint32_t rt) {
    uint32_t val = MEM_R32(mem, addr & ~3);
    uint32_t shift = (addr & 3) << 3;
    uint32_t mask = 0xFFFFFFFF >> shift;
    return (rt & ~mask) | (val >> shift);
}

static inline void psp_swl(uint8_t *mem, uint32_t addr, uint32_t rt) {
    uint32_t val = MEM_R32(mem, addr & ~3);
    uint32_t shift = (addr & 3) << 3;
    uint32_t mask = 0xFFFFFFFF >> (24 - shift);
    val = (val & ~mask) | (rt >> (24 - shift));
    MEM_W32(mem, addr & ~3, val);
}

static inline void psp_swr(uint8_t *mem, uint32_t addr, uint32_t rt) {
    uint32_t val = MEM_R32(mem, addr & ~3);
    uint32_t shift = (addr & 3) << 3;
    uint32_t mask = 0xFFFFFFFF << shift;
    val = (val & ~mask) | (rt << shift);
    MEM_W32(mem, addr & ~3, val);
}

/* ── Macros de Acesso Direto ─────────────────────────────────────────────── */
/* Agora o MEM_PTR passa pela nossa MMU Virtual para consertar os ponteiros do PRX */
#define MEM_PTR(mem, addr) (&mem[psp_translate_addr(addr)])

/* Leituras */
#define MEM_R8(mem, addr)  (*(uint8_t*)MEM_PTR(mem, addr))
#define MEM_R16(mem, addr) (*(uint16_t*)MEM_PTR(mem, addr))
//#define MEM_R32(mem, addr) (*(uint32_t*)MEM_PTR(mem, addr))

/* Escritas */
#define MEM_W8(mem, addr, val)  (*(uint8_t*)MEM_PTR(mem, addr) = (val))
#define MEM_W16(mem, addr, val) (*(uint16_t*)MEM_PTR(mem, addr) = (val))
//#define MEM_W32(mem, addr, val) (*(uint32_t*)MEM_PTR(mem, addr) = (val))

/* ── Gerenciamento da memória ────────────────────────────────────────────── */
uint8_t *mem_init(void);
void     mem_load_segment(uint8_t *mem, const uint8_t *data, uint32_t vaddr, uint32_t size);
void     mem_free(uint8_t *mem);