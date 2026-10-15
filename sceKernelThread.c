/*
 * PSP Allegrex HLE Thread & Callback Management & Cooperative Scheduler
 */

#include "sceKernelThread.h"
#include "mips_cpu.h"
#include "mips_hle.h"
#include "mips_reloc.h"
#include "sceSysMem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

PSPThread threads[MAX_THREADS] = {0};
int current_thread_id = -1;
int next_thread_id_to_assign = 1;

HLECallback g_callbacks[MAX_CALLBACKS] = {0};
int g_next_cb_id = 1;

extern uint32_t relocated_gp;

#define SCE_KERNEL_ERROR_UNKNOWN_THID 0x80020198u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE 0x800200D2u
#define MAX_FPLS 64
#define SCE_KERNEL_ERROR_UNKNOWN_UID 0x800200CBu
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200D2u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR 0x800200D3u
#define SCE_KERNEL_ERROR_NO_MEMORY 0x80020190u
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK 0x800201B6u
#define SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE 0x800201B7u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT 0x800201A8u
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201A7u
#define SCE_KERNEL_ERROR_ILLEGAL_THID 0x80020197u
#define SCE_KERNEL_ERROR_DORMANT 0x800201A2u
#define SCE_KERNEL_ERROR_THREAD_TERMINATED 0x800201ACu

static uint64_t g_next_wait_sequence = 1;
static uint32_t g_timed_wait_count;
static bool g_dispatch_enabled = true;
static bool g_system_time_started;
static uint64_t g_system_time_origin_us;
static bool g_system_time_replay_initialized;
static bool g_system_time_replay_requested;
static bool g_system_time_replay_failed;
static FILE *g_system_time_replay_file;
static uint64_t g_system_time_replay_calls;

bool hle_scheduler_dispatch_enabled(void) {
    return g_dispatch_enabled;
}

void hle_scheduler_set_dispatch_enabled(bool enabled) {
    g_dispatch_enabled = enabled;
}

uint32_t hle_sceKernelSuspendDispatchThread(void) {
    uint32_t previous = g_dispatch_enabled ? 1u : 0u;
    g_dispatch_enabled = false;
    return previous;
}

uint32_t hle_sceKernelResumeDispatchThread(uint32_t enabled) {
    g_dispatch_enabled = enabled != 0;
    return 0;
}

static bool monotonic_time_us(uint64_t *time_us) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fprintf(stderr, "[HLE Scheduler Error] clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
                strerror(errno));
        return false;
    }
    *time_us = (uint64_t)now.tv_sec * UINT64_C(1000000) +
               (uint64_t)now.tv_nsec / UINT64_C(1000);
    return true;
}

static int replay_system_time_us(uint64_t *time_us) {
    if (!g_system_time_replay_initialized) {
        const char *path = getenv("MIPS_TEST_SYSTEM_TIME_SEQUENCE_FILE");
        g_system_time_replay_initialized = true;
        if (path == NULL || path[0] == '\0') return 0;

        g_system_time_replay_requested = true;
        g_system_time_replay_file = fopen(path, "r");
        if (g_system_time_replay_file == NULL) {
            fprintf(stderr,
                    "[HLE Time Replay Error] Could not open sequence file '%s'.\n",
                    path);
            g_system_time_replay_failed = true;
        } else {
            fprintf(stderr,
                    "[HLE Time Replay] Replaying sceKernelGetSystemTimeWide from '%s'.\n",
                    path);
        }
    }

    if (!g_system_time_replay_requested) return 0;
    if (g_system_time_replay_failed) return -1;

    unsigned long long value;
    if (fscanf(g_system_time_replay_file, "%llx", &value) != 1) {
        fprintf(stderr,
                "[HLE Time Replay Error] Invalid or exhausted sequence after %llu calls.\n",
                (unsigned long long)g_system_time_replay_calls);
        fclose(g_system_time_replay_file);
        g_system_time_replay_file = NULL;
        g_system_time_replay_failed = true;
        return -1;
    }

    g_system_time_replay_calls++;
    *time_us = (uint64_t)value;
    return 1;
}

void hle_sceKernelGetSystemTimeWide(MIPS_CPU *cpu) {
    uint64_t elapsed_us;
    int replay_result = replay_system_time_us(&elapsed_us);
    if (replay_result < 0) {
        cpu->exit_requested = true;
        return;
    }
    if (replay_result == 0) {
        uint64_t now_us;
        if (!monotonic_time_us(&now_us)) {
            cpu->exit_requested = true;
            return;
        }
        if (!g_system_time_started) {
            g_system_time_origin_us = now_us;
            g_system_time_started = true;
        }
        elapsed_us = now_us - g_system_time_origin_us;
    }
    cpu->gpr[0x02] = (uint32_t)elapsed_us;
    cpu->gpr[0x03] = (uint32_t)(elapsed_us >> 32);
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelGetSystemTimeLow(MIPS_CPU *cpu) {
    hle_sceKernelGetSystemTimeWide(cpu);
    cpu->gpr[3] = 0;
}

bool hle_scheduler_deadline_after_us(uint32_t usec, uint64_t *deadline_us) {
    uint64_t now;
    if (deadline_us == NULL) return false;
    if (usec == 0) {
        *deadline_us = 0;
        return true;
    }
    if (!monotonic_time_us(&now)) return false;
    *deadline_us = UINT64_MAX - now < usec ? UINT64_MAX : now + usec;
    return true;
}

static void clear_thread_wait(PSPThread *thread) {
    if (thread->wait_deadline_us != 0 && g_timed_wait_count > 0) {
        g_timed_wait_count--;
    }
    thread->wait_type = THREAD_WAIT_NONE;
    thread->wait_semaphore_id = 0;
    thread->wait_semaphore_count = 0;
    thread->wait_event_flag_id = 0;
    thread->wait_event_flag_bits = 0;
    thread->wait_event_flag_mode = 0;
    thread->wait_event_flag_pattern = 0;
    thread->wait_timeout_addr = 0;
    thread->wait_deadline_us = 0;
    thread->wait_sequence = 0;
    thread->waiting_for_thread_end = false;
    thread->wait_target_thread_id = -1;
    thread->wait_return_value = 0;
    thread->wait_status_addr = 0;
}

static uint32_t update_wait_timeout(MIPS_CPU *cpu, PSPThread *thread,
                                    uint64_t now_us) {
    if (thread->wait_timeout_addr == 0) return 0;
    uint64_t remaining = thread->wait_deadline_us > now_us
        ? thread->wait_deadline_us - now_us : 0;
    uint32_t remaining_us = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    if (mips_get_ptr_range(cpu, thread->wait_timeout_addr, sizeof(uint32_t)) == NULL) {
        printf("[HLE Scheduler Error] Invalid timed-wait output address 0x%08X for thread '%s'\n",
               thread->wait_timeout_addr, thread->name);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    mips_write32(cpu, thread->wait_timeout_addr, remaining_us);
    return 0;
}

void hle_scheduler_wake_expired(MIPS_CPU *cpu) {
    if (cpu == NULL || g_timed_wait_count == 0) return;

    uint64_t now_us;
    if (!monotonic_time_us(&now_us)) return;
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state != THREAD_STATE_WAITING ||
            thread->wait_deadline_us == 0 ||
            thread->wait_deadline_us > now_us) {
            continue;
        }

        PSPThreadWaitType wait_type = thread->wait_type;
        uint32_t timeout_result = update_wait_timeout(cpu, thread, now_us);
        uint32_t result = (wait_type == THREAD_WAIT_SEMAPHORE ||
                           wait_type == THREAD_WAIT_EVENT_FLAG)
            ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
        if (wait_type == THREAD_WAIT_EVENT_FLAG &&
            thread->wait_status_addr != 0 &&
            (thread->wait_status_addr & 3u) == 0 &&
            mips_get_ptr_range(cpu, thread->wait_status_addr,
                               sizeof(uint32_t)) != NULL) {
            mips_write32(cpu, thread->wait_status_addr,
                         thread->wait_event_flag_pattern);
        }
        if (timeout_result != 0) result = timeout_result;
        thread->regs[2] = result;
        clear_thread_wait(thread);
        thread->state = THREAD_STATE_READY;
        printf("[HLE Scheduler] Timed wait expired: thread '%s' (ID: %d), result=0x%08X\n",
               thread->name, thread->id, result);
    }
}

bool hle_scheduler_has_ready_thread(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_READY &&
            threads[i].id != current_thread_id) {
            return true;
        }
    }
    return false;
}

bool hle_thread_wait_semaphore(MIPS_CPU *cpu, int semaphore_id, int count,
                               uint64_t deadline_us, uint32_t timeout_addr) {
    if (cpu == NULL || semaphore_id <= 0 || count <= 0) return false;
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->id != current_thread_id ||
            thread->state != THREAD_STATE_RUNNING) {
            continue;
        }
        thread->wait_type = THREAD_WAIT_SEMAPHORE;
        thread->wait_semaphore_id = semaphore_id;
        thread->wait_semaphore_count = count;
        thread->wait_timeout_addr = timeout_addr;
        thread->wait_deadline_us = deadline_us;
        thread->wait_sequence = g_next_wait_sequence++;
        if (deadline_us != 0) g_timed_wait_count++;
        return true;
    }
    printf("[HLE Scheduler Error] Cannot block sceKernelWaitSema: no RUNNING TCB for thread ID %d\n",
           current_thread_id);
    return false;
}

bool hle_thread_wait_delay(uint64_t deadline_us) {
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->id != current_thread_id ||
            thread->state != THREAD_STATE_RUNNING) {
            continue;
        }

        thread->wait_type = THREAD_WAIT_DELAY;
        thread->wait_deadline_us = deadline_us;
        thread->wait_sequence = g_next_wait_sequence++;
        if (deadline_us != 0) g_timed_wait_count++;
        return true;
    }
    printf("[HLE Scheduler Error] Cannot block sceKernelDelayThread: no RUNNING TCB for thread ID %d\n",
           current_thread_id);
    return false;
}

bool hle_thread_wait_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t bits, uint32_t mode,
                                uint32_t pattern, uint32_t status_addr,
                                uint32_t timeout_addr,
                                uint64_t deadline_us) {
    if (cpu == NULL || event_flag_id <= 0 || bits == 0) return false;
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->id != current_thread_id ||
            thread->state != THREAD_STATE_RUNNING) {
            continue;
        }
        thread->wait_type = THREAD_WAIT_EVENT_FLAG;
        thread->wait_event_flag_id = event_flag_id;
        thread->wait_event_flag_bits = bits;
        thread->wait_event_flag_mode = mode;
        thread->wait_event_flag_pattern = pattern;
        thread->wait_status_addr = status_addr;
        thread->wait_timeout_addr = timeout_addr;
        thread->wait_deadline_us = deadline_us;
        thread->wait_sequence = g_next_wait_sequence++;
        if (deadline_us != 0) g_timed_wait_count++;
        return true;
    }
    printf("[HLE Scheduler Error] Cannot block sceKernelWaitEventFlag: no RUNNING TCB for thread ID %d\n",
           current_thread_id);
    return false;
}

void hle_thread_wake_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t *pattern, bool multiple) {
    if (cpu == NULL || event_flag_id <= 0 || pattern == NULL) return;
    hle_scheduler_wake_expired(cpu);

    for (;;) {
        int selected = -1;
        uint64_t oldest_sequence = UINT64_MAX;
        for (int i = 0; i < MAX_THREADS; i++) {
            PSPThread *thread = &threads[i];
            bool matches = (thread->wait_event_flag_mode & 1u) != 0
                ? ((*pattern & thread->wait_event_flag_bits) != 0)
                : ((*pattern & thread->wait_event_flag_bits) ==
                   thread->wait_event_flag_bits);
            if (thread->state == THREAD_STATE_WAITING &&
                thread->wait_type == THREAD_WAIT_EVENT_FLAG &&
                thread->wait_event_flag_id == event_flag_id &&
                matches && thread->wait_sequence < oldest_sequence) {
                selected = i;
                oldest_sequence = thread->wait_sequence;
            }
        }
        if (selected < 0) return;

        PSPThread *thread = &threads[selected];
        uint32_t bits = thread->wait_event_flag_bits;
        uint32_t mode = thread->wait_event_flag_mode;
        uint32_t result = 0;
        if (thread->wait_status_addr != 0 &&
            (thread->wait_status_addr & 3u) == 0 &&
            mips_get_ptr_range(cpu, thread->wait_status_addr,
                               sizeof(uint32_t)) != NULL) {
            mips_write32(cpu, thread->wait_status_addr, *pattern);
        }
        if ((mode & 0x20u) != 0) *pattern &= ~bits;
        if ((mode & 0x10u) != 0) *pattern = 0;
        if (thread->wait_timeout_addr != 0) {
            uint64_t now_us;
            if (!monotonic_time_us(&now_us)) {
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            } else {
                uint32_t timeout_result = update_wait_timeout(cpu, thread,
                                                             now_us);
                if (timeout_result != 0) result = timeout_result;
            }
        }
        thread->regs[2] = result;
        clear_thread_wait(thread);
        thread->state = THREAD_STATE_READY;
        printf("[HLE Scheduler] Woke event flag waiter '%s' (ID: %d), "
               "event=%d result=0x%08X\n",
               thread->name, thread->id, event_flag_id, result);
        if (!multiple) return;
    }
}

void hle_thread_update_event_flag_pattern(int event_flag_id,
                                          uint32_t pattern) {
    if (event_flag_id <= 0) return;
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state == THREAD_STATE_WAITING &&
            thread->wait_type == THREAD_WAIT_EVENT_FLAG &&
            thread->wait_event_flag_id == event_flag_id) {
            thread->wait_event_flag_pattern = pattern;
        }
    }
}

void hle_thread_delete_event_flag_waiters(MIPS_CPU *cpu, int event_flag_id,
                                          uint32_t result) {
    if (cpu == NULL || event_flag_id <= 0) return;
    hle_scheduler_wake_expired(cpu);
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state != THREAD_STATE_WAITING ||
            thread->wait_type != THREAD_WAIT_EVENT_FLAG ||
            thread->wait_event_flag_id != event_flag_id) {
            continue;
        }
        if (thread->wait_status_addr != 0 &&
            (thread->wait_status_addr & 3u) == 0 &&
            mips_get_ptr_range(cpu, thread->wait_status_addr,
                               sizeof(uint32_t)) != NULL) {
            mips_write32(cpu, thread->wait_status_addr,
                         thread->wait_event_flag_pattern);
        }
        thread->regs[2] = result;
        clear_thread_wait(thread);
        thread->state = THREAD_STATE_READY;
        printf("[HLE Scheduler] Woke event flag waiter '%s' (ID: %d), "
               "event=%d result=0x%08X\n",
               thread->name, thread->id, event_flag_id, result);
    }
}

void hle_thread_wake_semaphore(MIPS_CPU *cpu, int semaphore_id, int *count) {
    if (cpu == NULL || count == NULL || semaphore_id <= 0) return;
    hle_scheduler_wake_expired(cpu);

    for (;;) {
        int selected = -1;
        uint64_t oldest_sequence = UINT64_MAX;
        for (int i = 0; i < MAX_THREADS; i++) {
            PSPThread *thread = &threads[i];
            if (thread->state == THREAD_STATE_WAITING &&
                thread->wait_type == THREAD_WAIT_SEMAPHORE &&
                thread->wait_semaphore_id == semaphore_id &&
                thread->wait_sequence < oldest_sequence) {
                selected = i;
                oldest_sequence = thread->wait_sequence;
            }
        }
        if (selected < 0) return;

        PSPThread *thread = &threads[selected];
        if (thread->wait_semaphore_count > *count) return;
        *count -= thread->wait_semaphore_count;
        uint32_t result = 0;
        if (thread->wait_timeout_addr != 0) {
            uint64_t now_us;
            if (!monotonic_time_us(&now_us)) {
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            } else {
                result = update_wait_timeout(cpu, thread, now_us);
            }
        }
        thread->regs[2] = result;
        clear_thread_wait(thread);
        thread->state = THREAD_STATE_READY;
        printf("[HLE Scheduler] Woke semaphore waiter '%s' (ID: %d), sema=%d result=0x%08X\n",
               thread->name, thread->id, semaphore_id, result);
    }
}

void hle_thread_delete_semaphore_waiters(MIPS_CPU *cpu, int semaphore_id,
                                         uint32_t result) {
    if (cpu == NULL || semaphore_id <= 0) return;
    hle_scheduler_wake_expired(cpu);

    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state != THREAD_STATE_WAITING ||
            thread->wait_type != THREAD_WAIT_SEMAPHORE ||
            thread->wait_semaphore_id != semaphore_id) {
            continue;
        }
        thread->regs[2] = result;
        clear_thread_wait(thread);
        thread->state = THREAD_STATE_READY;
        printf("[HLE Scheduler] Woke semaphore waiter '%s' (ID: %d), "
               "sema=%d result=0x%08X\n",
               thread->name, thread->id, semaphore_id, result);
    }
}

static uint64_t earliest_wait_deadline(void) {
    uint64_t earliest = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state == THREAD_STATE_WAITING &&
            thread->wait_deadline_us != 0 &&
            (earliest == 0 || thread->wait_deadline_us < earliest)) {
            earliest = thread->wait_deadline_us;
        }
    }
    return earliest;
}

static bool wait_for_deadline(uint64_t deadline_us) {
    uint64_t now_us;
    if (!monotonic_time_us(&now_us)) return false;
    if (deadline_us <= now_us) return true;

    uint64_t remaining_us = deadline_us - now_us;
    struct timespec remaining = {
        .tv_sec = (time_t)(remaining_us / UINT64_C(1000000)),
        .tv_nsec = (long)((remaining_us % UINT64_C(1000000)) * UINT64_C(1000))
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "[HLE Scheduler Error] nanosleep for timed wait failed: %s\n",
                strerror(errno));
        return false;
    }
    return true;
}

typedef struct {
    int id;
    char name[32];
    uint32_t partition_id;
    uint32_t block_size;
    uint32_t aligned_size;
    uint32_t block_count;
    uint32_t address;
    bool active;
    uint8_t *allocated;
} HLE_Fpl;

static HLE_Fpl g_fpls[MAX_FPLS];

static void fpl_return(MIPS_CPU *cpu, uint32_t result) {
    cpu->gpr[0x02] = result;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

static HLE_Fpl *fpl_find(uint32_t id) {
    for (int i = 0; i < MAX_FPLS; i++) {
        if (g_fpls[i].active && (uint32_t)g_fpls[i].id == id) {
            return &g_fpls[i];
        }
    }
    return NULL;
}

static uint32_t fpl_allocate_block(MIPS_CPU *cpu, HLE_Fpl *fpl,
                                   uint32_t output_ptr) {
    if (mips_get_ptr_range(cpu, output_ptr, sizeof(uint32_t)) == NULL) {
        printf("[HLE ThreadMan Error] sceKernelAllocateFpl: invalid output pointer 0x%08X\n",
               output_ptr);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    for (uint32_t i = 0; i < fpl->block_count; i++) {
        if (fpl->allocated[i] == 0) {
            uint32_t block_address = fpl->address + i * fpl->aligned_size;
            fpl->allocated[i] = 1;
            mips_write32(cpu, output_ptr, block_address);
            return 0;
        }
    }
    printf("[HLE ThreadMan Error] sceKernelAllocateFpl: pool %d has no free blocks\n",
           fpl->id);
    return SCE_KERNEL_ERROR_NO_MEMORY;
}

static void write_thread_exit_status(MIPS_CPU *cpu, uint32_t status_addr,
                                     uint32_t result, const char *operation) {
    if (status_addr == 0) return;
    if (mips_get_ptr_range(cpu, status_addr, sizeof(uint32_t)) == NULL) {
        printf("[HLE ThreadMan Error] %s: invalid status output address 0x%08X\n",
               operation, status_addr);
        return;
    }
    mips_write32(cpu, status_addr, result);
}

static void wake_thread_end_waiters(MIPS_CPU *cpu, int target_thread_id) {
    uint32_t target_result = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].id == target_thread_id) {
            target_result = threads[i].regs[2];
            break;
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        PSPThread *thread = &threads[i];
        if (thread->state == THREAD_STATE_WAITING &&
            thread->waiting_for_thread_end &&
            thread->wait_target_thread_id == target_thread_id) {
            write_thread_exit_status(cpu, thread->wait_status_addr,
                                     target_result, "thread-end wait");
            uint32_t wait_return_value = thread->wait_return_value;
            if (thread->wait_type == THREAD_WAIT_MODULE_START) {
                printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                       "action=start_completed thread=%d status=0x%08X\n",
                       wait_return_value, target_thread_id, target_result);
            }
            clear_thread_wait(thread);
            thread->regs[2] = wait_return_value;
            thread->state = THREAD_STATE_READY;
            printf("[HLE Scheduler] Woke thread '%s' (ID: %d): waited-for thread %d terminated.\n",
                   thread->name, thread->id, target_thread_id);
        }
    }
}

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
    clear_thread_wait(&threads[slot]);
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
    threads[slot].waiting_for_thread_end = false;
    threads[slot].wait_target_thread_id = -1;
    g_dispatch_enabled = true;

    printf("[HLE ThreadMan] Created and started internal module thread '%s' (ID: %d, Entry: 0x%08X)\n", name, thid, entry_pc);
    return thid;
}

void hle_thread_wait_for_module_start(MIPS_CPU *cpu, int thread_id,
                                      uint32_t module_id, uint32_t status_addr) {
    PSPThread *target = NULL;
    PSPThread *waiter = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].id == thread_id) target = &threads[i];
        if (threads[i].id == current_thread_id &&
            threads[i].state == THREAD_STATE_RUNNING) {
            waiter = &threads[i];
        }
    }

    if (target == NULL) {
        printf("[HLE ThreadMan Error] sceKernelStartModule: start thread %d disappeared before wait\n",
               thread_id);
        cpu->gpr[2] = SCE_KERNEL_ERROR_UNKNOWN_THID;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (target->state == THREAD_STATE_FREE) {
        write_thread_exit_status(cpu, status_addr, target->regs[2],
                                 "sceKernelStartModule");
        cpu->gpr[2] = module_id;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (!g_dispatch_enabled) {
        printf("[HLE ThreadMan Error] sceKernelStartModule: dispatch is suspended; cannot wait for module %u\n",
               module_id);
        cpu->gpr[2] = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (waiter == NULL) {
        printf("[HLE ThreadMan Error] sceKernelStartModule: current thread %d has no RUNNING TCB\n",
               current_thread_id);
        cpu->gpr[2] = SCE_KERNEL_ERROR_UNKNOWN_THID;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    waiter->waiting_for_thread_end = true;
    waiter->wait_target_thread_id = thread_id;
    waiter->wait_return_value = module_id;
    waiter->wait_status_addr = status_addr;
    waiter->wait_type = THREAD_WAIT_MODULE_START;
    cpu->gpr[2] = module_id;
    cpu->pc = cpu->gpr[31];
    cpu->next_pc = cpu->pc + 4;
    printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X waiting for start thread %d\n",
           module_id, thread_id);
    run_scheduler(cpu, THREAD_STATE_WAITING);
}

void run_scheduler(MIPS_CPU *cpu, ThreadState current_new_state) {
    if (!g_dispatch_enabled && current_new_state == THREAD_STATE_READY) {
        return;
    }

    int completed_thread_id = current_new_state == THREAD_STATE_FREE
        ? current_thread_id : -1;

    // 1. Save current context and update its state
    if (current_thread_id != -1) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id && threads[i].state == THREAD_STATE_RUNNING) {
                if (cpu != NULL) {
                    cpu->scheduler_saved_v0 = cpu->gpr[2];
                    cpu->scheduler_saved_v0_valid = true;
                }
                for (int r = 0; r < 32; r++) {
                    threads[i].regs[r] = cpu->gpr[r];
                }
                threads[i].pc = cpu->pc;
                threads[i].next_pc = cpu->next_pc;
                threads[i].hi = cpu->hi;
                threads[i].lo = cpu->lo;
                threads[i].state = current_new_state;
                if (current_new_state == THREAD_STATE_FREE) {
                    clear_thread_wait(&threads[i]);
                }
                printf("[HLE Scheduler TCB] Saved Thread '%s' (ID: %d): PC=0x%08X, RA=0x%08X, SP=0x%08X -> State: %d\n", 
                       threads[i].name, threads[i].id, threads[i].pc, threads[i].regs[31], threads[i].regs[29], current_new_state);
                break;
            }
        }
    }

    if (completed_thread_id != -1) {
        wake_thread_end_waiters(cpu, completed_thread_id);
    }

    hle_scheduler_wake_expired(cpu);

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
    for (;;) {
        for (int i = 0; i < MAX_THREADS; i++) {
            int idx = (start_idx + i) % MAX_THREADS;
            if (threads[idx].state == THREAD_STATE_READY) {
                next_idx = idx;
                break;
            }
        }
        if (next_idx != -1) break;

        uint64_t deadline_us = earliest_wait_deadline();
        if (deadline_us == 0) break;
        if (!wait_for_deadline(deadline_us)) break;
        hle_scheduler_wake_expired(cpu);
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
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].state == THREAD_STATE_WAITING) {
                has_waiting = true;
            }
        }

        current_thread_id = -1;
        cpu->pc = 0x08000300;
        cpu->next_pc = 0x08000304;
        if (has_waiting) {
            printf("[HLE Scheduler Error] No READY threads; refusing to wake "
                   "WAITING threads without a matching event. Remaining waits:\n");
            for (int i = 0; i < MAX_THREADS; i++) {
                if (threads[i].state == THREAD_STATE_WAITING) {
                    printf("[HLE Scheduler Error] Thread '%s' (ID: %d) "
                           "wait_type=%d waiting_for_thread_end=%s target=%d sema=%d count=%d deadline_us=%llu\n",
                           threads[i].name, threads[i].id,
                           (int)threads[i].wait_type,
                           threads[i].waiting_for_thread_end ? "yes" : "no",
                           threads[i].wait_target_thread_id,
                           threads[i].wait_semaphore_id,
                           threads[i].wait_semaphore_count,
                           (unsigned long long)threads[i].wait_deadline_us);
                }
            }
        } else {
            printf("[HLE Scheduler] All emulated threads have exited.\n");
        }
        cpu->exit_requested = true;
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
    g_dispatch_enabled = true;
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

    /*
     * Do not replace small-but-valid entry points with the module entry.
     * Some homebrew ELFs intentionally start a worker at an address close to
     * the first loaded segment (Cave Story uses 0x0890003C).  The loader has
     * already validated the segment bounds; only the exact segment base is
     * treated as the unresolved-pointer sentinel below.
     */
    

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
        clear_thread_wait(&threads[slot]);
        threads[slot].id = next_thread_id_to_assign++;
        strncpy(threads[slot].name, name, sizeof(threads[slot].name) - 1);
        threads[slot].entry_pc = entry_pc;
        threads[slot].state = THREAD_STATE_DORMANT;
        memset(threads[slot].regs, 0, sizeof(threads[slot].regs));
        threads[slot].waiting_for_thread_end = false;
        threads[slot].wait_target_thread_id = -1;
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
    threads[slot].waiting_for_thread_end = false;
    threads[slot].wait_target_thread_id = -1;
    g_dispatch_enabled = true;

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
            wake_thread_end_waiters(cpu, threads[i].id);
            clear_thread_wait(&threads[i]);
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

void hle_sceKernelTerminateThread(MIPS_CPU *cpu, bool delete_after) {
    uint32_t thread_id = cpu->gpr[4];
    uint32_t result = SCE_KERNEL_ERROR_UNKNOWN_THID;
    PSPThread *target = NULL;

    if (thread_id == 0 || thread_id == (uint32_t)current_thread_id) {
        result = SCE_KERNEL_ERROR_ILLEGAL_THID;
    } else if (!g_dispatch_enabled) {
        result = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    } else {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id >= 0 && (uint32_t)threads[i].id == thread_id &&
                threads[i].state != THREAD_STATE_FREE) {
                target = &threads[i];
                break;
            }
        }
        if (target == NULL) {
            result = SCE_KERNEL_ERROR_UNKNOWN_THID;
        } else if (target->state == THREAD_STATE_DORMANT) {
            result = SCE_KERNEL_ERROR_DORMANT;
        } else {
            target->regs[2] = SCE_KERNEL_ERROR_THREAD_TERMINATED;
            clear_thread_wait(target);
            target->state = THREAD_STATE_DORMANT;
            wake_thread_end_waiters(cpu, target->id);
            result = 0;
            if (delete_after) {
                clear_thread_wait(target);
                memset(target, 0, sizeof(*target));
                target->state = THREAD_STATE_FREE;
            }
            printf("[HLE ThreadMan] sceKernelTerminate%sThread: thread %u %s\n",
                   delete_after ? "Delete" : "",
                   thread_id, delete_after ? "terminated and deleted" : "terminated");
        }
    }

    cpu->gpr[2] = result;
    cpu->pc = cpu->gpr[31];
    cpu->next_pc = cpu->pc + 4;
}

// HLE Syscall Handler for sceKernelDelayThread (Syscall 0x22224 / NID 0x9ACE131E)
void hle_sceKernelDelayThread(MIPS_CPU *cpu) {
    uint32_t usec = cpu->gpr[0x04];
    printf("[HLE ThreadMan] sceKernelDelayThread: delaying %u us\n", usec);
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
    if (!g_dispatch_enabled && usec != 0) {
        cpu->gpr[0x02] = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
        return;
    }
    cpu->gpr[0x02] = 0;
    if (usec == 0) {
        run_scheduler(cpu, THREAD_STATE_READY);
        return;
    }
    uint64_t deadline_us;
    if (!hle_scheduler_deadline_after_us(usec, &deadline_us) ||
        !hle_thread_wait_delay(deadline_us)) {
        cpu->gpr[0x02] = SCE_KERNEL_ERROR_UNKNOWN_THID;
        return;
    }
    run_scheduler(cpu, THREAD_STATE_WAITING);
}

void hle_sceKernelGetThreadState(MIPS_CPU *cpu) {
    printf("[HLE ThreadMan] sceKernelGetThreadState -> Returning THREAD_STATE_READY\n");
    cpu->gpr[0x02] = 1;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

void hle_sceKernelGetThreadId(MIPS_CPU *cpu) {
    cpu->gpr[0x02] = (uint32_t)current_thread_id;
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
    if (!g_dispatch_enabled) {
        cpu->gpr[0x02] = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
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

// sceKernelWaitThreadEnd (NID 0x278C0DF5)
// Suspende a chamadora até o alvo terminar; run_scheduler a acorda no
// momento em que o TCB do alvo passa para THREAD_STATE_FREE.
void hle_sceKernelWaitThreadEndCB(MIPS_CPU *cpu) {
    uint32_t thid   = cpu->gpr[0x04]; // $a0 = thread ID alvo
    uint32_t status = cpu->gpr[0x05]; // $a1 = ponteiro para status de saida (pode ser NULL)

    bool target_done = false;
    bool target_found = thid == 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if ((uint32_t)threads[i].id == thid) {
            target_found = true;
            if (threads[i].state == THREAD_STATE_FREE ||
                threads[i].state == THREAD_STATE_DORMANT) {
                target_done = true;
            }
            break;
        }
    }

    if (target_done || thid == 0) {
        printf("[HLE ThreadMan] sceKernelWaitThreadEndCB(thid=%u): thread alvo ja terminou -> retornando imediatamente\n", thid);
        if (target_done) {
            for (int i = 0; i < MAX_THREADS; i++) {
                if ((uint32_t)threads[i].id == thid) {
                    write_thread_exit_status(cpu, status, threads[i].regs[2],
                                             "sceKernelWaitThreadEndCB");
                    break;
                }
            }
        }
        cpu->gpr[0x02] = 0; // PSP_OK
        cpu->pc        = cpu->gpr[0x1F];
        cpu->next_pc   = cpu->pc + 4;
    } else if (!target_found) {
        printf("[HLE ThreadMan Error] sceKernelWaitThreadEndCB: unknown thread ID %u\n",
               thid);
        cpu->gpr[0x02] = SCE_KERNEL_ERROR_UNKNOWN_THID;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
    } else {
        if (!g_dispatch_enabled) {
            cpu->gpr[0x02] = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
            cpu->pc = cpu->gpr[0x1F];
            cpu->next_pc = cpu->pc + 4;
            return;
        }
        printf("[HLE ThreadMan] sceKernelWaitThreadEndCB(thid=%u): thread alvo ainda ativa -> colocando chamadora (ID %d) em WAITING\n",
               thid, current_thread_id);
        // Salva a continuação após a syscall e associa a espera ao alvo.
        cpu->pc      = cpu->gpr[0x1F]; // retorna para o caller quando acordar
        cpu->next_pc = cpu->pc + 4;
        cpu->gpr[0x02] = 0; // PSP_OK
        bool waiter_registered = false;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id &&
                threads[i].state == THREAD_STATE_RUNNING) {
                threads[i].waiting_for_thread_end = true;
                threads[i].wait_target_thread_id = (int)thid;
                threads[i].wait_return_value = 0;
                threads[i].wait_status_addr = status;
                threads[i].wait_type = THREAD_WAIT_THREAD_END;
                waiter_registered = true;
                break;
            }
        }
        if (!waiter_registered) {
            printf("[HLE ThreadMan Error] sceKernelWaitThreadEndCB: "
                   "current thread ID %d has no RUNNING TCB\n",
                   current_thread_id);
            cpu->gpr[0x02] = SCE_KERNEL_ERROR_UNKNOWN_THID;
            return;
        }
        run_scheduler(cpu, THREAD_STATE_WAITING);
    }
}

uint32_t hle_sceKernelReferThreadStatus(MIPS_CPU *cpu) {
    enum {
        PSP_THREAD_STATUS_SIZE = 108,
        PSP_THREAD_STATUS_NATIVE_SIZE = 104,
        PSP_THREAD_STATUS_RUNNING = 1,
        PSP_THREAD_STATUS_READY = 2,
        PSP_THREAD_STATUS_WAITING = 4,
        PSP_THREAD_STATUS_DORMANT = 16
    };

    uint32_t thread_id = cpu->gpr[4];
    uint32_t status_addr = cpu->gpr[5];
    uint8_t status[PSP_THREAD_STATUS_SIZE] = {0};
    PSPThread *thread = NULL;

    if (thread_id == 0) thread_id = (uint32_t)current_thread_id;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].id >= 0 && (uint32_t)threads[i].id == thread_id) {
            thread = &threads[i];
            break;
        }
    }
    if (thread == NULL) {
        printf("[HLE ThreadMan Error] sceKernelReferThreadStatus: unknown thread ID %u\n",
               thread_id);
        return SCE_KERNEL_ERROR_UNKNOWN_THID;
    }
    if ((status_addr & 3u) != 0 ||
        mips_get_ptr_range(cpu, status_addr, sizeof(uint32_t)) == NULL) {
        printf("[HLE ThreadMan Error] sceKernelReferThreadStatus: invalid status pointer 0x%08X\n",
               status_addr);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }

    uint32_t requested_size = mips_read32(cpu, status_addr);
    if (requested_size > PSP_THREAD_STATUS_SIZE) {
        printf("[HLE ThreadMan Error] sceKernelReferThreadStatus: unsupported status size %u\n",
               requested_size);
        return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    }
    if (requested_size == 0) return 0;
    if (mips_get_ptr_range(cpu, status_addr, requested_size) == NULL) {
        printf("[HLE ThreadMan Error] sceKernelReferThreadStatus: status buffer 0x%08X+%u is invalid\n",
               status_addr, requested_size);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }

    uint32_t psp_state = PSP_THREAD_STATUS_DORMANT;
    switch (thread->state) {
        case THREAD_STATE_RUNNING:
            psp_state = PSP_THREAD_STATUS_RUNNING;
            break;
        case THREAD_STATE_READY:
            psp_state = PSP_THREAD_STATUS_READY;
            break;
        case THREAD_STATE_WAITING:
            psp_state = PSP_THREAD_STATUS_WAITING;
            break;
        case THREAD_STATE_FREE:
        case THREAD_STATE_DORMANT:
            psp_state = PSP_THREAD_STATUS_DORMANT;
            break;
    }

    uint32_t wait_type = 0;
    uint32_t wait_id = 0;
    switch (thread->wait_type) {
        case THREAD_WAIT_THREAD_END:
        case THREAD_WAIT_MODULE_START:
            wait_type = 9;
            wait_id = (uint32_t)thread->wait_target_thread_id;
            break;
        case THREAD_WAIT_SEMAPHORE:
            wait_type = 3;
            wait_id = (uint32_t)thread->wait_semaphore_id;
            break;
        case THREAD_WAIT_EVENT_FLAG:
            wait_type = 4;
            wait_id = (uint32_t)thread->wait_event_flag_id;
            break;
        case THREAD_WAIT_DELAY:
            wait_type = 2;
            break;
        case THREAD_WAIT_NONE:
            break;
    }

    uint32_t fields[] = {
        PSP_THREAD_STATUS_SIZE, 0, psp_state, thread->entry_pc,
        0, 0, thread->regs[28], 0x20, 0x20, wait_type,
        wait_id, 0,
        (thread->state == THREAD_STATE_FREE ||
         thread->state == THREAD_STATE_DORMANT) ? thread->regs[2] : 0,
        0, 0, 0, 0, 0
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t offset = i == 0 ? 0 : 36 + (i - 1) * sizeof(uint32_t);
        if (offset + sizeof(uint32_t) <= PSP_THREAD_STATUS_NATIVE_SIZE) {
            for (size_t byte = 0; byte < sizeof(uint32_t); byte++) {
                status[offset + byte] =
                    (uint8_t)(fields[i] >> (byte * 8));
            }
        }
    }
    memcpy(status + 4, thread->name, sizeof(thread->name) < 32
           ? sizeof(thread->name) : 31);
    memcpy(mips_get_ptr_range(cpu, status_addr, requested_size),
           status, requested_size);
    return 0;
}

void hle_sceKernelCreateFpl(MIPS_CPU *cpu) {
    uint32_t name_ptr = cpu->gpr[0x04];
    uint32_t partition_id = cpu->gpr[0x05];
    uint32_t attr = cpu->gpr[0x06];
    uint32_t block_size = cpu->gpr[0x07];
    uint32_t stack_args = cpu->gpr[29] + 16;
    uint32_t block_count = mips_read32(cpu, stack_args);
    uint32_t option_ptr = mips_read32(cpu, stack_args + 4);
    if (block_count == 0 && cpu->gpr[0x08] != 0) {
        // This EBOOT passes arguments five and six in t0/t1 for this NID.
        block_count = cpu->gpr[0x08];
        option_ptr = cpu->gpr[0x09];
    }
    char name[32] = {0};

    if (name_ptr == 0 || !safe_read_cstring(cpu, name_ptr, name, sizeof(name))) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: invalid name pointer 0x%08X\n",
               name_ptr);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        return;
    }
    if (partition_id < 1 || partition_id > 6) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: invalid partition %u\n",
               partition_id);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        return;
    }
    if (block_size == 0 || block_count == 0) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: invalid block size/count (%u/%u)\n",
               block_size, block_count);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE);
        return;
    }

    uint32_t alignment = 4;
    if (option_ptr != 0) {
        if (mips_get_ptr_range(cpu, option_ptr, 8) == NULL) {
            printf("[HLE ThreadMan Error] sceKernelCreateFpl: invalid option pointer 0x%08X\n",
                   option_ptr);
            fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            return;
        }
        uint32_t option_size = mips_read32(cpu, option_ptr);
        if (option_size >= 4) {
            alignment = mips_read32(cpu, option_ptr + 4);
            if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
                printf("[HLE ThreadMan Error] sceKernelCreateFpl: invalid alignment %u\n",
                       alignment);
                fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
                return;
            }
        }
    }
    if (alignment < 4) alignment = 4;

    uint64_t aligned_size =
        ((uint64_t)block_size + alignment - 1) & ~((uint64_t)alignment - 1);
    uint64_t total_size = aligned_size * block_count;
    if (aligned_size > UINT32_MAX || total_size == 0 ||
        total_size > PSP_RAM_SIZE) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: pool size overflows (%u x %u)\n",
               block_size, block_count);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE);
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FPLS; i++) {
        if (!g_fpls[i].active) {
            slot = i;
            break;
        }
    }
    uint64_t pool_address =
        ((uint64_t)g_next_partition_alloc_addr + 15) & ~UINT64_C(15);
    uint64_t pool_end = pool_address + total_size;
    if (slot < 0 || pool_end > 0x0C000000 ||
        mips_get_ptr_range(cpu, (uint32_t)pool_address, (size_t)total_size) == NULL) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: no emulated RAM for %llu bytes\n",
               (unsigned long long)total_size);
        fpl_return(cpu, SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    uint8_t *allocated = calloc(block_count, sizeof(*allocated));
    if (allocated == NULL) {
        printf("[HLE ThreadMan Error] sceKernelCreateFpl: cannot allocate pool metadata\n");
        fpl_return(cpu, SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    HLE_Fpl *fpl = &g_fpls[slot];
    memset(fpl, 0, sizeof(*fpl));
    fpl->id = g_next_block_id++;
    strncpy(fpl->name, name, sizeof(fpl->name) - 1);
    fpl->partition_id = partition_id;
    fpl->block_size = block_size;
    fpl->aligned_size = (uint32_t)aligned_size;
    fpl->block_count = block_count;
    fpl->address = (uint32_t)pool_address;
    fpl->allocated = allocated;
    fpl->active = true;
    g_next_partition_alloc_addr = (uint32_t)pool_end;

    printf("[HLE ThreadMan] sceKernelCreateFpl('%s', partition=%u, attr=0x%08X, "
           "block=%u, count=%u) -> ID %d, address=0x%08X\n",
           fpl->name, partition_id, attr, block_size, block_count,
           fpl->id, fpl->address);
    fpl_return(cpu, (uint32_t)fpl->id);
}

void hle_sceKernelDeleteFpl(MIPS_CPU *cpu) {
    uint32_t id = cpu->gpr[0x04];
    HLE_Fpl *fpl = fpl_find(id);
    if (fpl == NULL) {
        printf("[HLE ThreadMan Error] sceKernelDeleteFpl: unknown ID %u\n", id);
        fpl_return(cpu, SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    printf("[HLE ThreadMan] sceKernelDeleteFpl(ID %u, '%s')\n", id, fpl->name);
    free(fpl->allocated);
    memset(fpl, 0, sizeof(*fpl));
    fpl_return(cpu, 0);
}

void hle_sceKernelAllocateFpl(MIPS_CPU *cpu) {
    uint32_t id = cpu->gpr[0x04];
    uint32_t output_ptr = cpu->gpr[0x05];
    HLE_Fpl *fpl = fpl_find(id);
    if (fpl == NULL) {
        printf("[HLE ThreadMan Error] sceKernelAllocateFpl: unknown ID %u\n", id);
        fpl_return(cpu, SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    uint32_t result = fpl_allocate_block(cpu, fpl, output_ptr);
    fpl_return(cpu, result);
}

void hle_sceKernelFreeFpl(MIPS_CPU *cpu) {
    uint32_t id = cpu->gpr[0x04];
    uint32_t block_address = cpu->gpr[0x05];
    HLE_Fpl *fpl = fpl_find(id);
    if (fpl == NULL) {
        printf("[HLE ThreadMan Error] sceKernelFreeFpl: unknown ID %u\n", id);
        fpl_return(cpu, SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    uint64_t pool_end = (uint64_t)fpl->address +
                        (uint64_t)fpl->aligned_size * fpl->block_count;
    if (block_address < fpl->address || block_address >= pool_end ||
        (block_address - fpl->address) % fpl->aligned_size != 0) {
        printf("[HLE ThreadMan Error] sceKernelFreeFpl: invalid block address 0x%08X\n",
               block_address);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK);
        return;
    }
    uint32_t index = (block_address - fpl->address) / fpl->aligned_size;
    if (fpl->allocated[index] == 0) {
        printf("[HLE ThreadMan Error] sceKernelFreeFpl: block 0x%08X was not allocated\n",
               block_address);
        fpl_return(cpu, SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK);
        return;
    }
    fpl->allocated[index] = 0;
    fpl_return(cpu, 0);
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
