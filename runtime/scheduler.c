#include "scheduler.h"
#include <stdio.h>

PSP_Thread thread_pool[MAX_THREADS];
int current_thread_idx = -1;
int thread_count = 0;

void scheduler_init() {
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_pool[i].status = THREAD_DEAD;
    }
}

int scheduler_create_thread(const char* name, uint32_t entry_pc, MIPS_CPU *current_cpu) {
    if (thread_count >= MAX_THREADS) return -1;
    
    int idx = thread_count++;
    thread_pool[idx].uid = idx + 1; 
    strncpy(thread_pool[idx].name, name, 63);
    thread_pool[idx].status = THREAD_READY;
    
    memset(&thread_pool[idx].context, 0, sizeof(MIPS_CPU));
    thread_pool[idx].context.pc = entry_pc;
    
    // INJEÇÃO DO SENTINELA: O endereço de retorno falso
    thread_pool[idx].context.ra = 0x0FFFFFFF; 
    
    thread_pool[idx].context.running = 1; 
    thread_pool[idx].context.sp = 0x09FFF000 - (idx * 0x10000); 
    
    return thread_pool[idx].uid;
}

// NOVA FUNÇÃO: Mata a thread e cede o controle
void scheduler_exit_thread(MIPS_CPU *current_cpu) {
    if (current_thread_idx == -1) return;
    
    printf("[SCHEDULER] Thread %d finalizada graciosamente. Removendo da fila...\n", thread_pool[current_thread_idx].uid);
    thread_pool[current_thread_idx].status = THREAD_DEAD;
    
    // Força o Context Switch passando o controle para a próxima thread Ready
    scheduler_yield(current_cpu, 0); 
}

void scheduler_start_thread(int uid) {
    for (int i = 0; i < thread_count; i++) {
        if (thread_pool[i].uid == uid && thread_pool[i].status == THREAD_READY) {
            if (current_thread_idx == -1) {
                current_thread_idx = i;
                thread_pool[i].status = THREAD_RUNNING;
            }
            
            // INJEÇÃO DO GLOBAL POINTER (GP) PARA O KRATOS ACHAR OS CONSTRUTORES
            // O valor padrão do GP no God of War costuma ficar em 0x088...
            // (Colocaremos um valor seguro, mas você pode pegar do json depois)
            thread_pool[i].context.s0 = 0x08815200; // Valor aproximado da base da .ctors + offset
            
            printf("[SCHEDULER] Thread %d ('%s') marcada para iniciar.\n", uid, thread_pool[i].name);
            return;
        }
    }
}

// O coração do emulador: Troca de Contexto
void scheduler_yield(MIPS_CPU *current_cpu, uint32_t wait_cycles) {
    if (current_thread_idx == -1) return;

    // 1. Salva estado
    thread_pool[current_thread_idx].context = *current_cpu;
    thread_pool[current_thread_idx].wait_timer = wait_cycles;
    
    if (wait_cycles > 0) {
        thread_pool[current_thread_idx].status = THREAD_WAITING;
    } else if (thread_pool[current_thread_idx].status != THREAD_DEAD) {
        thread_pool[current_thread_idx].status = THREAD_READY; 
    }

    // 2. Procura a próxima thread (AGORA BLINDADO)
    int next_idx = -1;
    // Atenção aqui: trocamos MAX_THREADS por thread_count!
    for (int i = 1; i <= thread_count; i++) { 
        int check_idx = (current_thread_idx + i) % thread_count;
        if (thread_pool[check_idx].status == THREAD_READY) {
            next_idx = check_idx;
            break;
        }
    }

    if (next_idx == -1 && thread_pool[current_thread_idx].status != THREAD_RUNNING) {
        printf("[SCHEDULER FATAL] Nenhuma thread pronta. Deadlock!\n");
        current_cpu->running = 0;
        return;
    }

    // 3. Restaura e ATUALIZA O PC DA CPU
    if (next_idx != -1) {
        current_thread_idx = next_idx;
        thread_pool[current_thread_idx].status = THREAD_RUNNING;
        
        // Copia todo o contexto da thread escolhida para a CPU real do emulador
        *current_cpu = thread_pool[current_thread_idx].context;
        
        printf("[SCHEDULER] Context Switch -> Retomando Thread %d no PC 0x%08X\n", 
               thread_pool[current_thread_idx].uid, current_cpu->pc);
    }
}