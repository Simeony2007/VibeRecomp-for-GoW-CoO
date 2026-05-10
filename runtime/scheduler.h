#pragma once
#include <stdint.h>
#include <string.h>
#include "cpu.h"

// Estados da Thread
// Estados da Thread
#define THREAD_EMPTY   0
#define THREAD_READY   1
#define THREAD_RUNNING 2
#define THREAD_WAITING 3
#define THREAD_DEAD    4

#define MAX_THREADS 16

#define MAX_SEMAS 32
#define THREAD_WAITING_SEMA 3  // Novo status

#define MAX_EVFLAGS 32
#define THREAD_WAITING_EVFLAG 4 // Novo status de bloqueio

typedef struct {
    int uid;
    char name[64];
    uint32_t current_pattern;
    int status; // 0 = Inativo, 1 = Ativo
} PSP_EventFlag;

// Adicione isso ao seu arquivo de cabeçalho
typedef struct {
    int uid;
    char name[64];
    int current_count;
    int max_count;
    int init_count;
    int status; // 0 = Morto, 1 = Ativo
} PSP_Sema;

typedef struct {
    uint32_t uid;
    char name[64];
    uint32_t status;
    uint32_t wait_timer; // Ciclos restantes para acordar
    int priority;
    int wait_sema_id;
    uint32_t wait_evflag_bits;
    uint32_t wait_evflag_mode; // PSP_EVENT_WAIT_OR, etc.
    MIPS_CPU context;    // O backup de todos os registradores
} PSP_Thread;

extern PSP_Thread thread_pool[MAX_THREADS];
extern int current_thread_idx;
extern PSP_Sema sema_pool[MAX_SEMAS];


void scheduler_init();
int scheduler_create_thread(const char* name, uint32_t entry_pc, int priority, MIPS_CPU *current_cpu);
void scheduler_change_priority(int uid, int priority, MIPS_CPU *current_cpu);void scheduler_start_thread(int uid);
void scheduler_yield(MIPS_CPU *current_cpu, uint32_t wait_cycles);
void scheduler_exit_thread(MIPS_CPU *current_cpu);

// Protótipos
int scheduler_create_sema(const char* name, int init_count, int max_count);
int scheduler_wait_sema(int sema_id, int signal, MIPS_CPU *cpu);
void scheduler_signal_sema(int sema_id, int signal);
int scheduler_create_evflag(const char *name, int attr, uint32_t init_pattern);
void scheduler_set_evflag(int uid, uint32_t bits, MIPS_CPU *current_cpu);