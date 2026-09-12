#ifndef SCE_KERNEL_THREAD_H
#define SCE_KERNEL_THREAD_H

#include "mips_cpu.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef MAX_THREADS
#define MAX_THREADS 128
#endif

#define CB_RETURN_HACK_ADDR 0x08000200

#ifndef THREAD_STATE_DEFINED
#define THREAD_STATE_DEFINED
typedef enum {
    THREAD_STATE_FREE = 0,
    THREAD_STATE_RUNNING,
    THREAD_STATE_READY,
    THREAD_STATE_WAITING,
    THREAD_STATE_DORMANT
} ThreadState;
#endif

#ifndef PSP_THREAD_DEFINED
#define PSP_THREAD_DEFINED
typedef struct {
    int id;
    char name[64];
    uint32_t entry_pc;
    ThreadState state;
    uint32_t regs[32];
    uint32_t pc;
    uint32_t next_pc;
    uint32_t hi;
    uint32_t lo;
    bool suspicious_entry;
} PSPThread;
#endif

#ifndef HLE_CALLBACK_DEFINED
#define HLE_CALLBACK_DEFINED
typedef struct HLECallback {
    int id;
    char name[32];
    uint32_t entrypoint;
    uint32_t common_arg;
    int thread_id;
    int notify_count;
    int notify_arg;
    bool is_active;
} HLECallback;
#endif

#define MAX_CALLBACKS 32
extern HLECallback g_callbacks[MAX_CALLBACKS];
extern int g_next_cb_id;

// Entry point real do modulo principal (definido em main_v35.c), usado como
// fallback quando sceKernelCreateThread recebe um entry_pc == load_bias
// (offset 0 do segmento - nunca e' uma funcao valida, so acontece quando o
// simbolo original ja veio 0 no ELF, tipicamente uma referencia weak nao
// resolvida pelo linker do jogo).
extern uint32_t g_module_entry_point;

// Base (load_bias) e fim do segmento do modulo principal ja carregado.
// Definidas de verdade em main_v35.c (antes eram 'extern' sem definicao em
// lugar nenhum, o que quebrava o link).
extern uint32_t g_binary_segment_start;
extern uint32_t g_binary_segment_end;

extern PSPThread threads[MAX_THREADS];
extern int current_thread_id;
extern int next_thread_id_to_assign;

// Function prototypes for thread & callback management
int hle_create_and_start_thread(MIPS_CPU *cpu, const char *name, uint32_t entry_pc);
void run_scheduler(MIPS_CPU *cpu, ThreadState current_new_state);

// HLE Syscall Handlers
void hle_sceKernelCreateThread(MIPS_CPU *cpu);
void hle_sceKernelStartThread(MIPS_CPU *cpu);
void hle_sceKernelExitThread(MIPS_CPU *cpu);
void hle_sceKernelDeleteThread(MIPS_CPU *cpu);
void hle_sceKernelDelayThread(MIPS_CPU *cpu);
void hle_sceKernelGetThreadState(MIPS_CPU *cpu);
void hle_sceKernelGetThreadCurrentPriority(MIPS_CPU *cpu);
void hle_sceKernelChangeThreadPriority(MIPS_CPU *cpu);
void hle_sceKernelSleepThreadCB(MIPS_CPU *cpu);
void hle_sceKernelWaitThreadEndCB(MIPS_CPU *cpu);

// Callback Handlers
void hle_sceKernelCreateCallback(MIPS_CPU *cpu);
void hle_sceKernelCheckCallback(MIPS_CPU *cpu);
void hle_sceKernelDeleteCallback(MIPS_CPU *cpu);

#endif // SCE_KERNEL_THREAD_H
