#include "mips_cpu.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif

typedef struct {
    FILE *file;
    bool initialized;
    bool enabled;
    bool include_fp;
    bool include_memory;
    bool include_all_hle;
    bool stop_at_max_steps;
    bool watch_configured;
    bool watch_only;
    bool watch_trace_configured;
    bool watch_trace_started;
    bool watch_nan_writes;
    bool watch_nan_address_configured;
    bool watch_nan_value_configured;
    bool memory_watch_configured;
    bool has_start;
    bool has_end;
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t watch_pc;
    uint32_t memory_watch_addr;
    uint32_t memory_watch_size;
    uint32_t watch_nan_addr;
    uint32_t watch_nan_size;
    uint32_t watch_nan_value;
    uint64_t watch_every;
    uint64_t watch_samples_limit;
    uint64_t watch_visits;
    uint64_t watch_samples;
    uint64_t watch_trace_after;
    uint64_t watch_trace_steps;
    uint64_t watch_trace_remaining;
    uint64_t watch_nan_write_limit;
    uint64_t watch_nan_writes_seen;
    uint64_t max_steps;
    uint64_t step_sequence;
    uint64_t emitted_steps;
} MIPS_TraceState;

static MIPS_TraceState g_trace;

static uint64_t trace_parse_u64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') return fallback;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (end == value || *end != '\0') {
        fprintf(stderr, "[TRACE ERROR] Valor invalido em %s: '%s'\n", name, value);
        return fallback;
    }
    return (uint64_t)parsed;
}

static bool trace_env_enabled(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

static void trace_close(void) {
    if (g_trace.file != NULL) {
        fprintf(g_trace.file,
                "TRACE END steps=%" PRIu64 " seq=%" PRIu64 " enabled=%u\n",
                g_trace.emitted_steps, g_trace.step_sequence,
                g_trace.enabled ? 1u : 0u);
        fflush(g_trace.file);
        fclose(g_trace.file);
        g_trace.file = NULL;
    }
}

static void trace_initialize(MIPS_CPU *cpu) {
    if (g_trace.initialized) {
        if (cpu != NULL) {
            cpu->trace_enabled = g_trace.enabled;
            cpu->trace_memory_enabled =
                g_trace.enabled &&
                (g_trace.include_memory || g_trace.memory_watch_configured ||
                 g_trace.watch_nan_writes);
        }
        return;
    }
    g_trace.initialized = true;

    const char *path = getenv("MIPS_TRACE_FILE");
    if (path == NULL || *path == '\0') return;

    g_trace.file = fopen(path, "w");
    if (g_trace.file == NULL) {
        fprintf(stderr, "[TRACE ERROR] Nao foi possivel abrir '%s' para escrita.\n", path);
        return;
    }
    setvbuf(g_trace.file, NULL, _IOLBF, 0);

    g_trace.enabled = true;
    g_trace.include_fp = trace_env_enabled("MIPS_TRACE_FP");
    g_trace.include_memory = trace_env_enabled("MIPS_TRACE_MEMORY");
    g_trace.include_all_hle = trace_env_enabled("MIPS_TRACE_HLE_ALL");
    g_trace.watch_nan_writes = trace_env_enabled("MIPS_TRACE_WATCH_NAN");
    g_trace.watch_nan_write_limit =
        trace_parse_u64("MIPS_TRACE_WATCH_NAN_LIMIT", 0);
    const char *watch_nan_value = getenv("MIPS_TRACE_WATCH_NAN_VALUE");
    if (watch_nan_value != NULL && *watch_nan_value != '\0') {
        g_trace.watch_nan_value = (uint32_t)trace_parse_u64(
            "MIPS_TRACE_WATCH_NAN_VALUE", 0);
        g_trace.watch_nan_value_configured = true;
    }
    const char *watch_nan_addr = getenv("MIPS_TRACE_WATCH_NAN_ADDR");
    if (watch_nan_addr != NULL && *watch_nan_addr != '\0') {
        g_trace.watch_nan_addr =
            (uint32_t)trace_parse_u64("MIPS_TRACE_WATCH_NAN_ADDR", 0);
        g_trace.watch_nan_size =
            (uint32_t)trace_parse_u64("MIPS_TRACE_WATCH_NAN_SIZE",
                                      sizeof(uint32_t));
        if (g_trace.watch_nan_size == 0 ||
            (uint64_t)g_trace.watch_nan_addr + g_trace.watch_nan_size >
                UINT32_MAX + UINT64_C(1)) {
            fprintf(stderr,
                    "[TRACE ERROR] Faixa invalida em "
                    "MIPS_TRACE_WATCH_NAN_ADDR/SIZE.\n");
            g_trace.watch_nan_size = 0;
        } else {
            g_trace.watch_nan_address_configured = true;
        }
    } else if (getenv("MIPS_TRACE_WATCH_NAN_SIZE") != NULL) {
        fprintf(stderr,
                "[TRACE ERROR] MIPS_TRACE_WATCH_NAN_SIZE requer "
                "MIPS_TRACE_WATCH_NAN_ADDR.\n");
    }
    const char *memory_watch_addr = getenv("MIPS_TRACE_WATCH_ADDR");
    if (memory_watch_addr != NULL && *memory_watch_addr != '\0') {
        g_trace.memory_watch_addr =
            (uint32_t)trace_parse_u64("MIPS_TRACE_WATCH_ADDR", 0);
        g_trace.memory_watch_size =
            (uint32_t)trace_parse_u64("MIPS_TRACE_WATCH_SIZE", sizeof(uint32_t));
        if (g_trace.memory_watch_size == 0 ||
            (uint64_t)g_trace.memory_watch_addr + g_trace.memory_watch_size >
                UINT32_MAX + UINT64_C(1)) {
            fprintf(stderr, "[TRACE ERROR] Faixa invalida em MIPS_TRACE_WATCH_ADDR/SIZE.\n");
            g_trace.memory_watch_size = 0;
        } else {
            g_trace.memory_watch_configured = true;
        }
    }
    g_trace.max_steps = trace_parse_u64("MIPS_TRACE_MAX", 0);
    g_trace.stop_at_max_steps = trace_env_enabled("MIPS_TRACE_STOP");
    g_trace.watch_only = trace_env_enabled("MIPS_TRACE_WATCH_ONLY");
    const char *watch_pc = getenv("MIPS_TRACE_WATCH_PC");
    if (watch_pc != NULL && *watch_pc != '\0') {
        g_trace.watch_pc = (uint32_t)trace_parse_u64("MIPS_TRACE_WATCH_PC", 0);
        g_trace.watch_every = trace_parse_u64("MIPS_TRACE_WATCH_EVERY", 100000);
        g_trace.watch_samples_limit =
            trace_parse_u64("MIPS_TRACE_WATCH_SAMPLES", 0);
        g_trace.watch_trace_after =
            trace_parse_u64("MIPS_TRACE_WATCH_TRACE_AFTER", 0);
        g_trace.watch_trace_steps =
            trace_parse_u64("MIPS_TRACE_WATCH_TRACE_STEPS", 0);
        if (g_trace.watch_every == 0) {
            fprintf(stderr, "[TRACE ERROR] MIPS_TRACE_WATCH_EVERY precisa ser maior que zero.\n");
            g_trace.watch_every = 100000;
        }
        if (g_trace.watch_trace_after != 0 ||
            g_trace.watch_trace_steps != 0) {
            if (g_trace.watch_trace_after == 0 ||
                g_trace.watch_trace_steps == 0) {
                fprintf(stderr,
                        "[TRACE ERROR] MIPS_TRACE_WATCH_TRACE_AFTER e "
                        "MIPS_TRACE_WATCH_TRACE_STEPS devem ser usados juntos.\n");
            } else {
                g_trace.watch_trace_configured = true;
            }
        }
        g_trace.watch_configured = true;
    } else if (getenv("MIPS_TRACE_WATCH_TRACE_AFTER") != NULL ||
               getenv("MIPS_TRACE_WATCH_TRACE_STEPS") != NULL) {
        fprintf(stderr,
                "[TRACE ERROR] A captura disparada requer MIPS_TRACE_WATCH_PC.\n");
    }
    const char *start = getenv("MIPS_TRACE_START");
    const char *end = getenv("MIPS_TRACE_END");
    if (start != NULL && *start != '\0') {
        g_trace.start_pc = (uint32_t)trace_parse_u64("MIPS_TRACE_START", 0);
        g_trace.has_start = true;
    }
    if (end != NULL && *end != '\0') {
        g_trace.end_pc = (uint32_t)trace_parse_u64("MIPS_TRACE_END", UINT32_MAX);
        g_trace.has_end = true;
    }
#ifdef FORCE_INTERPRETER
    const char *mode = "PURE_INTERPRETER";
#else
    const char *mode = "HYBRID_AOT_INTERPRETER";
#endif
    fprintf(g_trace.file,
            "TRACE HEADER format=1 mode=%s build=%s_%s start=%08X end=%08X max=%" PRIu64
            " fp=%u memory=%u watch_addr=%08X watch_size=%u watch_nan=%u "
            "nan_limit=%" PRIu64 " nan_addr=%08X nan_size=%u "
            "nan_value=%08X\n",
            mode, __DATE__, __TIME__,
            g_trace.has_start ? g_trace.start_pc : 0,
            g_trace.has_end ? g_trace.end_pc : UINT32_MAX,
            g_trace.max_steps, g_trace.include_fp ? 1u : 0u,
            g_trace.include_memory ? 1u : 0u,
            g_trace.memory_watch_addr, g_trace.memory_watch_size,
            g_trace.watch_nan_writes ? 1u : 0u,
            g_trace.watch_nan_write_limit, g_trace.watch_nan_addr,
            g_trace.watch_nan_size, g_trace.watch_nan_value_configured
                ? g_trace.watch_nan_value : 0);
    atexit(trace_close);
    if (cpu != NULL) {
        cpu->trace_enabled = true;
        cpu->trace_memory_enabled =
            g_trace.include_memory || g_trace.memory_watch_configured ||
            g_trace.watch_nan_writes;
    }
}

void mips_trace_init(MIPS_CPU *cpu) {
    trace_initialize(cpu);
}

static bool trace_pc_selected(uint32_t pc) {
    return (!g_trace.has_start || pc >= g_trace.start_pc) &&
           (!g_trace.has_end || pc <= g_trace.end_pc);
}

static uint32_t trace_normalize_pc(uint32_t pc) {
    return pc != 0 && pc < 0x04000000 ? pc + PSP_USER_LOAD_BASE : pc;
}

void mips_trace_instruction(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc, uint32_t inst) {
    if (cpu == NULL) return;
    g_trace.step_sequence++;
    pc = trace_normalize_pc(pc);
    next_pc = trace_normalize_pc(next_pc);
    cpu->trace_current_pc = pc;
    cpu->trace_current_inst = inst;
    if (!g_trace.enabled || !trace_pc_selected(pc)) return;
    if (g_trace.watch_configured && pc == g_trace.watch_pc) {
        g_trace.watch_visits++;
        if (g_trace.watch_visits % g_trace.watch_every == 0) {
            uint32_t record[3] = { 0, 0, 0 };
            const void *record_ptr = mips_get_ptr_range(cpu, cpu->gpr[4], sizeof(record));
            if (record_ptr != NULL) {
                memcpy(record, record_ptr, sizeof(record));
            }
            g_trace.watch_samples++;
            fprintf(g_trace.file,
                    "TRACE WATCH sample=%" PRIu64 " visits=%" PRIu64
                    " seq=%" PRIu64 " pc=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                    "t0=%08X t1=%08X t2=%08X v0=%08X ra=%08X "
                    "mem_a0=%08X,%08X,%08X valid=%u\n",
                    g_trace.watch_samples, g_trace.watch_visits,
                    g_trace.step_sequence, pc, cpu->gpr[4], cpu->gpr[5],
                    cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9],
                    cpu->gpr[10], cpu->gpr[2], cpu->gpr[31],
                    record[0], record[1], record[2], record_ptr != NULL ? 1u : 0u);
            fflush(g_trace.file);
            if (g_trace.watch_trace_configured &&
                !g_trace.watch_trace_started &&
                g_trace.watch_visits >= g_trace.watch_trace_after) {
                g_trace.watch_trace_started = true;
                g_trace.watch_trace_remaining = g_trace.watch_trace_steps;
                fprintf(g_trace.file,
                        "TRACE WATCH TRACE START visits=%" PRIu64
                        " steps=%" PRIu64 "\n",
                        g_trace.watch_visits, g_trace.watch_trace_steps);
                fflush(g_trace.file);
            }
            if (g_trace.watch_samples_limit != 0 &&
                g_trace.watch_samples >= g_trace.watch_samples_limit) {
                fprintf(g_trace.file, "TRACE WATCH STOP samples=%" PRIu64 "\n",
                        g_trace.watch_samples);
                fflush(g_trace.file);
                cpu->exit_requested = true;
                g_trace.watch_configured = false;
            }
        }
    }
    bool watch_trace_active = g_trace.watch_trace_started &&
                              g_trace.watch_trace_remaining != 0;
    if (g_trace.watch_trace_configured && !watch_trace_active) return;
    if (g_trace.watch_only && !watch_trace_active) {
        if (g_trace.max_steps != 0 &&
            g_trace.step_sequence >= g_trace.max_steps) {
            fprintf(g_trace.file, "TRACE LIMIT steps=%" PRIu64 " last_pc=%08X\n",
                    g_trace.step_sequence, pc);
            fflush(g_trace.file);
            if (g_trace.stop_at_max_steps) {
                cpu->exit_requested = true;
            }
            g_trace.enabled = false;
            cpu->trace_enabled = false;
            cpu->trace_memory_enabled = false;
        }
        return;
    }
    if (g_trace.max_steps != 0 && g_trace.emitted_steps >= g_trace.max_steps) {
        fprintf(g_trace.file, "TRACE LIMIT steps=%" PRIu64 " last_pc=%08X\n",
                g_trace.emitted_steps, pc);
        fflush(g_trace.file);
        if (g_trace.stop_at_max_steps) {
            cpu->exit_requested = true;
        }
        g_trace.enabled = false;
        cpu->trace_enabled = false;
        cpu->trace_memory_enabled = false;
        return;
    }

    fprintf(g_trace.file,
            "TRACE STEP seq=%" PRIu64 " pc=%08X next=%08X inst=%08X hi=%08X lo=%08X "
            "fcr31=%08X gpr=",
            g_trace.step_sequence, pc, next_pc, inst, cpu->hi, cpu->lo, cpu->fcr31);
    for (unsigned i = 0; i < 32; i++) {
        fprintf(g_trace.file, "%s%08X", i == 0 ? "" : ",", cpu->gpr[i]);
    }
    if (g_trace.include_fp) {
        fprintf(g_trace.file, " fpr=");
        for (unsigned i = 0; i < 32; i++) {
            uint32_t bits;
            memcpy(&bits, &cpu->fpr[i], sizeof(bits));
            fprintf(g_trace.file, "%s%08X", i == 0 ? "" : ",", bits);
        }
        fprintf(g_trace.file, " vfpu=");
        for (unsigned i = 0; i < 128; i++) {
            uint32_t bits;
            memcpy(&bits, &cpu->vfpu[i], sizeof(bits));
            fprintf(g_trace.file, "%s%08X", i == 0 ? "" : ",", bits);
        }
        fprintf(g_trace.file, " vcr=%08X,%08X,%08X,%08X",
                cpu->vcr[0], cpu->vcr[1], cpu->vcr[2], cpu->vcr[3]);
    }
    fputc('\n', g_trace.file);
    g_trace.emitted_steps++;
    if (watch_trace_active) {
        g_trace.watch_trace_remaining--;
        if (g_trace.watch_trace_remaining == 0) {
            fprintf(g_trace.file,
                    "TRACE WATCH TRACE END visits=%" PRIu64
                    " captured=%" PRIu64 "\n",
                    g_trace.watch_visits, g_trace.watch_trace_steps);
            fflush(g_trace.file);
        }
    }
}

void mips_trace_instruction_at(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc) {
    if (cpu == NULL || !cpu->trace_enabled) return;
    pc = trace_normalize_pc(pc);
    cpu->trace_current_pc = pc;
    cpu->trace_current_inst = 0;
    uint32_t inst = mips_read32(cpu, pc);
    mips_trace_instruction(cpu, pc, next_pc, inst);
}

void mips_trace_hle(MIPS_CPU *cpu, uint32_t caller_pc, uint32_t syscall_id,
                    const uint32_t args[4], uint32_t result) {
    if (cpu == NULL) return;
    trace_initialize(cpu);
    caller_pc = trace_normalize_pc(caller_pc);
    if (!g_trace.enabled ||
        (!g_trace.include_all_hle && !trace_pc_selected(caller_pc))) return;
    fprintf(g_trace.file,
            "TRACE HLE at=%" PRIu64 " pc=%08X id=%05X args=%08X,%08X,%08X,%08X "
            "result=%08X ra=%08X sp=%08X\n",
            g_trace.step_sequence, caller_pc, syscall_id,
            args[0], args[1], args[2], args[3], result,
            cpu->gpr[31], cpu->gpr[29]);
}

void mips_trace_memory_write(MIPS_CPU *cpu, uint32_t address, uint32_t width,
                             uint32_t old_value, uint32_t value) {
    if (cpu == NULL || !g_trace.enabled) return;
    uint64_t access_end = (uint64_t)address + width;
    uint64_t nan_watch_end = (uint64_t)g_trace.watch_nan_addr +
                             g_trace.watch_nan_size;
    bool nan_address_watched =
        !g_trace.watch_nan_address_configured ||
        ((uint64_t)address < nan_watch_end &&
         access_end > g_trace.watch_nan_addr);
    if (g_trace.watch_nan_writes && nan_address_watched &&
        width == sizeof(uint32_t) &&
        (value & 0x7F800000u) == 0x7F800000u &&
        (value & 0x007FFFFFu) != 0 &&
        (!g_trace.watch_nan_value_configured ||
         value == g_trace.watch_nan_value)) {
        g_trace.watch_nan_writes_seen++;
        fprintf(g_trace.file,
                "TRACE WATCH NAN at=%" PRIu64 " pc=%08X inst=%08X "
                "addr=%08X value=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                "t0=%08X t1=%08X ra=%08X sp=%08X\n",
                g_trace.step_sequence, cpu->trace_current_pc,
                cpu->trace_current_inst, address, value,
                cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
                cpu->gpr[8], cpu->gpr[9], cpu->gpr[31], cpu->gpr[29]);
        if (g_trace.watch_nan_write_limit != 0 &&
            g_trace.watch_nan_writes_seen >= g_trace.watch_nan_write_limit) {
            g_trace.watch_nan_writes = false;
        }
    }
    uint64_t watch_end = (uint64_t)g_trace.memory_watch_addr +
                         g_trace.memory_watch_size;
    bool watched = g_trace.memory_watch_configured &&
                   (uint64_t)address < watch_end &&
                   access_end > g_trace.memory_watch_addr;
    if (watched) {
        fprintf(g_trace.file,
                "TRACE WATCH MEM at=%" PRIu64 " pc=%08X inst=%08X op=W%u "
                "addr=%08X old=%08X value=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                "t0=%08X t1=%08X ra=%08X sp=%08X\n",
                g_trace.step_sequence, cpu->trace_current_pc,
                cpu->trace_current_inst, width * 8, address, old_value, value,
                cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
                cpu->gpr[8], cpu->gpr[9], cpu->gpr[31], cpu->gpr[29]);
    }
    if (g_trace.include_memory && trace_pc_selected(cpu->trace_current_pc)) {
        fprintf(g_trace.file,
                "TRACE MEM at=%" PRIu64 " pc=%08X op=W%u addr=%08X "
                "old=%08X value=%08X\n",
                g_trace.step_sequence, cpu->trace_current_pc, width * 8,
                address, old_value, value);
    }
}

void mips_trace_memory_read(MIPS_CPU *cpu, uint32_t address, uint32_t width,
                            uint32_t value) {
    if (cpu == NULL || !g_trace.enabled) return;
    uint64_t access_end = (uint64_t)address + width;
    uint64_t watch_end = (uint64_t)g_trace.memory_watch_addr +
                         g_trace.memory_watch_size;
    bool watched = g_trace.memory_watch_configured &&
                   (uint64_t)address < watch_end &&
                   access_end > g_trace.memory_watch_addr;
    if (watched) {
        fprintf(g_trace.file,
                "TRACE WATCH MEM at=%" PRIu64 " pc=%08X inst=%08X op=R%u "
                "addr=%08X value=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                "t0=%08X t1=%08X ra=%08X sp=%08X\n",
                g_trace.step_sequence, cpu->trace_current_pc,
                cpu->trace_current_inst, width * 8, address, value,
                cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
                cpu->gpr[8], cpu->gpr[9], cpu->gpr[31], cpu->gpr[29]);
    }
    if (g_trace.include_memory && trace_pc_selected(cpu->trace_current_pc)) {
        fprintf(g_trace.file,
                "TRACE MEM at=%" PRIu64 " pc=%08X op=R%u addr=%08X value=%08X\n",
                g_trace.step_sequence, cpu->trace_current_pc, width * 8,
                address, value);
    }
}

void mips_trace_memory_fault(MIPS_CPU *cpu, uint32_t address, uint32_t access) {
    if (cpu == NULL || !g_trace.enabled ||
        !trace_pc_selected(cpu->trace_current_pc)) return;
    fprintf(g_trace.file,
            "TRACE FAULT at=%" PRIu64 " pc=%08X addr=%08X access=%03X count=%u\n",
            g_trace.step_sequence, cpu->trace_current_pc, address, access,
            cpu->memory_fault_count);
}

int mips_cpu_init(MIPS_CPU *cpu) {
    if (cpu == NULL) {
        return -1;
    }

    // Initialize all fields to 0
    memset(cpu, 0, sizeof(MIPS_CPU));

    // Allocate RAM (64MB)
    // Using posix_memalign or aligned_alloc ensures ARM64 hardware vectors (NEON)
    // can load/store values with optimal performance.
    int ret;
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->ram = (uint8_t*)_aligned_malloc(PSP_RAM_SIZE, 4096);
    ret = (cpu->ram == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->ram, 4096, PSP_RAM_SIZE);
#endif
    if (ret != 0) {
        return -2;
    }
    memset(cpu->ram, 0, PSP_RAM_SIZE);

    // Allocate VRAM (2MB)
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->vram = (uint8_t*)_aligned_malloc(PSP_VRAM_SIZE, 4096);
    ret = (cpu->vram == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->vram, 4096, PSP_VRAM_SIZE);
#endif
    if (ret != 0) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
#else
        free(cpu->ram);
#endif
        cpu->ram = NULL;
        return -3;
    }
    memset(cpu->vram, 0, PSP_VRAM_SIZE);

    // Allocate Scratchpad (16KB)
#if defined(_MSC_VER) || defined(__MINGW32__)
    cpu->scratchpad = (uint8_t*)_aligned_malloc(PSP_SCRATCH_SIZE, 4096);
    ret = (cpu->scratchpad == NULL) ? -1 : 0;
#else
    ret = posix_memalign((void**)&cpu->scratchpad, 4096, PSP_SCRATCH_SIZE);
#endif
    if (ret != 0) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
        _aligned_free(cpu->vram);
#else
        free(cpu->ram);
        free(cpu->vram);
#endif
        cpu->ram = NULL;
        cpu->vram = NULL;
        return -4;
    }
    memset(cpu->scratchpad, 0, PSP_SCRATCH_SIZE);

    // Initial reset of register values
    mips_cpu_reset(cpu);
    mips_trace_init(cpu);

    return 0;
}

void mips_cpu_free(MIPS_CPU *cpu) {
    if (cpu == NULL) return;

    if (cpu->ram) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->ram);
#else
        free(cpu->ram);
#endif
        cpu->ram = NULL;
    }
    if (cpu->vram) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->vram);
#else
        free(cpu->vram);
#endif
        cpu->vram = NULL;
    }
    if (cpu->scratchpad) {
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(cpu->scratchpad);
#else
        free(cpu->scratchpad);
#endif
        cpu->scratchpad = NULL;
    }
}

void mips_cpu_reset(MIPS_CPU *cpu) {
    if (cpu == NULL) return;

    // Reset GPR, FPU and VFPU state (except memory pointers, cycles and exit flag).
    memset(cpu->gpr, 0, sizeof(cpu->gpr));
    memset(cpu->fpr, 0, sizeof(cpu->fpr));
    memset(cpu->vfpu, 0, sizeof(cpu->vfpu));
    memset(cpu->vcr, 0, sizeof(cpu->vcr));
    cpu->vcr[0] = 0xE4;
    cpu->vcr[1] = 0xE4;
    memset(cpu->cop0, 0, sizeof(cpu->cop0));
    
    cpu->pc = 0x08804000; // Standard PSP EBOOT entry point virtual address
    cpu->next_pc = 0x08804004;
    cpu->hi = 0;
    cpu->lo = 0;
    cpu->fcr31 = 0;
    cpu->ic_state = 0;
    cpu->bad_vaddr = 0;
    cpu->last_fault_pc = 0;
    cpu->last_fault_inst = 0;
    cpu->last_fault_access = 0;
    cpu->memory_fault_count = 0;
    cpu->cycles = 0;
    cpu->exit_requested = false;
    cpu->scheduler_saved_v0 = 0;
    cpu->scheduler_saved_v0_valid = false;

    // Ensure R0 is always 0
    cpu->gpr[0] = 0;
    cpu->postmortem_next = 0;
    cpu->postmortem_total = 0;
    memset(cpu->postmortem, 0, sizeof(cpu->postmortem));
}

void mips_postmortem_record(MIPS_CPU *cpu, uint32_t pc, uint32_t next_pc) {
    if (cpu == NULL) return;

    MIPS_PostMortemEntry *entry =
        &cpu->postmortem[cpu->postmortem_next % MIPS_POSTMORTEM_SIZE];
    entry->pc = pc;
    entry->next_pc = next_pc;
    entry->inst = 0;
    uint32_t *inst_ptr = (uint32_t *)mips_get_ptr_range(cpu, pc, sizeof(uint32_t));
    if (inst_ptr != NULL) entry->inst = *inst_ptr;
    entry->ra = cpu->gpr[31];
    entry->sp = cpu->gpr[29];
    entry->v0 = cpu->gpr[2];
    entry->a0 = cpu->gpr[4];
    entry->a1 = cpu->gpr[5];
    entry->a2 = cpu->gpr[6];
    entry->a3 = cpu->gpr[7];
    entry->bad_vaddr = cpu->bad_vaddr;
    cpu->postmortem_next++;
    cpu->postmortem_total++;
}

void mips_postmortem_dump(const MIPS_CPU *cpu, const char *reason) {
    if (cpu == NULL) return;

    uint32_t count = cpu->postmortem_total < MIPS_POSTMORTEM_SIZE
        ? (uint32_t)cpu->postmortem_total
        : MIPS_POSTMORTEM_SIZE;
    uint32_t first = cpu->postmortem_total < MIPS_POSTMORTEM_SIZE
        ? 0
        : cpu->postmortem_next % MIPS_POSTMORTEM_SIZE;

    printf("[PostMortem] reason=%s entries=%u total=%llu\n",
           reason != NULL ? reason : "unspecified",
           count, (unsigned long long)cpu->postmortem_total);
    for (uint32_t i = 0; i < count; i++) {
        const MIPS_PostMortemEntry *entry =
            &cpu->postmortem[(first + i) % MIPS_POSTMORTEM_SIZE];
        printf("[PostMortem] #%02u pc=0x%08X next=0x%08X inst=0x%08X "
               "ra=0x%08X sp=0x%08X v0=0x%08X a0=0x%08X a1=0x%08X "
               "a2=0x%08X a3=0x%08X bad=0x%08X\n",
               i, entry->pc, entry->next_pc, entry->inst,
               entry->ra, entry->sp, entry->v0, entry->a0, entry->a1,
               entry->a2, entry->a3, entry->bad_vaddr);
    }
    fflush(stdout);
}
