#include "scheduler.h"
#include <stdio.h>

PSP_Thread thread_pool[MAX_THREADS];
int current_thread_idx = -1;
int thread_count = 0;

PSP_Sema sema_pool[MAX_SEMAS];
int sema_count = 0;

#define THREAD_SLEEPING 5
int wakeup_counts[MAX_THREADS] = {0};

// Pool de EventFlags
#define MAX_EVFLAGS 32
PSP_EventFlag evflag_pool[MAX_EVFLAGS];
int evflag_count = 0;



int scheduler_create_sema(const char* name, int init_count, int max_count) {
    if (sema_count >= MAX_SEMAS) return -1;

    int idx = sema_count++;
    sema_pool[idx].uid = idx + 1;
    strncpy(sema_pool[idx].name, name, 63);
    sema_pool[idx].name[63] = '\0';  // FIX: garantia de terminador
    sema_pool[idx].init_count    = init_count;
    sema_pool[idx].current_count = init_count;
    sema_pool[idx].max_count     = max_count;
    sema_pool[idx].status        = 1;

    return sema_pool[idx].uid;
}

int scheduler_wait_sema(int sema_id, int signal, MIPS_CPU *current_cpu) {
    for (int i = 0; i < sema_count; i++) {
        if (sema_pool[i].uid == sema_id && sema_pool[i].status == 1) {
            if (sema_pool[i].current_count >= signal) {
                sema_pool[i].current_count -= signal;
                return 0;
            } else {
                thread_pool[current_thread_idx].status      = THREAD_WAITING_SEMA;
                thread_pool[current_thread_idx].wait_sema_id = sema_id;
                scheduler_yield(current_cpu, 0);
                return 1;
            }
        }
    }
    return -1;
}

void scheduler_signal_sema(int sema_id, int signal) {
    // FIX: linha órfã "sema_pool[i].current_count += signal" removida daqui
    for (int i = 0; i < sema_count; i++) {
        if (sema_pool[i].uid == sema_id && sema_pool[i].status == 1) {
            sema_pool[i].current_count += signal;
            if (sema_pool[i].current_count > sema_pool[i].max_count) {
                sema_pool[i].current_count = sema_pool[i].max_count;
            }

            // FIX: só acorda thread se há recurso disponível (evita current_count negativo)
            for (int t = 0; t < thread_count; t++) {
                if (thread_pool[t].status       == THREAD_WAITING_SEMA &&
                    thread_pool[t].wait_sema_id == sema_id             &&
                    sema_pool[i].current_count  >= 1) {
                    thread_pool[t].status       = THREAD_READY;
                    thread_pool[t].wait_sema_id = 0;
                    sema_pool[i].current_count -= 1;
                }
            }
            return;
        }
    }
}

// ─── Criação e Controle de EventFlags ───────────────────────────────────────

int scheduler_create_evflag(const char *name, int attr, uint32_t init_pattern) {
    if (evflag_count >= MAX_EVFLAGS) return -1;
    
    int idx = evflag_count++;
    evflag_pool[idx].uid = idx + 1; // UIDs do PSP sempre começam em 1
    strncpy(evflag_pool[idx].name, name, 63);
    evflag_pool[idx].name[63] = '\0';
    evflag_pool[idx].current_pattern = init_pattern;
    evflag_pool[idx].status = 1;
    
    return evflag_pool[idx].uid;
}

void scheduler_set_evflag(int uid, uint32_t bits, MIPS_CPU *current_cpu) {
    for (int i = 0; i < evflag_count; i++) {
        if (evflag_pool[i].uid == uid && evflag_pool[i].status == 1) {
            // "Seta" os bits (Soma binária OR)
            evflag_pool[i].current_pattern |= bits; 
            printf("[SCHEDULER] EvFlag %d ('%s') acesa! Bits: 0x%08X (Atual: 0x%08X)\n",
                   uid, evflag_pool[i].name, bits, evflag_pool[i].current_pattern);
            
            // Força o emulador a trocar de thread, para que o Kratos 
            // acorde imediatamente agora que a bandeira que ele esperava acendeu!
            scheduler_yield(current_cpu, 0);
            return;
        }
    }
}

void scheduler_init() {
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_pool[i].status = THREAD_DEAD;
    }
}

extern uint32_t global_gp;

int scheduler_create_thread(const char* name, uint32_t entry_pc, int priority, MIPS_CPU *current_cpu) {
    if (thread_count >= MAX_THREADS) return -1;

    int idx = thread_count++;
    thread_pool[idx].uid = idx + 1;
    strncpy(thread_pool[idx].name, name, 63);
    thread_pool[idx].name[63] = '\0';
    thread_pool[idx].status   = THREAD_READY;
    thread_pool[idx].priority = priority; 

    memset(&thread_pool[idx].context, 0, sizeof(MIPS_CPU));
    thread_pool[idx].context.pc      = entry_pc;
    thread_pool[idx].context.ra      = 0x0FFFFFFF;
    
    // --- O MILAGRE DO GP ---
    // Ignoramos o current_cpu->gp (que o jogo corrompeu) e forçamos o verdadeiro!
    thread_pool[idx].context.gp      = global_gp; 
    thread_pool[idx].context.t9      = entry_pc;  // Regra obrigatória da arquitetura MIPS
    
    thread_pool[idx].context.running = 1;
    thread_pool[idx].context.sp      = 0x09FFF000 - (idx * 0x10000);

    return thread_pool[idx].uid;
}

void scheduler_change_priority(int uid, int priority, MIPS_CPU *current_cpu) {
    int target_idx = -1;
    
    // UID 0 significa "Mudar a prioridade da Thread que chamou a função"
    if (uid == 0) {
        target_idx = current_thread_idx;
    } else {
        for (int i = 0; i < thread_count; i++) {
            if (thread_pool[i].uid == uid) {
                target_idx = i;
                break;
            }
        }
    }

    if (target_idx != -1) {
        printf("[SCHEDULER] Thread %d ('%s') mudou prioridade para %d\n", 
               thread_pool[target_idx].uid, thread_pool[target_idx].name, priority);
        thread_pool[target_idx].priority = priority;
        
        // Força um yield! Se a thread nova tiver prioridade maior, ela rouba a CPU agora.
        scheduler_yield(current_cpu, 0); 
    }
}

void scheduler_exit_thread(MIPS_CPU *current_cpu) {
    if (current_thread_idx == -1) return;

    printf("[SCHEDULER] Thread %d finalizada graciosamente. Removendo da fila...\n",
           thread_pool[current_thread_idx].uid);
    thread_pool[current_thread_idx].status = THREAD_DEAD;
    scheduler_yield(current_cpu, 0);
}

void scheduler_start_thread(int uid) {
    for (int i = 0; i < thread_count; i++) {
        if (thread_pool[i].uid == uid && thread_pool[i].status == THREAD_READY) {
            if (current_thread_idx == -1) {
                current_thread_idx = i;
                thread_pool[i].status = THREAD_RUNNING;
            }
            printf("[SCHEDULER] Thread %d ('%s') marcada para iniciar.\n",
                   uid, thread_pool[i].name);
            return;
        }
    }
}

void scheduler_yield(MIPS_CPU *current_cpu, uint32_t wait_cycles) {
    if (current_thread_idx == -1) return;

    // 1. Salva estado da thread atual
    thread_pool[current_thread_idx].context    = *current_cpu;
    thread_pool[current_thread_idx].wait_timer = wait_cycles;

    // ─── O SEGREDO DO DEADLOCK RESOLVIDO AQUI ───
    if (wait_cycles > 0) {
        thread_pool[current_thread_idx].status = THREAD_WAITING;
    } 
    else if (thread_pool[current_thread_idx].status != THREAD_DEAD && 
             thread_pool[current_thread_idx].status != THREAD_SLEEPING) {
        // Se a thread estava apenas rodando (RUNNING) e passou a vez, 
        // ela DEVE voltar para a fila (READY)! Isso salva a Thread 1!
        thread_pool[current_thread_idx].status = THREAD_READY;
    }
    // ────────────────────────────────────────────

    // 2. Procura a próxima thread pronta (Round-Robin Justo / Anti-Starvation)
    int next_idx = -1;
    for (int i = 1; i <= thread_count; i++) {
        int check_idx = (current_thread_idx + i) % thread_count;
        if (thread_pool[check_idx].status == THREAD_READY) {
            next_idx = check_idx;
            break;
        }
    }

    // 3. Fast-forward: avança o tempo se só há threads em THREAD_WAITING (timer)
    // ... (o resto do seu código continua exatamente igual daqui pra baixo!)

    // 3. Fast-forward: avança o tempo se só há threads em THREAD_WAITING (timer)
    if (next_idx == -1) {
        int has_waiting  = 0;
        uint32_t min_wait = 0xFFFFFFFF;

        for (int i = 0; i < thread_count; i++) {
            if (thread_pool[i].status == THREAD_WAITING) {
                has_waiting = 1;
                if (thread_pool[i].wait_timer < min_wait)
                    min_wait = thread_pool[i].wait_timer;
            }
        }
        
        if (has_waiting) {
            for (int i = 0; i < thread_count; i++) {
                if (thread_pool[i].status == THREAD_WAITING) {
                    thread_pool[i].wait_timer -= min_wait;
                    if (thread_pool[i].wait_timer == 0) {
                        thread_pool[i].status = THREAD_READY;
                        if (next_idx == -1) next_idx = i;
                    }
                }
            }
        }
    }

    // FIX Bug #4: Detecta deadlock real independentemente do status da thread atual.
    // Antes a condição era "next_idx == -1 && status != THREAD_RUNNING", o que
    // deixava passar o caso em que a thread atual voltou para THREAD_READY mas
    // nenhuma outra estava pronta (deadlock por semáforo silencioso).
    if (next_idx == -1) {
        printf("[SCHEDULER FATAL] Deadlock: nenhuma thread pronta ou com timer ativo.\n");
        printf("[SCHEDULER FATAL] Estado das threads:\n");
        for (int i = 0; i < thread_count; i++) {
            printf("  Thread %d ('%s'): status=%d wait_sema=%d wait_timer=%u\n",
                   thread_pool[i].uid, thread_pool[i].name,
                   thread_pool[i].status, thread_pool[i].wait_sema_id,
                   thread_pool[i].wait_timer);
        }
        current_cpu->running = 0;
        return;
    }

    // 4. Restaura e executa a próxima thread
    current_thread_idx = next_idx;
    thread_pool[current_thread_idx].status = THREAD_RUNNING;
    *current_cpu = thread_pool[current_thread_idx].context;
}

// Adicione no final do scheduler.c
int scheduler_wait_evflag(int evid, uint32_t bits, int wait_type, uint32_t *out_bits, MIPS_CPU *current_cpu) {
    for (int i = 0; i < evflag_count; i++) {
        if (evflag_pool[i].uid == evid && evflag_pool[i].status == 1) {
            int match = 0;
            
            // Verifica o tipo de espera: OR (1) ou AND (0)
            if (wait_type & 1) { // PSP_EVENT_WAITOR
                match = ((evflag_pool[i].current_pattern & bits) != 0);
            } else {             // PSP_EVENT_WAITAND
                match = ((evflag_pool[i].current_pattern & bits) == bits);
            }

            if (match) {
                // SUCESSO! A condição foi atingida.
                if (out_bits) *out_bits = evflag_pool[i].current_pattern;
                
                // O PSP permite limpar a bandeira após ler (CLEAR = 0x10, CLEARPAT = 0x20)
                if (wait_type & 0x10) evflag_pool[i].current_pattern = 0;
                else if (wait_type & 0x20) evflag_pool[i].current_pattern &= ~bits;
                
                return 0; // Libera a Thread!
            } else {
                // BLOQUEIO! A condição ainda não foi atingida. Pausamos a Thread.
                thread_pool[current_thread_idx].status = THREAD_WAITING_EVFLAG;
                thread_pool[current_thread_idx].wait_sema_id = evid; // Reusamos esse campo pro ID
                thread_pool[current_thread_idx].wait_evflag_bits = bits;
                
                // MÁGICA: Dizemos pro Dispatcher repetir esse bloco de código quando acordar!
                current_cpu->zero = 1; 
                scheduler_yield(current_cpu, 0);
                return 1; // Sinaliza que a thread dormiu
            }
        }
    }
    return -1; // EvFlag não encontrada
}

// ─── Controle de Sono e Despertar (Sleep / Wakeup) ──────────────────────────

void scheduler_sleep_thread(MIPS_CPU *current_cpu) {
    if (current_thread_idx == -1) return;

    // Se a thread já recebeu um "Acorda" adiantado, ela gasta o crédito e não dorme!
    if (wakeup_counts[current_thread_idx] > 0) {
        wakeup_counts[current_thread_idx]--;
        printf("[SCHEDULER] Thread %d tentou dormir, mas gastou um Wakeup adiantado. Seguindo!\n", 
               thread_pool[current_thread_idx].uid);
        current_cpu->v0 = 0;
        return;
    }

    // Se não tem crédito, dorme de verdade
    thread_pool[current_thread_idx].status = THREAD_SLEEPING;
    printf("[SCHEDULER] Thread %d entrou em estado de SLEEP profundo.\n", thread_pool[current_thread_idx].uid);
    
    // Força a CPU a passar a vez para outra thread
    scheduler_yield(current_cpu, 0);
}

int scheduler_wakeup_thread(int uid) {
    for (int i = 0; i < thread_count; i++) {
        if (thread_pool[i].uid == uid) {
            if (thread_pool[i].status == THREAD_SLEEPING) {
                // Se estava dormindo, acorda e volta para a fila
                thread_pool[i].status = THREAD_READY;
                printf("[SCHEDULER] Thread %d foi ACORDADA e voltou pra fila!\n", uid);
            } else {
                // Se não estava dormindo, guarda um "Crédito" de wakeup para o futuro!
                wakeup_counts[i]++;
                printf("[SCHEDULER] Thread %d recebeu Wakeup adiantado (Créditos: %d).\n", 
                       uid, wakeup_counts[i]);
            }
            return 0; // Sucesso
        }
    }
    return 0x800201A0; // Erro: SCE_KERNEL_ERROR_UNKNOWN_THID
}