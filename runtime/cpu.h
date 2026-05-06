/**
 * runtime/cpu.h
 * Versão Final Consolidada - Sem Redefinições
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── Struct principal da CPU (Alinhada para ARM64) ───────────────────────── */
typedef struct __attribute__((aligned(16))) {
    uint32_t zero;           /* Sempre manteremos em 0 */
    uint32_t at, v0, v1;     /* GPRs 1-3 */
    uint32_t a0, a1, a2, a3; /* GPRs 4-7 */
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7; /* GPRs 8-15 */
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7; /* GPRs 16-23 */
    uint32_t t8, t9, k0, k1; /* GPRs 24-27 */
    uint32_t gp, sp, fp, ra; /* GPRs 28-31 */

    uint32_t hi, lo, pc;     /* Registradores especiais */
    float    f[32];          /* FPU */
    uint32_t fcsr;           /* Status FPU */
    int      running;        /* Flag de execução */
} MIPS_CPU;

/* ── Protótipos de Funções de Suporte ────────────────────────────────────── */
void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target);
void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, const char *name);

/* ── Inicialização ───────────────────────────────────────────────────────── */
static inline void cpu_init(MIPS_CPU *cpu, uint32_t entry, uint32_t sp_val) {
    memset(cpu, 0, sizeof(MIPS_CPU));
    cpu->pc      = entry;
    cpu->sp      = sp_val;
    cpu->running = 1;
    cpu->zero    = 0;
}

/* ── Macros de Fluxo (JR, JALR, SYSCALL) ────────────────────────────────── */
/* Usamos do { } while(0) por segurança de escopo no C */

#define CPU_JR(cpu, addr) \
    do { \
        cpu->pc = addr; \
        return; \
    } while(0)

#define CPU_JALR(cpu, addr) \
    do { \
        cpu->ra = cpu->pc + 8; \
        cpu->pc = addr; \
        return; \
    } while(0)

// Altere a macro existente para isto:
#define CPU_SYSCALL(cpu, code) psp_syscall_mips(cpu, mem, code)

// Adicione a declaração da nova função (junto da outra psp_syscall):
void psp_syscall_mips(MIPS_CPU *cpu, uint8_t *mem, uint32_t code);