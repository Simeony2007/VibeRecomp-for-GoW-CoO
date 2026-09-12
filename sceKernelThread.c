/*
 * PSP Allegrex HLE Thread & Callback Management & Cooperative Scheduler
 */

#include "sceKernelThread.h"
#include "mips_cpu.h"
#include "mips_hle.h"
#include "mips_reloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

PSPThread threads[MAX_THREADS] = {0};
int current_thread_id = -1;
int next_thread_id_to_assign = 1;

HLECallback g_callbacks[MAX_CALLBACKS] = {0};
int g_next_cb_id = 1;

extern uint32_t relocated_gp;

// NOVO: despeja as instrucoes de RAM em torno de um endereco (ja relocadas,
// lidas direto da memoria emulada - nao depende do trace_buffer, que nunca
// e preenchido em lugar nenhum do codigo atual). Usado para ver o que o
// codigo do jogo fez logo antes de um 'jal' suspeito.
static void dump_instr_window(MIPS_CPU *cpu, uint32_t center_addr, int words_before, int words_after) {
    printf("[HLE Debug] Instrucoes de RAM em torno de 0x%08X (provavel local do 'jal'):\n", center_addr);
    for (int i = -words_before; i <= words_after; i++) {
        uint32_t a = center_addr + (i * 4);
        uint32_t *p = (uint32_t *)mips_get_ptr(cpu, a);
        if (p) {
            printf("  %s 0x%08X: 0x%08X\n", (i == 0) ? "-->" : "   ", a, *p);
        } else {
            printf("      0x%08X: <fora da memoria mapeada>\n", a);
        }
    }
}

int hle_create_and_start_thread(MIPS_CPU *cpu, const char *name, uint32_t entry_pc) {
    if (entry_pc == 0) {
        printf("[HLE ThreadMan Error] hle_create_and_start_thread: Refusing to create module thread '%s' with entry_pc == 0\n", name);
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_FREE) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return -1;

    int thid = next_thread_id_to_assign++;
    threads[slot].id = thid;
    strncpy(threads[slot].name, name, sizeof(threads[slot].name) - 1);
    threads[slot].entry_pc = entry_pc;
    threads[slot].suspicious_entry = false;
    memset(threads[slot].regs, 0, sizeof(threads[slot].regs));

    // FIX: usa o INDICE DO SLOT (sempre 0..MAX_THREADS-1) para calcular o
    // endereco da pilha, nao o 'thid' (que so cresce, sem limite, enquanto
    // o jogo rodar). Com 'thid' sem limite, depois de ~191 threads criadas
    // (mesmo que a maioria ja tenha terminado/sido liberada) a subtracao
    // 0x0BFFF000 - (thid*0x100000) estoura (wraparound de unsigned),
    // fazendo pilhas de threads diferentes se sobrepoem e corrompendo a
    // memoria/execucao de forma silenciosa.
    if (slot >= 32) {  // ajuste esse limite ao tamanho real de PSP_RAM_SIZE / 0x100000
        printf("[HLE ThreadMan Error] Slot %d excede a janela segura de stacks (max 32) - recusando criar thread\n", slot);
        cpu->gpr[0x02] = -1;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return -1; // (ou o equivalente no ponto de retorno de cada função)
    }
    uint32_t stack_top = 0x0BFFF000 - (slot * 0x00100000);
    uint32_t k0_addr = stack_top - 256;
    uint32_t sp = k0_addr - 64;

    threads[slot].regs[29] = sp; // $sp
    threads[slot].regs[26] = k0_addr; // $k0
    mips_write32(cpu, k0_addr + 0xc0, 1);
    mips_write32(cpu, k0_addr + 0xc8, stack_top);
    mips_write32(cpu, k0_addr + 0xf8, 0xffffffff);
    mips_write32(cpu, k0_addr + 0xfc, 0xffffffff);
    threads[slot].regs[31] = 0x08000100; // Return address trap
    threads[slot].regs[28] = relocated_gp ? relocated_gp : 0x08B533C0; // $gp fallback

    threads[slot].pc = entry_pc;
    threads[slot].next_pc = entry_pc + 4;
    threads[slot].state = THREAD_STATE_READY;

    printf("[HLE ThreadMan] Created and started internal module thread '%s' (ID: %d, Entry: 0x%08X)\n", name, thid, entry_pc);
    return thid;
}

void run_scheduler(MIPS_CPU *cpu, ThreadState current_new_state) {
    // 1. Save current context and update its state
    if (current_thread_id != -1) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id && threads[i].state == THREAD_STATE_RUNNING) {
                for (int r = 0; r < 32; r++) {
                    threads[i].regs[r] = cpu->gpr[r];
                }
                threads[i].pc = cpu->pc;
                threads[i].next_pc = cpu->next_pc;
                threads[i].hi = cpu->hi;
                threads[i].lo = cpu->lo;
                threads[i].state = current_new_state;
                printf("[HLE Scheduler TCB] Saved Thread '%s' (ID: %d): PC=0x%08X, RA=0x%08X, SP=0x%08X -> State: %d\n", 
                       threads[i].name, threads[i].id, threads[i].pc, threads[i].regs[31], threads[i].regs[29], current_new_state);
                break;
            }
        }
    }

    // 2. Round-robin search starting from current slot for READY thread
    int start_idx = 0;
    if (current_thread_id != -1) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id) {
                start_idx = (i + 1) % MAX_THREADS;
                break;
            }
        }
    }

    int next_idx = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        int idx = (start_idx + i) % MAX_THREADS;
        if (threads[idx].state == THREAD_STATE_READY) {
            next_idx = idx;
            break;
        }
    }

    if (next_idx != -1) {
        // 3. Load next thread context
        threads[next_idx].state = THREAD_STATE_RUNNING;
        current_thread_id = threads[next_idx].id;
        for (int r = 0; r < 32; r++) {
            cpu->gpr[r] = threads[next_idx].regs[r];
        }
        cpu->pc = threads[next_idx].pc;
        cpu->next_pc = threads[next_idx].next_pc;
        cpu->hi = threads[next_idx].hi;
        cpu->lo = threads[next_idx].lo;
        printf("[HLE Scheduler TCB] Switched to Thread '%s' (ID: %d): PC=0x%08X, RA=0x%08X, SP=0x%08X\n", 
               threads[next_idx].name, threads[next_idx].id, cpu->pc, cpu->gpr[31], cpu->gpr[29]);
    } else {
        bool has_waiting = false;
        int first_waiting_idx = -1;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].state == THREAD_STATE_WAITING) {
                has_waiting = true;
                if (first_waiting_idx == -1) first_waiting_idx = i;
            }
        }

        if (has_waiting) {
            // CORREÇÃO BUG 1: Sem esta linha, o cpu->pc continua apontando pra
            // 0x08000100 (return address trap da thread que acabou de morrer).
            // Na próxima chamada do interpretador, o trap dispara de novo,
            // chama run_scheduler(FREE) de novo, que acha a thread já FREE e
            // não salva, busca READY, não acha, cai aqui de novo — loop
            // infinito gerando 257 mil linhas de log antes de você matar o
            // processo. Resolver: zerar o current_thread_id e apontar o PC
            // para um endereço de idle seguro que não dispara nenhum trap.
            current_thread_id = -1;
            cpu->pc      = 0x08000300; // Idle address — não é trap nem código real
            cpu->next_pc = 0x08000304;

            // CORREÇÃO BUG 2: Threads em WAITING (ex: user_main dormindo via
            // sceKernelSleepThreadCB esperando um callback de power/exit que
            // o emulador nunca vai disparar de verdade) ficariam dormindo para
            // sempre. A correção heurística: depois de um ciclo idle sem nenhuma
            // thread READY, acorda a primeira thread WAITING como se o evento
            // que ela esperava tivesse chegado. Isso é o que o PSP real faria
            // via interrupção de hardware — aqui simulamos na thread seguinte
            // ao scheduler detectar o idle.
            printf("[HLE Scheduler] No READY threads. Waiting threads active. "
                   "Waking up thread '%s' (ID: %d) to continue execution.\n",
                   threads[first_waiting_idx].name, threads[first_waiting_idx].id);
            threads[first_waiting_idx].state = THREAD_STATE_READY;

            // Recarrega o contexto da thread acordada imediatamente
            threads[first_waiting_idx].state = THREAD_STATE_RUNNING;
            current_thread_id = threads[first_waiting_idx].id;
            for (int r = 0; r < 32; r++) {
                cpu->gpr[r] = threads[first_waiting_idx].regs[r];
            }
            cpu->pc      = threads[first_waiting_idx].pc;
            cpu->next_pc = threads[first_waiting_idx].next_pc;
            cpu->hi      = threads[first_waiting_idx].hi;
            cpu->lo      = threads[first_waiting_idx].lo;
            printf("[HLE Scheduler TCB] Switched to Thread '%s' (ID: %d): "
                   "PC=0x%08X, RA=0x%08X, SP=0x%08X\n",
                   threads[first_waiting_idx].name, threads[first_waiting_idx].id,
                   cpu->pc, cpu->gpr[31], cpu->gpr[29]);
        } else {
            printf("[HLE Scheduler] All emulated threads have exited. Terminating emulator cleanly.\n");
            cpu->exit_requested = true;
        }
    }
}

// NOVO: detector de loop de criacao de threads. Se o MESMO nome de thread
// for criado muitas vezes seguidas sem nada diferente acontecer entre
// elas, e quase certeza de que o jogo esta preso esperando uma resposta
// de uma API que devolvemos como stub generico (ex: status de carregamento
// de modulo via sceUtility que nunca "termina"). Em vez de deixar isso
// consumir todos os slots de thread / estourar o calculo de stack em
// silencio, a gente avisa alto e recusa a criacao depois de um limite.
#define THREAD_LOOP_WARN_THRESHOLD 20
static char g_last_created_thread_name[64] = {0};
static int g_repeat_create_count = 0;

// HLE Syscall Handler for sceKernelCreateThread (Syscall 0x22225 / NID 0x446D8DE6)
void hle_sceKernelCreateThread(MIPS_CPU *cpu) {
    uint32_t name_ptr = cpu->gpr[0x04];      // $a0
    uint32_t raw_entry_pc = cpu->gpr[0x05];  // $a1
    uint32_t entry_pc = raw_entry_pc;
    const char *name = (const char *)mips_get_ptr(cpu, name_ptr);
    if (!name) name = "unnamed_thread";

    // FIX: 'g_binary_segment_start' antes era um extern pra uma variavel que
    // nunca existia em lugar nenhum do projeto (dava "undefined reference"
    // no link). Agora aponta pra variavel real definida em main_v35.c.
    uint32_t module_base = g_binary_segment_start ? g_binary_segment_start : 0x08800000;

    if (raw_entry_pc > 0 && raw_entry_pc < 0x04000000) {
        entry_pc = raw_entry_pc + module_base;
    }

    if (entry_pc > 0 && entry_pc < module_base + 0x34 && entry_pc != module_base) {
        printf("[HLE ThreadMan Error] sceKernelCreateThread: Refusing to create thread '%s' with invalid header entry_pc: 0x%08X\n", name, entry_pc);
        cpu->gpr[0x02] = -1;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    

    bool is_suspicious_entry = (raw_entry_pc == 0 || entry_pc == module_base);
    if (is_suspicious_entry) {
        printf("[HLE ThreadMan Warning] sceKernelCreateThread: entry_pc para '%s' chegou como 0x%08X (raw=0x%08X) - "
               "provavel ponteiro de funcao nao relocado. Caller RA: 0x%08X. "
               "A thread sera criada mesmo assim, mas provavelmente vai executar lixo/dados.\n",
               name, entry_pc, raw_entry_pc, cpu->gpr[0x1F]);
        dump_instr_window(cpu, cpu->gpr[0x1F], 12, 2);

        // CORREÇÃO: Detecta loop infinito de spawn antes de usar o fallback.
        // Sem este check, o boot code cria 'user_main' com entry_pc=0 →
        // fallback substitui por g_module_entry_point (o próprio boot code) →
        // a nova thread executa o boot e cria mais uma 'user_main' → repete
        // até esgotar todos os 128 slots de thread e travar o emulador.
        // A solução: se já existe uma thread ATIVA com o mesmo nome, recusa
        // criar outra — return -1 como faria o kernel PSP real em duplicata.
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].state != THREAD_STATE_FREE &&
                strncmp(threads[i].name, name, sizeof(threads[i].name)) == 0) {
                printf("[HLE ThreadMan Error] sceKernelCreateThread: Recusando criacao de '%s' duplicada "
                       "(thread ID %d ja existe em estado %d com entry 0x%08X). "
                       "entry_pc invalido neste homebrew provavelmente causa loop de spawn infinito.\n",
                       name, threads[i].id, threads[i].state, threads[i].entry_pc);
                cpu->gpr[0x02] = (uint32_t)-1;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
        }

        if (entry_pc == 0x08800000 && g_module_entry_point != 0 && g_module_entry_point != 0x08800000) {
            printf("[HLE ThreadMan Warning] sceKernelCreateThread: usando fallback entry_pc = 0x%08X (entry point real do modulo) no lugar de 0x08800000 para a thread '%s'.\n",
                   g_module_entry_point, name);
            entry_pc = g_module_entry_point;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_FREE) {
            slot = i;
            break;
        }
    }

    // Deteccao de loop: nome igual ao da ultima thread criada, de novo e de
    // novo. Isso NAO impede a primeira/segunda/etc tentativa (comportamento
    // normal), so age quando passa do limite, evitando que o emulador va
    // silenciosamente ate estourar os slots/memoria.
    if (strncmp(name, g_last_created_thread_name, sizeof(g_last_created_thread_name) - 1) == 0) {
        g_repeat_create_count++;
    } else {
        g_repeat_create_count = 1;
        strncpy(g_last_created_thread_name, name, sizeof(g_last_created_thread_name) - 1);
    }
    if (g_repeat_create_count == THREAD_LOOP_WARN_THRESHOLD) {
        printf("[HLE ThreadMan WARNING] Possivel LOOP INFINITO detectado: thread '%s' foi criada %d vezes seguidas "
               "(Caller RA: 0x%08X). Isso geralmente significa que alguma API HLE que o jogo fica checando em loop "
               "(ex: status de carregamento de modulo via sceUtility) esta retornando um stub generico que nunca "
               "satisfaz a condicao que o jogo espera. Veja as chamadas [HLE Fallback] logo antes desta mensagem "
               "para identificar qual NID precisa de uma implementacao real.\n",
               name, g_repeat_create_count, cpu->gpr[0x1F]);
    }
    if (g_repeat_create_count >= THREAD_LOOP_WARN_THRESHOLD * 4) {
        printf("[HLE ThreadMan ERROR] Loop de criacao de threads '%s' passou de %d repeticoes - recusando criar mais "
               "para evitar esgotar os slots de thread / corromper enderecos de pilha. Corrija a API HLE responsavel "
               "(veja o aviso anterior) em vez de deixar isso continuar.\n",
               name, g_repeat_create_count);
        cpu->gpr[0x02] = -1;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (slot == -1) {
        printf("[HLE ThreadMan Error] Out of thread slots for: %s\n", name);
        cpu->gpr[0x02] = -1;
    } else {
        threads[slot].id = next_thread_id_to_assign++;
        strncpy(threads[slot].name, name, sizeof(threads[slot].name) - 1);
        threads[slot].entry_pc = entry_pc;
        threads[slot].state = THREAD_STATE_DORMANT;
        memset(threads[slot].regs, 0, sizeof(threads[slot].regs));
        threads[slot].pc = entry_pc;
        threads[slot].next_pc = entry_pc + 4;
        threads[slot].suspicious_entry = is_suspicious_entry;
        printf("[HLE ThreadMan] sceKernelCreateThread: Name: '%s', Entry PC: 0x%08X. Assigned Thread ID: %d\n", 
               name, entry_pc, threads[slot].id);
        cpu->gpr[0x02] = threads[slot].id;
    }
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

// HLE Syscall Handler for sceKernelStartThread (Syscall 0x22226 / NID 0xF475845D)
void hle_sceKernelStartThread(MIPS_CPU *cpu) {
    uint32_t thid = cpu->gpr[0x04];
    uint32_t arg_size = cpu->gpr[0x05];
    uint32_t arg_ptr = cpu->gpr[0x06];

    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if ((uint32_t)threads[i].id == thid && threads[i].state == THREAD_STATE_DORMANT) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        printf("[HLE ThreadMan Error] Thread ID %d not found or not DORMANT\n", thid);
        cpu->gpr[0x02] = -1;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    printf("[HLE ThreadMan] sceKernelStartThread: Starting Thread ID %d ('%s')\n", thid, threads[slot].name);

    // FIX: mesmo raciocinio do hle_create_and_start_thread - usa o indice
    // do slot (0..MAX_THREADS-1), nao o thid (sem limite), pra nunca
    // estourar essa subtracao por mais threads que o jogo ja tenha criado
    // ao longo da execucao.
    if (slot >= 32) {  // ajuste esse limite ao tamanho real de PSP_RAM_SIZE / 0x100000
        printf("[HLE ThreadMan Error] Slot %d excede a janela segura de stacks (max 32) - recusando criar thread\n", slot);
        cpu->gpr[0x02] = -1;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return; // (ou o equivalente no ponto de retorno de cada função)
    }
    uint32_t stack_top = 0x0BFFF000 - (slot * 0x00100000);;
    uint32_t k0_addr = stack_top - 256;
    uint32_t sp = k0_addr;

    if (arg_size > 0 && arg_ptr != 0) {
        uint32_t aligned_size = (arg_size + 15) & ~15;
        sp -= aligned_size;
        void *src = mips_get_ptr(cpu, arg_ptr);
        void *dst = mips_get_ptr(cpu, sp);
        if (src && dst) {
            memcpy(dst, src, arg_size);
            printf("[HLE ThreadMan] Copied thread arguments (%u bytes) to stack address: 0x%08X\n", arg_size, sp);
        }
        threads[slot].regs[4] = arg_size; // $a0
        threads[slot].regs[5] = sp;       // $a1
    } else {
        threads[slot].regs[4] = 0;
        threads[slot].regs[5] = 0;
    }

    sp -= 64;
    threads[slot].regs[29] = sp; // $sp
    threads[slot].regs[26] = k0_addr; // $k0
    mips_write32(cpu, k0_addr + 0xc0, 1);
    mips_write32(cpu, k0_addr + 0xc8, stack_top);
    mips_write32(cpu, k0_addr + 0xf8, 0xffffffff);
    mips_write32(cpu, k0_addr + 0xfc, 0xffffffff);
    threads[slot].regs[31] = 0x08000100; // Return address trap
    threads[slot].regs[28] = relocated_gp ? relocated_gp : 0x08B533C0;

    threads[slot].pc = (threads[slot].entry_pc != 0) ? threads[slot].entry_pc : 0x08000100;
    threads[slot].next_pc = threads[slot].pc + 4;
    threads[slot].state = THREAD_STATE_READY;

    cpu->gpr[0x02] = 0;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;

    run_scheduler(cpu, THREAD_STATE_READY);
}

// HLE Syscall Handler for sceKernelExitThread (Syscall 0x22227 / NID 0xAA73C935)
void hle_sceKernelExitThread(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] ExitThread called for Thread ID %d.\n", current_thread_id);
    run_scheduler(cpu, THREAD_STATE_FREE);
}

// HLE Syscall Handler for sceKernelDeleteThread (NID 0x9FA03CD3)
void hle_sceKernelDeleteThread(MIPS_CPU *cpu) {
    uint32_t thid = cpu->gpr[0x04];
    bool deleted = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if ((uint32_t)threads[i].id == thid && threads[i].id != current_thread_id) {
            memset(&threads[i], 0, sizeof(threads[i]));
            threads[i].state = THREAD_STATE_FREE;
            deleted = true;
            printf("[HLE ThreadMan] sceKernelDeleteThread: Thread ID %u slot liberado\n", thid);
            break;
        }
    }
    if (!deleted) {
        printf("[HLE ThreadMan Warning] sceKernelDeleteThread: Thread ID %u nao encontrada ou e a thread atual\n", thid);
    }
    cpu->gpr[0x02] = 0;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

// HLE Syscall Handler for sceKernelDelayThread (Syscall 0x22224 / NID 0x9ACE131E)
void hle_sceKernelDelayThread(MIPS_CPU *cpu) {
    uint32_t usec = cpu->gpr[0x04];
    printf("[HLE ThreadMan] sceKernelDelayThread: delaying %u us\n", usec);
    if (usec > 0) {
        usleep(usec);
    }
    cpu->gpr[0x02] = 0;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
    run_scheduler(cpu, THREAD_STATE_READY);
}

void hle_sceKernelGetThreadState(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] sceKernelGetThreadState -> Returning THREAD_STATE_READY\n");
    cpu->gpr[0x02] = 1;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelGetThreadCurrentPriority(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] sceKernelGetThreadCurrentPriority -> Returning priority 32\n");
    cpu->gpr[0x02] = 32;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelChangeThreadPriority(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] sceKernelChangeThreadPriority -> PSP_OK\n");
    cpu->gpr[0x02] = 0;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelSleepThreadCB(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] sceKernelSleepThreadCB called. Putting current thread (ID: %d) to sleep.\n", current_thread_id);
    // CORREÇÃO: avança o PC para $ra antes de salvar o contexto.
    // Sem isso, o contexto salvo aponta pro stub de SleepThreadCB,
    // e quando o scheduler acorda a thread ela chama SleepThreadCB
    // de novo em loop infinito.
    cpu->pc      = cpu->gpr[0x1F]; // retorna para o caller quando acordar
    cpu->next_pc = cpu->pc + 4;
    cpu->gpr[0x02] = 0; // PSP_OK
    run_scheduler(cpu, THREAD_STATE_WAITING);
}

// sceKernelWaitThreadEndCB (NID 0xD979E9BF)
// Aguarda outra thread terminar, aceitando callbacks enquanto espera.
// Implementacao minima: se a thread alvo ja terminou (FREE), retorna
// imediatamente; caso contrario, coloca a chamadora em WAITING e
// reschedula (comportamento correto para emulacao cooperativa).
void hle_sceKernelWaitThreadEndCB(MIPS_CPU *cpu) {
    uint32_t thid   = cpu->gpr[0x04]; // $a0 = thread ID alvo
    uint32_t status = cpu->gpr[0x05]; // $a1 = ponteiro para status de saida (pode ser NULL)

    bool target_done = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if ((uint32_t)threads[i].id == thid) {
            if (threads[i].state == THREAD_STATE_FREE) {
                target_done = true;
            }
            break;
        }
    }

    if (target_done || thid == 0) {
        printf("[HLE ThreadMan] sceKernelWaitThreadEndCB(thid=%u): thread alvo ja terminou -> retornando imediatamente\n", thid);
        cpu->gpr[0x02] = 0; // PSP_OK
        cpu->pc        = cpu->gpr[0x1F];
        cpu->next_pc   = cpu->pc + 4;
    } else {
        printf("[HLE ThreadMan] sceKernelWaitThreadEndCB(thid=%u): thread alvo ainda ativa -> colocando chamadora (ID %d) em WAITING\n",
               thid, current_thread_id);
        // CORREÇÃO: avança o PC para $ra antes de salvar o contexto.
        // Sem isso, o contexto salvo aponta pro stub de SleepThreadCB,
        // e quando o scheduler acorda a thread ela chama SleepThreadCB
        // de novo em loop infinito.
        cpu->pc      = cpu->gpr[0x1F]; // retorna para o caller quando acordar
        cpu->next_pc = cpu->pc + 4;
        cpu->gpr[0x02] = 0; // PSP_OK
        run_scheduler(cpu, THREAD_STATE_WAITING);
    }
}

// --- Callback Implementation ---

void hle_sceKernelCreateCallback(MIPS_CPU *cpu) {
    uint32_t name_ptr = cpu->gpr[0x04];
    uint32_t entry = cpu->gpr[0x05];
    uint32_t common_arg = cpu->gpr[0x06];
    const char *name = (const char *)mips_get_ptr(cpu, name_ptr);
    if (!name) name = "unnamed_cb";

    int slot = -1;
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (!g_callbacks[i].is_active) {
            slot = i;
            break;
        }
    }

    if (slot != -1) {
        g_callbacks[slot].id = g_next_cb_id++;
        strncpy(g_callbacks[slot].name, name, 31);
        g_callbacks[slot].name[31] = '\0';
        g_callbacks[slot].entrypoint = entry;
        g_callbacks[slot].common_arg = common_arg;
        g_callbacks[slot].thread_id = current_thread_id;
        g_callbacks[slot].notify_count = 0;
        g_callbacks[slot].notify_arg = 0;
        g_callbacks[slot].is_active = true;
        printf("[HLE ThreadMan] sceKernelCreateCallback: Name: '%s', Entry: 0x%08X -> Assigned ID %d\n",
               name, entry, g_callbacks[slot].id);
        cpu->gpr[0x02] = g_callbacks[slot].id;
    } else {
        printf("[HLE ThreadMan Error] Out of callback slots for '%s'\n", name);
        cpu->gpr[0x02] = -1;
    }
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelCheckCallback(MIPS_CPU *cpu) {
    int pending_cb_slot = -1;
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (g_callbacks[i].is_active && g_callbacks[i].notify_count > 0 &&
            (g_callbacks[i].thread_id == current_thread_id || g_callbacks[i].thread_id == -1)) {
            pending_cb_slot = i;
            break;
        }
    }

    if (pending_cb_slot != -1) {
        uint32_t cb_entry = g_callbacks[pending_cb_slot].entrypoint;
        uint32_t count = g_callbacks[pending_cb_slot].notify_count;
        uint32_t arg = g_callbacks[pending_cb_slot].notify_arg;
        uint32_t common = g_callbacks[pending_cb_slot].common_arg;

        if (cb_entry < 0x08800000 || cb_entry >= 0x0A000000) {
            printf("[HLE ThreadMan Warning] sceKernelCheckCallback: callback '%s' (ID %d) tem Entry PC invalido (0x%08X) - descartando notificacao\n",
                   g_callbacks[pending_cb_slot].name, g_callbacks[pending_cb_slot].id, cb_entry);
            g_callbacks[pending_cb_slot].notify_count = 0;
            g_callbacks[pending_cb_slot].notify_arg = 0;
            cpu->gpr[0x02] = 0;
            cpu->pc = cpu->gpr[0x1F];
            cpu->next_pc = cpu->pc + 4;
            run_scheduler(cpu, THREAD_STATE_READY);
            return;
        }

        g_callbacks[pending_cb_slot].notify_count = 0;
        g_callbacks[pending_cb_slot].notify_arg = 0;

        uint32_t sp = cpu->gpr[29] - 128;
        cpu->gpr[29] = sp;

        for (int r = 4; r <= 25; r++) {
            mips_write32(cpu, sp + (r * 4), cpu->gpr[r]);
        }
        mips_write32(cpu, sp + 31 * 4, cpu->gpr[31]);
        mips_write32(cpu, sp + 0, cpu->pc);

        cpu->gpr[0x04] = count;
        cpu->gpr[0x05] = arg;
        cpu->gpr[0x06] = common;
        cpu->gpr[0x02] = 1;
        cpu->gpr[31]   = CB_RETURN_HACK_ADDR;

        cpu->pc = cb_entry;
        cpu->next_pc = cb_entry + 4;
        return;
    } else {
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        run_scheduler(cpu, THREAD_STATE_READY);
        return;
    }
}

void hle_sceKernelDeleteCallback(MIPS_CPU *cpu) {
    uint32_t cbid = cpu->gpr[0x04];
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (g_callbacks[i].is_active && g_callbacks[i].id == cbid) {
            g_callbacks[i].is_active = false;
            g_callbacks[i].notify_count = 0;
            printf("[HLE ThreadMan] sceKernelDeleteCallback: Deleted callback ID %u ('%s')\n", cbid, g_callbacks[i].name);
            break;
        }
    }
    cpu->gpr[0x02] = 0;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}
