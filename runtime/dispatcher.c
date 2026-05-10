#include "dispatcher.h"
#include "memory.h"
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
    target = CLEAN_ADDR(target);
    if ((target & 3u) != 0) target &= ~3u;
    uint32_t *item = bsearch(&target, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
    if (item) return target;

    // Fallback desativado: forçar erro imediato em PC inválido/desalinhado.
    return 0;
}

void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {
    if ((target & 3u) != 0) target &= ~3u;
    target = CLEAN_ADDR(target);

    // 1. Gestão de threads via Sentinel RA
    if (target == 0x0FFFFFFF || target == 0x0FFFFFFC) {
        printf("[HLE] Retorno de Thread detectado. Destruindo thread atual...\n");
        extern void scheduler_exit_thread(MIPS_CPU *cpu);
        scheduler_exit_thread(cpu);
        return; 
    }

    /* --- PROTEÇÃO CONTRA CALLBACKS NULOS --- */
    if (target == 0x00000000) {
        
        // ========================================================
        // GOW HACKS: Bypasses de Tabelas de Callbacks Vazias
        // ========================================================
        
        // HACK 1: Callback de Inicialização
        if (cpu->ra == 0x08805FE8) {
            printf("[PATCH] God of War: Callback inicial vazio ignorado. Redirecionando fluxo...\n");
            cpu->pc = 0x08806144u;
            return;
        }

        // HACK 2: Callback de Sincronização de Vídeo (Double Buffering)
        if (cpu->ra == 0x0881671C) {
            printf("[PATCH] God of War: Callback de Vídeo ignorado. Redirecionando fluxo para Geometria...\n");
            // 0x08816878 é a saída segura original da função de VBlank
            cpu->pc = 0x08816878u; 
            return;
        }

        // ========================================================

        printf("[WARN] Execução NULA interceptada! A culpa é da função no RA: 0x%08X\n", cpu->ra);
        cpu->pc = cpu->ra; 
        return;
    }

    /* --- LÓGICA DE SOMA (Auto-Relocação de Offsets) --- */
    if (target > 0 && target < 0x08000000) {
        target += 0x08800000; 
    }

    /* --- MEMORY HOOK: PROTEÇÃO CONTRA PONTEIROS NÃO REALOCADOS --- */
    if (target == 0x464C457F) {
        cpu->pc = cpu->ra; 
        return;
    }

    /* Proteção contra PCs que apontam para strings ASCII */
    if ((target & 0xFF000000) == 0x72000000 ||  /* 'r' */
        (target & 0xFF000000) == 0x45000000 ||  /* 'E' */
        target < 0x08000000) {
        printf("[WARN] PC parece ser string/dado, não código: 0x%08X — retornando ao RA\n", target);
        cpu->pc = cpu->ra;
        return;
    }

    /* --- REDE DE SEGURANÇA GLOBAL (ESCUDO MESTRE C++) --- */
    // Bloqueia qualquer salto para o Heap (> 0x08B10000) ou fora da RAM.
    // Garante que não bloqueia o fecho de threads (0x0FFFFFFF).
    if (target >= 0x08B10000 && target != 0x0FFFFFFF && target != 0x0FFFFFFC) {
        
        // Proteção contra chamadas iterativas fantasmas
        if (cpu->ra >= 0x08B10000 && cpu->ra != 0x0FFFFFFF && cpu->ra != 0x0FFFFFFC) {
             printf("[ERRO FATAL] Stack C++ corrompida. Origem absurda: 0x%08X\n", cpu->ra);
             cpu->running = 0;
             return;
        }

        // Simula o retorno de um método de VTable ausente.
        cpu->v0 = 0;
        cpu->pc = cpu->ra;
        return;
    }

    /* --- PREEMPTIVE SCHEDULING (TIME SLICE) --- */
    static int time_slice = 0;
    time_slice++;
    if (time_slice > 1500) { 
        time_slice = 0;
        extern void scheduler_yield(MIPS_CPU *current_cpu, uint32_t wait_cycles);
        scheduler_yield(cpu, 0); 
        
        if (cpu->pc != target) {
            return;
        }
    }

    /* --- PC TRACE --- */
    printf("[TRACE] Executando bloco: 0x%08X | RA: 0x%08X\n", target, cpu->ra);

    /* --- INTERCEPTAÇÃO HLE ABSOLUTA --- */
    uint32_t pc_before_hle = cpu->pc; 

    if (intercept_hle(cpu, mem, target)) {
        if (cpu->zero == 1) {
            cpu->zero = 0; 
            return;        
        }

        if (cpu->pc == pc_before_hle) {
            cpu->pc = cpu->ra; 
        } 
        return;
    }

    // 2. Execução
    uint32_t mapped = find_dispatch_address(target);
    if (mapped != 0) {
        uint32_t *item = bsearch(&mapped, psp_func_addrs, PSP_FUNC_COUNT, sizeof(uint32_t), compare_addrs);
        int index = item - psp_func_addrs;
        psp_func_table[index](cpu, mem);
    } else {
        // Fallback final: se o PC está na área válida (.text) mas não mapeado.
        printf("[ERRO] PC corrompido ou bloco não compilado: 0x%08X (RA: 0x%08X)\n", target, cpu->ra);
        cpu->running = 0;
    }
}