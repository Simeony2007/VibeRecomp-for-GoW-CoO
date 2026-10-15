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

typedef enum {
    THREAD_WAIT_NONE = 0,
    THREAD_WAIT_THREAD_END,
    THREAD_WAIT_MODULE_START,
    THREAD_WAIT_SEMAPHORE,
    THREAD_WAIT_EVENT_FLAG,
    THREAD_WAIT_DELAY
} PSPThreadWaitType;

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
    bool waiting_for_thread_end;
    int wait_target_thread_id;
    uint32_t wait_return_value;
    uint32_t wait_status_addr;
    PSPThreadWaitType wait_type;
    int wait_semaphore_id;
    int wait_semaphore_count;
    int wait_event_flag_id;
    uint32_t wait_event_flag_bits;
    uint32_t wait_event_flag_mode;
    uint32_t wait_event_flag_pattern;
    uint32_t wait_timeout_addr;
    uint64_t wait_deadline_us;
    uint64_t wait_sequence;
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
void hle_thread_wait_for_module_start(MIPS_CPU *cpu, int thread_id,
                                      uint32_t module_id, uint32_t status_addr);
void run_scheduler(MIPS_CPU *cpu, ThreadState current_new_state);
bool hle_thread_wait_semaphore(MIPS_CPU *cpu, int semaphore_id, int count,
                               uint64_t deadline_us, uint32_t timeout_addr);
bool hle_thread_wait_delay(uint64_t deadline_us);
void hle_thread_wake_semaphore(MIPS_CPU *cpu, int semaphore_id, int *count);
void hle_thread_delete_semaphore_waiters(MIPS_CPU *cpu, int semaphore_id,
                                         uint32_t result);
bool hle_thread_wait_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t bits, uint32_t mode,
                                uint32_t pattern, uint32_t status_addr,
                                uint32_t timeout_addr,
                                uint64_t deadline_us);
void hle_thread_wake_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t *pattern, bool multiple);
void hle_thread_update_event_flag_pattern(int event_flag_id,
                                         uint32_t pattern);
void hle_thread_delete_event_flag_waiters(MIPS_CPU *cpu, int event_flag_id,
                                          uint32_t result);
void hle_scheduler_wake_expired(MIPS_CPU *cpu);
bool hle_scheduler_has_ready_thread(void);
bool hle_scheduler_deadline_after_us(uint32_t usec, uint64_t *deadline_us);
bool hle_scheduler_dispatch_enabled(void);
void hle_scheduler_set_dispatch_enabled(bool enabled);
uint32_t hle_sceKernelSuspendDispatchThread(void);
uint32_t hle_sceKernelResumeDispatchThread(uint32_t enabled);

// HLE Syscall Handlers
void hle_sceKernelCreateThread(MIPS_CPU *cpu);
void hle_sceKernelStartThread(MIPS_CPU *cpu);
void hle_sceKernelExitThread(MIPS_CPU *cpu);
void hle_sceKernelDeleteThread(MIPS_CPU *cpu);
void hle_sceKernelDelayThread(MIPS_CPU *cpu);
void hle_sceKernelGetThreadState(MIPS_CPU *cpu);
void hle_sceKernelGetThreadId(MIPS_CPU *cpu);
void hle_sceKernelGetThreadCurrentPriority(MIPS_CPU *cpu);
void hle_sceKernelChangeThreadPriority(MIPS_CPU *cpu);
void hle_sceKernelSleepThreadCB(MIPS_CPU *cpu);
void hle_sceKernelWaitThreadEndCB(MIPS_CPU *cpu);
uint32_t hle_sceKernelReferThreadStatus(MIPS_CPU *cpu);
void hle_sceKernelCreateFpl(MIPS_CPU *cpu);
void hle_sceKernelDeleteFpl(MIPS_CPU *cpu);
void hle_sceKernelAllocateFpl(MIPS_CPU *cpu);
void hle_sceKernelFreeFpl(MIPS_CPU *cpu);
void hle_sceKernelGetSystemTimeWide(MIPS_CPU *cpu);
void hle_sceKernelGetSystemTimeLow(MIPS_CPU *cpu);
void hle_sceKernelTerminateThread(MIPS_CPU *cpu, bool delete_after);

// Callback Handlers
void hle_sceKernelCreateCallback(MIPS_CPU *cpu);
void hle_sceKernelCheckCallback(MIPS_CPU *cpu);
void hle_sceKernelDeleteCallback(MIPS_CPU *cpu);

#endif // SCE_KERNEL_THREAD_H
