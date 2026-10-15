#include "mips_dispatcher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sceKernelThread.h"

int mips_dispatcher_init(MIPS_Dispatcher *disp) {
    if (disp == NULL) {
        return -1;
    }

    // Determine number of elements in table (64MB / 4 bytes alignment = 16M elements)
    uint32_t elements = PSP_RAM_SIZE >> 2;

    // Allocate 128MB flat direct-mapped pointer table
    disp->table = (AOTBlockFunction*)calloc(elements, sizeof(AOTBlockFunction));
    if (disp->table == NULL) {
        return -2;
    }

    disp->fallback_interpreter = NULL; // Must be bound by the interpreter module later
    return 0;
}

void mips_dispatcher_free(MIPS_Dispatcher *disp) {
    if (disp == NULL) return;

    if (disp->table) {
        free(disp->table);
        disp->table = NULL;
    }
}

extern uint32_t g_binary_segment_start;
extern uint32_t g_binary_segment_end;

static uint32_t normalize_aot_control_address(uint32_t address) {
    /*
     * The generated AOT contains link-time PSP addresses below 0x04000000.
     * The loader maps the main executable at PSP_USER_LOAD_BASE. Normalize
     * only control-flow addresses; data/register
     * values must remain untouched.
     */
    if (address != 0 && address < 0x04000000) {
        return address + PSP_USER_LOAD_BASE;
    }
    return address;
}

static bool is_obvious_data_control_address(uint32_t address) {
    /*
     * These values are data patterns, not valid PSP code addresses.  They
     * commonly appear when an optional callback/function-table entry was
     * never initialized (0x20202020 is the space-filled homebrew buffer).
     */
    return address == 0x20202020 ||
           address == 0x27BDFFD0 ||
           address == 0xFFFFFFFF;
}

static uint64_t diagnostic_interval(void) {
    static bool initialized = false;
    static uint64_t interval = 1000000;
    if (initialized) return interval;
    initialized = true;

    const char *value = getenv("MIPS_THREAD_DIAG_EVERY");
    if (value == NULL || value[0] == '\0') return interval;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (value[0] == '-' || end == value || *end != '\0') {
        fprintf(stderr, "[Dispatcher] Invalid MIPS_THREAD_DIAG_EVERY='%s'; using %llu\n",
                value, (unsigned long long)interval);
    } else {
        interval = (uint64_t)parsed;
    }
    return interval;
}

static uint64_t preemption_quantum(void) {
    static bool initialized = false;
    static uint64_t quantum = 256;
    if (initialized) return quantum;
    initialized = true;

    const char *value = getenv("MIPS_THREAD_QUANTUM");
    if (value == NULL || value[0] == '\0') return quantum;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (value[0] == '-' || end == value || *end != '\0') {
        fprintf(stderr, "[Dispatcher] Invalid MIPS_THREAD_QUANTUM='%s'; using %llu\n",
                value, (unsigned long long)quantum);
    } else {
        quantum = (uint64_t)parsed;
    }
    return quantum;
}

static const char *mips_instruction_name(uint32_t instruction) {
    static const char *const special_names[64] = {
        [0x00] = "sll", [0x02] = "srl", [0x03] = "sra",
        [0x04] = "sllv", [0x06] = "srlv", [0x07] = "srav",
        [0x08] = "jr", [0x09] = "jalr", [0x0C] = "syscall",
        [0x10] = "mfhi", [0x11] = "mthi", [0x12] = "mflo",
        [0x13] = "mtlo", [0x18] = "mult", [0x19] = "multu",
        [0x1A] = "div", [0x1B] = "divu", [0x20] = "add",
        [0x21] = "addu", [0x22] = "sub", [0x23] = "subu",
        [0x24] = "and", [0x25] = "or", [0x26] = "xor",
        [0x27] = "nor", [0x2A] = "slt", [0x2B] = "sltu"
    };
    static const char *const opcode_names[64] = {
        [0x02] = "j", [0x03] = "jal", [0x04] = "beq",
        [0x05] = "bne", [0x06] = "blez", [0x07] = "bgtz",
        [0x14] = "beql", [0x15] = "bnel",
        [0x08] = "addi", [0x09] = "addiu", [0x0A] = "slti",
        [0x0B] = "sltiu", [0x0C] = "andi", [0x0D] = "ori",
        [0x0E] = "xori", [0x0F] = "lui", [0x20] = "lb",
        [0x21] = "lh", [0x23] = "lw", [0x24] = "lbu",
        [0x25] = "lhu", [0x28] = "sb", [0x29] = "sh",
        [0x2B] = "sw"
    };
    uint32_t opcode = instruction >> 26;
    if (opcode == 0) {
        const char *name = special_names[instruction & 0x3F];
        return name != NULL ? name : "special";
    }
    const char *name = opcode_names[opcode];
    return name != NULL ? name : "unknown";
}

static void log_thread_diagnostic(MIPS_CPU *cpu, uint64_t dispatch_count) {
    uint64_t interval = diagnostic_interval();
    if (interval == 0 || dispatch_count % interval != 0) return;

    const char *name = "<no-current-thread>";
    ThreadState state = THREAD_STATE_FREE;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].id == current_thread_id &&
            threads[i].state != THREAD_STATE_FREE) {
            name = threads[i].name;
            state = threads[i].state;
            break;
        }
    }
    uint32_t instruction = 0;
    const uint32_t *instruction_ptr =
        (const uint32_t *)mips_get_ptr_range(cpu, cpu->pc, sizeof(uint32_t));
    if (instruction_ptr != NULL) instruction = *instruction_ptr;

    printf("[Dispatcher Thread Diagnostic] dispatch=%llu thread='%s' id=%d state=%d "
           "pc=0x%08X next_pc=0x%08X inst=0x%08X (%s) "
           "v0=0x%08X a0=0x%08X a1=0x%08X t0=0x%08X t1=0x%08X "
           "s0=0x%08X ra=0x%08X sp=0x%08X\n",
           (unsigned long long)dispatch_count, name, current_thread_id, state,
           cpu->pc, cpu->next_pc, instruction,
           mips_instruction_name(instruction), cpu->gpr[2], cpu->gpr[4],
           cpu->gpr[5], cpu->gpr[8], cpu->gpr[9], cpu->gpr[16],
           cpu->gpr[31], cpu->gpr[29]);
    fflush(stdout);
}

bool mips_aot_indirect_target_is_data(uint32_t target) {
    return is_obvious_data_control_address(target);
}

void mips_aot_trace_control_window(MIPS_CPU *cpu, uint32_t relative_pc) {
    static unsigned long trace_count = 0;
    if (cpu == NULL) return;
    trace_count++;
    if (trace_count > 128 && (trace_count % 10000) != 0) return;

    printf("[AOT TRACE] #%lu relpc=0x%08X abs_pc=0x%08X "
           "a0=%08X a1=%08X a2=%08X a3=%08X "
           "v0=%08X v1=%08X t0=%08X t1=%08X "
           "s0=%08X s1=%08X ra=%08X sp=%08X\n",
           trace_count, relative_pc, cpu->pc,
           cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
           cpu->gpr[2], cpu->gpr[3], cpu->gpr[8], cpu->gpr[9],
           cpu->gpr[16], cpu->gpr[17], cpu->gpr[31], cpu->gpr[29]);

    if (relative_pc == 0x0001E444 || relative_pc == 0x0001E44C) {
        uint32_t base = cpu->gpr[16];
        printf("[AOT TRACE TABLE] pc=0x%08X s0=0x%08X "
               "table[0]=%08X table[4]=%08X table[8]=%08X table[12]=%08X "
               "table[16]=%08X table[20]=%08X\n",
               relative_pc, base,
               mips_read32(cpu, base + 0),
               mips_read32(cpu, base + 4),
               mips_read32(cpu, base + 8),
               mips_read32(cpu, base + 12),
               mips_read32(cpu, base + 16),
               mips_read32(cpu, base + 20));
    }
}

void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu) {
    if (disp == NULL || cpu == NULL) return;

    uint32_t last_pc = 0;
    uint32_t last_next_pc = 0;
    uint64_t repeated_state_count = 0;
    uint64_t dispatch_count = 0;
    uint64_t blocks_since_preemption = 0;
    int last_diagnostic_thread_id = current_thread_id;

    while (__builtin_expect(!cpu->exit_requested, 1)) {
        dispatch_count++;
        /*
         * Normalize generated AOT control flow before validating the PC.
         * Previously this happened only after the bounds check, so the first
         * relative jump (for example 0x00301DF4) was rejected immediately.
         * Zero is intentionally left untouched: it is a null/invalid target,
         * not a relative address.
         */
        cpu->pc = normalize_aot_control_address(cpu->pc);
        cpu->next_pc = normalize_aot_control_address(cpu->next_pc);
        mips_postmortem_record(cpu, cpu->pc, cpu->next_pc);
        log_thread_diagnostic(cpu, dispatch_count);
        if (current_thread_id != last_diagnostic_thread_id) {
            blocks_since_preemption = 0;
            last_diagnostic_thread_id = current_thread_id;
        }

        if (is_obvious_data_control_address(cpu->pc)) {
            uint32_t bad_pc = cpu->pc;
            printf("[HLE CONTROL] Salto indireto para dado 0x%08X (ra=0x%08X, sp=0x%08X)\n",
                   bad_pc, cpu->gpr[31], cpu->gpr[29]);
            /*
             * Do not fabricate a return address from $ra or the stack:
             * jalr's link points inside the callback helper, and unwinding
             * there can execute the callback table as code again.  Let
             * ThreadMan handle this as a terminated thread so the scheduler
             * can select another runnable thread.
             */
            printf("[HLE CONTROL] Callback invalido; encerrando a thread atual pelo trap do ThreadMan\n");
            cpu->pc = 0x08000100;
            cpu->next_pc = cpu->pc + 4;
            continue;
        }

        if (cpu->pc == last_pc && cpu->next_pc == last_next_pc) {
            repeated_state_count++;
        } else {
            repeated_state_count = 0;
            last_pc = cpu->pc;
            last_next_pc = cpu->next_pc;
        }
        if (dispatch_count <= 32) {
            printf("[Dispatcher] #%llu PC=0x%08X next=0x%08X ra=0x%08X\n",
                   (unsigned long long)dispatch_count, cpu->pc,
                   cpu->next_pc, cpu->gpr[31]);
        } else if (repeated_state_count == 1000000) {
            printf("[Dispatcher] Loop suspeito: PC=0x%08X next=0x%08X ra=0x%08X repetido por 1000000 iteracoes\n",
                   cpu->pc, cpu->next_pc, cpu->gpr[31]);
            // Loops de frame podem ser legítimos; o diagnóstico não altera o fluxo.
            repeated_state_count = 0;
        }

        /*
         * Keep the initial loaded code window on the interpreter path until
         * the generated AOT dispatch table is known to cover it. This is
         * executable EBOOT content, not a null-callback sentinel.
         */
        if (cpu->pc == 0x08000100 ||
            cpu->pc == CB_RETURN_HACK_ADDR ||
            cpu->pc == 0x08000300 ||
            (cpu->pc >= 0x08800000 && cpu->pc < 0x08800034)) {
            if (disp->fallback_interpreter != NULL) {
                disp->fallback_interpreter(cpu);
                continue;
            }
            cpu->exit_requested = true;
            break;
        }

        // Sentinelas de controle de fluxo que NAO sao codigo real (ver sceKernelThread.c/.h
        // e main_v35.c::interpreter_step, que trata cada um deles ANTES de cair no
        // interpretador de verdade):
        //   0x08000100 -> "thread terminou" (return address trap)
        //   0x08000200 -> CB_RETURN_HACK_ADDR, trampolim de retorno de callback
        //   0x08000300 -> endereco de idle do scheduler (nenhuma thread READY)
        // Os tres precisam ficar fora da checagem de limites, senao qualquer callback
        // que retorna ou qualquer momento de idle e' confundido com um salto para
        // memoria invalida.
        if (cpu->pc != 0x08000100 &&
            cpu->pc != CB_RETURN_HACK_ADDR &&
            cpu->pc != 0x08000300 &&
            (g_binary_segment_end <= g_binary_segment_start ||
             cpu->pc < g_binary_segment_start || cpu->pc >= g_binary_segment_end)) {
            printf("[HLE EXCEPTION INTERCEPTED] CPU saltou para fora dos limites legitimos!\n");
            printf("  -> Limites legitimos do binario: 0x%08X - 0x%08X\n", g_binary_segment_start, g_binary_segment_end);
            printf("  -> PC invalido: 0x%08X\n", cpu->pc);
            printf("  -> Estado: next=0x%08X ra=0x%08X sp=0x%08X v0=0x%08X instr=0x%08X\n",
                   cpu->next_pc, cpu->gpr[31], cpu->gpr[29],
                   cpu->gpr[2], mips_read32(cpu, cpu->pc));
            cpu->bad_vaddr = cpu->pc;
            cpu->exit_requested = true;
            break;
        }

        /*
         * Imported PSP stubs are patched in emulated RAM after the static
         * AOT was generated.  The generated C still contains the original
         * jr $ra/nop pair, so dispatching that block would bypass HLE
         * completely.  Prefer the interpreter whenever the relocated RAM
         * contains a syscall instruction.
         */
        AOTBlockFunction func = NULL;
#ifdef FORCE_INTERPRETER
        /*
         * Diagnostic build: keep the exact same loader/HLE/thread path but
         * execute every instruction through the architectural interpreter.
         */
        func = disp->fallback_interpreter;
#else
        uint32_t ram_inst = mips_read32(cpu, cpu->pc);
        if ((ram_inst & 0x3F) == 0x0C && (ram_inst >> 26) == 0) {
            func = disp->fallback_interpreter;
        } else if (cpu->pc >= 0x08805894 && cpu->pc <= 0x08805AF8) {
            /*
             * The current GOW AOT build repeatedly re-enters this compact
             * byte-swap/copy helper with relative control-flow values
             * (0x000059xx) instead of reaching its return path.  Execute
             * only this helper through the architectural interpreter until
             * its delay-slot behavior is proven equivalent; the rest of the
             * game remains AOT.
             */
            func = disp->fallback_interpreter;
        } else {
            func = mips_dispatcher_lookup(disp, cpu->pc);
        }
#endif
        
        if (__builtin_expect(func != NULL, 1)) {
            // Executing an AOT block or the interpreter for a sparse block.
            uint32_t block_pc = cpu->pc;
            func(cpu);
            cpu->pc = normalize_aot_control_address(cpu->pc);
            cpu->next_pc = normalize_aot_control_address(cpu->next_pc);
            blocks_since_preemption++;
            uint64_t quantum = preemption_quantum();
            if (!cpu->exit_requested && quantum != 0 &&
                blocks_since_preemption >= quantum &&
                hle_scheduler_dispatch_enabled() &&
                hle_scheduler_has_ready_thread()) {
                run_scheduler(cpu, THREAD_STATE_READY);
                blocks_since_preemption = 0;
                last_diagnostic_thread_id = current_thread_id;
            }
            if (cpu->pc != 0 &&
                cpu->pc != 0x08000100 &&
                cpu->pc != CB_RETURN_HACK_ADDR &&
                cpu->pc != 0x08000300 &&
                (cpu->pc < g_binary_segment_start ||
                 cpu->pc >= g_binary_segment_end)) {
                static unsigned int suspect_log_count = 0;
                suspect_log_count++;
                if (suspect_log_count <= 128 ||
                    (suspect_log_count % 10000) == 0) {
                    printf("[Dispatcher] Bloco 0x%08X produziu PC fora dos limites 0x%08X "
                           "(next=0x%08X ra=0x%08X, evento=%u)\n",
                           block_pc, cpu->pc, cpu->next_pc, cpu->gpr[31],
                           suspect_log_count);
                }
            }
        } else {
            // No interpreter was bound, so stopping is safer than spinning.
            // A missing AOT block by itself is not an error: lookup() returns
            // fallback_interpreter for every mapped address.
            cpu->bad_vaddr = cpu->pc;
            cpu->exit_requested = true;
            break;
        }
    }

    mips_postmortem_dump(cpu, cpu->exit_requested
        ? "dispatcher_exit_requested"
        : "dispatcher_stopped");
}
