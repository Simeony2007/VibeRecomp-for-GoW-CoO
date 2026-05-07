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

typedef struct {
    uint32_t uid;
    char name[64];
    uint32_t status;
    uint32_t wait_timer; // Ciclos restantes para acordar
    MIPS_CPU context;    // O backup de todos os registradores
} PSP_Thread;

extern PSP_Thread thread_pool[MAX_THREADS];
extern int current_thread_idx;

void scheduler_init();
int scheduler_create_thread(const char* name, uint32_t entry_pc, MIPS_CPU *current_cpu);
void scheduler_start_thread(int uid);
void scheduler_yield(MIPS_CPU *current_cpu, uint32_t wait_cycles);
void scheduler_exit_thread(MIPS_CPU *current_cpu);