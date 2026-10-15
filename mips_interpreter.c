#include "mips_interpreter.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// Extern routing function to our HLE Kernel
extern void hle_syscall(MIPS_CPU *cpu);

static void trace_allegrex_special_candidate(const MIPS_CPU *cpu,
                                              uint32_t pc, uint32_t inst,
                                              uint32_t opcode, uint32_t rs,
                                              uint32_t rt, uint32_t rd,
                                              uint32_t shamt,
                                              uint32_t funct) {
    static int enabled = -1;
    static uint32_t seen_pc[128];
    static size_t seen_count;
    static bool limit_reported;
    if (enabled < 0) {
        const char *setting = getenv("MIPS_TRACE_ALLEGREX_SPECIAL");
        enabled = setting != NULL && strcmp(setting, "1") == 0;
    }
    if (!enabled || opcode != 0) return;

    const char *name = NULL;
    switch (funct) {
        case 0x02: if (rs == 1) name = "ROTR"; break;
        case 0x06: if (shamt == 1) name = "ROTRV"; break;
        case 0x16: name = "CLZ"; break;
        case 0x17: name = "CLO"; break;
        case 0x1C: name = "MADD"; break;
        case 0x1D: name = "MADDU"; break;
        case 0x2C: name = "MAX"; break;
        case 0x2D: name = "MIN"; break;
        case 0x2E: name = "MSUB"; break;
        case 0x2F: name = "MSUBU"; break;
        default: break;
    }
    if (name == NULL) return;
    for (size_t i = 0; i < seen_count; i++) {
        if (seen_pc[i] == pc) return;
    }
    if (seen_count >= sizeof(seen_pc) / sizeof(seen_pc[0])) {
        if (!limit_reported) {
            fprintf(stderr,
                    "[ALLEGREX SPECIAL] unique-PC trace limit reached (%zu)\n",
                    seen_count);
            limit_reported = true;
        }
        return;
    }
    seen_pc[seen_count++] = pc;
    fprintf(stderr,
            "[ALLEGREX SPECIAL] pc=%08X inst=%08X op=%s rs=%u(%08X) "
            "rt=%u(%08X) rd=%u sa=%u hi=%08X lo=%08X\n",
            pc, inst, name, rs, cpu->gpr[rs], rt, cpu->gpr[rt], rd, shamt,
            cpu->hi, cpu->lo);
}

static void vfpu_get_vector_regs(uint8_t regs[4], uint32_t size, uint32_t vector_reg) {
    uint32_t matrix = (vector_reg >> 2) & 7;
    uint32_t column = vector_reg & 3;
    uint32_t row;
    bool transpose = (vector_reg & 0x20) != 0;

    switch (size) {
        case 1:
            transpose = false;
            row = (vector_reg >> 5) & 3;
            break;
        case 2:
            row = (vector_reg >> 5) & 2;
            break;
        case 3:
            row = (vector_reg >> 6) & 1;
            break;
        case 4:
            row = (vector_reg >> 5) & 2;
            break;
        default:
            return;
    }

    for (uint32_t i = 0; i < size; i++) {
        uint32_t index = matrix * 4;
        if (transpose) {
            index += ((row + i) & 3) + column * 32;
        } else {
            index += column + ((row + i) & 3) * 32;
        }
        regs[i] = (uint8_t)index;
    }
}

static void vfpu_get_matrix_regs(uint8_t regs[16], uint32_t size,
                                 uint32_t matrix_reg) {
    uint32_t matrix = (matrix_reg >> 2) & 7;
    uint32_t column = matrix_reg & 3;
    uint32_t row;
    bool transpose = (matrix_reg & 0x20) != 0;

    switch (size) {
        case 1:
            transpose = false;
            row = (matrix_reg >> 5) & 3;
            break;
        case 2:
        case 4:
            row = (matrix_reg >> 5) & 2;
            break;
        case 3:
            row = (matrix_reg >> 6) & 1;
            break;
        default:
            return;
    }

    for (uint32_t column_index = 0; column_index < size; column_index++) {
        for (uint32_t row_index = 0; row_index < size; row_index++) {
            uint32_t index = matrix * 4;
            if (transpose) {
                index += ((row + row_index) & 3) +
                         ((column + column_index) & 3) * 32;
            } else {
                index += ((column + column_index) & 3) +
                         ((row + row_index) & 3) * 32;
            }
            regs[column_index * 4 + row_index] = (uint8_t)index;
        }
    }
}

static uint32_t vfpu_float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void vfpu_apply_prefix_st(float values[4], uint32_t prefix,
                                 uint32_t size) {
    static const float constants[8] = {
        0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f
    };
    float original[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < size; i++) {
        original[i] = values[i];
    }

    for (uint32_t i = 0; i < size; i++) {
        uint32_t selector = (prefix >> (i * 2)) & 3;
        bool absolute = ((prefix >> (8 + i)) & 1) != 0;
        bool constant = ((prefix >> (12 + i)) & 1) != 0;
        bool negate = ((prefix >> (16 + i)) & 1) != 0;
        if (constant) {
            values[i] = constants[selector + (absolute ? 4 : 0)];
        } else if (selector < size) {
            values[i] = original[selector];
            uint32_t bits = vfpu_float_bits(values[i]);
            if (absolute) {
                bits &= 0x7FFFFFFFu;
            }
            memcpy(&values[i], &bits, sizeof(bits));
        } else {
            values[i] = 0.0f;
        }
        if (negate) {
            uint32_t bits = vfpu_float_bits(values[i]) ^ 0x80000000u;
            memcpy(&values[i], &bits, sizeof(bits));
        }
    }
}

static void vfpu_apply_prefix_d(float values[4], uint32_t size,
                                uint32_t prefix) {
    for (uint32_t i = 0; i < size; i++) {
        uint32_t saturation = (prefix >> (i * 2)) & 3;
        float minimum = saturation == 3 ? -1.0f : 0.0f;
        float maximum = 1.0f;
        if (saturation == 1 || saturation == 3) {
            if (values[i] < minimum) values[i] = minimum;
            if (values[i] > maximum) values[i] = maximum;
        }
    }
}

static void vfpu_eat_prefixes(MIPS_CPU *cpu) {
    cpu->vcr[0] = 0xE4;
    cpu->vcr[1] = 0xE4;
    cpu->vcr[2] = 0;
}

static void vfpu_retain_invalid_swizzle(const MIPS_CPU *cpu,
                                       float values[4], uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        uint32_t selector_s = (cpu->vcr[0] >> (i * 2)) & 3;
        uint32_t selector_t = (cpu->vcr[1] >> (i * 2)) & 3;
        bool constant_s = ((cpu->vcr[0] >> (12 + i)) & 1) != 0;
        bool constant_t = ((cpu->vcr[1] >> (12 + i)) & 1) != 0;
        if ((selector_s >= size && !constant_s) ||
            (selector_t >= size && !constant_t)) {
            values[i] = 0.0f;
        }
    }
}

static void vfpu_execute_vfpu3(MIPS_CPU *cpu, uint32_t pc,
                               uint32_t inst) {
    uint32_t subop = (inst >> 23) & 7;
    uint32_t vd = inst & 0x7F;
    uint32_t vs = (inst >> 8) & 0x7F;
    uint32_t vt = (inst >> 16) & 0x7F;
    uint32_t size = 1u + ((inst >> 7) & 1u) +
                    (((inst >> 15) & 1u) << 1);
    uint8_t dst_regs[4], src_regs[4], target_regs[4];
    float source[4] = {0};
    float target[4] = {0};
    float result[4] = {0};

    if (subop == 0) {
        uint32_t condition = inst & 0xF;
        uint32_t lane_cc = 0;
        uint32_t any_cc = 0;
        uint32_t all_cc = 1;
        uint32_t affected = (1u << 4) | (1u << 5);

        vfpu_get_vector_regs(src_regs, size, vs);
        vfpu_get_vector_regs(target_regs, size, vt);
        for (uint32_t i = 0; i < size; i++) {
            source[i] = cpu->vfpu[src_regs[i]];
            target[i] = cpu->vfpu[target_regs[i]];
        }
        vfpu_apply_prefix_st(source, cpu->vcr[0], size);
        vfpu_apply_prefix_st(target, cpu->vcr[1], size);
        for (uint32_t i = 0; i < size; i++) {
            bool value;
            switch (condition) {
                case 0: value = false; break;
                case 1: value = source[i] == target[i]; break;
                case 2: value = source[i] < target[i]; break;
                case 3: value = source[i] <= target[i]; break;
                case 4: value = true; break;
                case 5: value = source[i] != target[i]; break;
                case 6: value = source[i] >= target[i]; break;
                case 7: value = source[i] > target[i]; break;
                case 8: value = source[i] == 0.0f; break;
                case 9: value = isnan(source[i]); break;
                case 10: value = isinf(source[i]); break;
                case 11: value = isnan(source[i]) || isinf(source[i]); break;
                case 12: value = source[i] != 0.0f; break;
                case 13: value = !isnan(source[i]); break;
                case 14: value = !isinf(source[i]); break;
                default: value = !isnan(source[i]) && !isinf(source[i]); break;
            }
            if (value) lane_cc |= 1u << i;
            any_cc |= value ? 1u : 0u;
            all_cc &= value ? 1u : 0u;
            affected |= 1u << i;
        }
        cpu->vcr[3] = (cpu->vcr[3] & ~affected) |
                      ((lane_cc | (any_cc << 4) | (all_cc << 5)) & affected);
        vfpu_eat_prefixes(cpu);
        return;
    }

    if (subop != 2 && subop != 3 && subop != 5 &&
        subop != 6 && subop != 7) {
        fprintf(stderr,
                "[Interpreter Warning] Unsupported VFPU3 subop %u "
                "at PC 0x%08X (Inst: 0x%08X)\n",
                subop, pc, inst);
        return;
    }

    vfpu_get_vector_regs(dst_regs, size, vd);
    vfpu_get_vector_regs(src_regs, size, vs);
    vfpu_get_vector_regs(target_regs, size, vt);
    for (uint32_t i = 0; i < size; i++) {
        source[i] = cpu->vfpu[src_regs[i]];
        target[i] = cpu->vfpu[target_regs[i]];
    }
    vfpu_apply_prefix_st(source, cpu->vcr[0], size);
    vfpu_apply_prefix_st(target, cpu->vcr[1], size);

    for (uint32_t i = 0; i < size; i++) {
        switch (subop) {
            case 2:
            case 3: {
                uint32_t source_bits = vfpu_float_bits(source[i]);
                uint32_t target_bits = vfpu_float_bits(target[i]);
                if (isnan(source[i]) || isinf(source[i]) ||
                    isnan(target[i]) || isinf(target[i])) {
                    int32_t source_order = (int32_t)source_bits;
                    int32_t target_order = (int32_t)target_bits;
                    bool both_negative =
                        (source_bits & 0x80000000u) != 0 &&
                        (target_bits & 0x80000000u) != 0;
                    bool choose_source;
                    if (subop == 2) {
                        choose_source = both_negative
                            ? source_order > target_order
                            : source_order < target_order;
                    } else {
                        choose_source = both_negative
                            ? source_order < target_order
                            : source_order > target_order;
                    }
                    uint32_t selected = choose_source
                        ? source_bits : target_bits;
                    memcpy(&result[i], &selected, sizeof(selected));
                } else if (subop == 2) {
                    result[i] = source[i] < target[i]
                        ? source[i] : target[i];
                } else {
                    result[i] = target[i] < source[i]
                        ? source[i] : target[i];
                }
                break;
            }
            case 5: {
                float difference = source[i] - target[i];
                if (isnan(difference)) {
                    uint32_t source_bits = vfpu_float_bits(source[i]);
                    uint32_t target_bits = vfpu_float_bits(target[i]);
                    int64_t source_magnitude = source_bits & 0x7FFFFFFFu;
                    int64_t target_magnitude = target_bits & 0x7FFFFFFFu;
                    int64_t ordered_source =
                        (source_bits & 0x80000000u) != 0
                        ? -source_magnitude : source_magnitude;
                    int64_t ordered_target =
                        (target_bits & 0x80000000u) != 0
                        ? -target_magnitude : target_magnitude;
                    result[i] = (float)((ordered_source > ordered_target) -
                                        (ordered_source < ordered_target));
                } else {
                    result[i] = (float)((difference > 0.0f) -
                                        (difference < 0.0f));
                }
                break;
            }
            case 6:
                result[i] = isnan(source[i]) || isnan(target[i])
                    ? 0.0f : (source[i] >= target[i] ? 1.0f : 0.0f);
                break;
            case 7:
                result[i] = isnan(source[i]) || isnan(target[i])
                    ? 0.0f : (source[i] < target[i] ? 1.0f : 0.0f);
                break;
        }
    }

    vfpu_retain_invalid_swizzle(cpu, result, size);
    vfpu_apply_prefix_d(result, size, cpu->vcr[2]);
    for (uint32_t i = 0; i < size; i++) {
        if ((cpu->vcr[2] & (1u << (8 + i))) == 0) {
            cpu->vfpu[dst_regs[i]] = result[i];
        }
    }
    vfpu_eat_prefixes(cpu);
}

static uint32_t mips_float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool mips_nan_origin_matches(uint32_t bits) {
    static int initialized;
    static bool value_configured;
    static uint32_t watched_value;
    if (!initialized) {
        const char *setting = getenv("MIPS_TRACE_NAN_ORIGIN_VALUE");
        if (setting != NULL && setting[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(setting, &end, 0);
            if (end != setting && *end == '\0' && parsed <= UINT32_MAX) {
                watched_value = (uint32_t)parsed;
                value_configured = true;
            } else {
                fprintf(stderr,
                        "[NAN ORIGIN ERROR] Invalid MIPS_TRACE_NAN_ORIGIN_VALUE '%s'.\n",
                        setting);
            }
        }
        initialized = 1;
    }
    return !value_configured || bits == watched_value;
}

static bool mips_bits_are_nan(uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u &&
           (bits & 0x007FFFFFu) != 0;
}

static uint64_t mips_interpreter_icount;

static bool mips_nan_load_trace_enabled(void) {
    static int initialized;
    static bool enabled;
    if (!initialized) {
        const char *setting = getenv("MIPS_TRACE_NAN_LOADS");
        enabled = setting != NULL && setting[0] != '\0' &&
                  strcmp(setting, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

static void mips_trace_nan_load(MIPS_CPU *cpu, uint32_t pc, uint32_t inst,
                                uint32_t address, uint32_t value,
                                uint32_t target_reg) {
    enum { MAX_UNIQUE_NAN_LOADS = 256 };
    static struct {
        uint32_t pc;
        uint32_t address;
    } seen[MAX_UNIQUE_NAN_LOADS];
    static uint32_t unique_count;
    static bool limit_reported;

    if (!mips_nan_load_trace_enabled() || !mips_bits_are_nan(value)) return;

    for (uint32_t i = 0; i < unique_count; i++) {
        if (seen[i].pc == pc && seen[i].address == address) return;
    }
    if (unique_count == MAX_UNIQUE_NAN_LOADS) {
        if (!limit_reported) {
            fprintf(stderr, "[NAN LOAD] unique-event limit reached (%u)\n",
                    MAX_UNIQUE_NAN_LOADS);
            limit_reported = true;
        }
        return;
    }

    seen[unique_count].pc = pc;
    seen[unique_count].address = address;
    unique_count++;
    fprintf(stderr,
            "[NAN LOAD] pc=%08X inst=%08X reg=f%u addr=%08X value=%08X "
            "base=r%u:%08X offset=%04X sp=%08X icount=%llu\n",
            pc, inst, target_reg, address, value, (inst >> 21) & 31,
            cpu->gpr[(inst >> 21) & 31], inst & 0xFFFFu, cpu->gpr[29],
            (unsigned long long)mips_interpreter_icount);
    fflush(stderr);
}

typedef struct {
    uint64_t count;
    uint64_t first_icount;
    uint64_t last_icount;
    uint32_t pc;
    bool summary_registered;
} MIPS_VTFM_TRACE;

static MIPS_VTFM_TRACE mips_vtfm_trace;

static void mips_trace_vtfm_summary(void) {
    fprintf(stderr,
            "[VFPU TFM SUMMARY] pc=%08X count=%llu first_icount=%llu "
            "last_icount=%llu\n",
            mips_vtfm_trace.pc, (unsigned long long)mips_vtfm_trace.count,
            (unsigned long long)mips_vtfm_trace.first_icount,
            (unsigned long long)mips_vtfm_trace.last_icount);
}

static bool mips_trace_vtfm_enabled(uint32_t pc) {
    static int initialized;
    static bool enabled;
    static uint32_t watched_pc = 0x089C4750u;
    if (!initialized) {
        const char *setting = getenv("MIPS_TRACE_VFPU_TFM");
        enabled = setting != NULL && setting[0] != '\0' &&
                  strcmp(setting, "0") != 0;
        const char *pc_setting = getenv("MIPS_TRACE_VFPU_TFM_PC");
        if (pc_setting != NULL && pc_setting[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(pc_setting, &end, 0);
            if (end != pc_setting && *end == '\0' && parsed <= UINT32_MAX) {
                watched_pc = (uint32_t)parsed;
            } else {
                fprintf(stderr,
                        "[VFPU TFM ERROR] Invalid MIPS_TRACE_VFPU_TFM_PC '%s'.\n",
                        pc_setting);
            }
        }
        if (enabled) {
            mips_vtfm_trace.pc = watched_pc;
            if (atexit(mips_trace_vtfm_summary) == 0) {
                mips_vtfm_trace.summary_registered = true;
            }
        }
        initialized = 1;
    }
    return enabled && pc == watched_pc;
}

static bool mips_trace_vtfm_pc(uint32_t pc, uint32_t inst) {
    if (!mips_trace_vtfm_enabled(pc)) return false;
    if (mips_vtfm_trace.count == 0) {
        mips_vtfm_trace.first_icount = mips_interpreter_icount;
    }
    mips_vtfm_trace.pc = pc;
    mips_vtfm_trace.count++;
    mips_vtfm_trace.last_icount = mips_interpreter_icount;
    if (mips_vtfm_trace.count <= 8 ||
        (mips_vtfm_trace.count % 1024) == 0) {
        fprintf(stderr,
                "[VFPU TFM PC] pc=%08X inst=%08X count=%llu icount=%llu\n",
                pc, inst, (unsigned long long)mips_vtfm_trace.count,
                (unsigned long long)mips_interpreter_icount);
        fflush(stderr);
    }
    return true;
}

static void mips_trace_vtfm_hit(uint32_t pc, uint32_t inst,
                                uint32_t matrix_size, uint32_t vector_size,
                                uint32_t sprefix, uint32_t tprefix,
                                uint32_t dprefix, const float *matrix,
                                const float *vector, const float *result,
                                bool trace_pc) {
    if (!trace_pc) return;
    if (mips_vtfm_trace.count > 8 &&
        (mips_vtfm_trace.count % 1024) != 0) return;

    fprintf(stderr,
            "[VFPU TFM HIT] pc=%08X inst=%08X count=%llu icount=%llu "
            "kind=%s msize=%u vsize=%u pfx=%08X,%08X,%08X M=",
            pc, inst, (unsigned long long)mips_vtfm_trace.count,
            (unsigned long long)mips_interpreter_icount,
            vector_size == ((inst >> 23) & 3) ? "vhtfm" : "vtfm",
            matrix_size, vector_size, sprefix, tprefix, dprefix);
    for (uint32_t i = 0; i < matrix_size * matrix_size; i++) {
        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                mips_float_bits(matrix[i]));
    }
    fprintf(stderr, " V=");
    for (uint32_t i = 0; i < matrix_size; i++) {
        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                mips_float_bits(vector[i]));
    }
    fprintf(stderr, " D=");
    for (uint32_t i = 0; i < matrix_size; i++) {
        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                mips_float_bits(result[i]));
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static bool mips_nan_origin_enabled(uint64_t *limit) {
    static int initialized;
    static bool enabled;
    static uint64_t event_limit;
    if (!initialized) {
        const char *setting = getenv("MIPS_TRACE_NAN_ORIGIN");
        enabled = setting != NULL && setting[0] != '\0' &&
                  strcmp(setting, "0") != 0;
        const char *limit_setting = getenv("MIPS_TRACE_NAN_ORIGIN_LIMIT");
        if (limit_setting != NULL && limit_setting[0] != '\0') {
            char *end = NULL;
            unsigned long long parsed = strtoull(limit_setting, &end, 0);
            if (end != limit_setting && *end == '\0') {
                event_limit = (uint64_t)parsed;
            }
        }
        if (event_limit == 0) event_limit = 128;
        initialized = 1;
    }
    *limit = event_limit;
    return enabled;
}

static void mips_trace_nan_registers(MIPS_CPU *cpu, uint32_t pc,
                                     uint32_t inst,
                                     const uint32_t old_fpr[32],
                                     const uint32_t old_vfpu[128]) {
    static uint64_t events;
    uint64_t limit;
    if (!mips_nan_origin_enabled(&limit) || events >= limit) return;

    for (uint32_t i = 0; i < 32 && events < limit; i++) {
        uint32_t value = mips_float_bits(cpu->fpr[i]);
        if (mips_bits_are_nan(value) && value != old_fpr[i] &&
            mips_nan_origin_matches(value)) {
            fprintf(stderr,
                    "[NAN ORIGIN] pc=%08X inst=%08X bank=FPR reg=%u "
                    "old=%08X value=%08X fcr31=%08X "
                    "a0=%08X a1=%08X a2=%08X a3=%08X "
                    "fs=%08X ft=%08X fd=%08X\n",
                    pc, inst, i, old_fpr[i], value, cpu->fcr31,
                    cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
                    old_fpr[(inst >> 11) & 31],
                    old_fpr[(inst >> 16) & 31],
                    old_fpr[(inst >> 6) & 31]);
            events++;
        }
    }
    for (uint32_t i = 0; i < 128 && events < limit; i++) {
        uint32_t value = mips_float_bits(cpu->vfpu[i]);
        if (mips_bits_are_nan(value) && value != old_vfpu[i] &&
            mips_nan_origin_matches(value)) {
            fprintf(stderr,
                    "[NAN ORIGIN] pc=%08X inst=%08X bank=VFPU reg=%u "
                    "old=%08X value=%08X vcr=%08X,%08X,%08X,%08X "
                    "a0=%08X a1=%08X a2=%08X a3=%08X\n",
                    pc, inst, i, old_vfpu[i], value,
                    cpu->vcr[0], cpu->vcr[1], cpu->vcr[2], cpu->vcr[3],
                    cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
            events++;
        }
    }
    if (events >= limit) {
        fprintf(stderr, "[NAN ORIGIN] event limit reached (%llu)\n",
                (unsigned long long)limit);
    }
}

void mips_interpreter_step(MIPS_CPU *cpu) {
    mips_interpreter_icount++;
    cpu->gpr[0] = 0; // Guard-rail: Enforce MIPS zero-register hardwired invariant
    uint32_t pc = cpu->pc;
    uint32_t trace_next_pc = cpu->trace_next_pc_override
        ? cpu->trace_next_pc_override_value : cpu->next_pc;
    cpu->trace_next_pc_override = false;
    cpu->trace_current_pc = pc;
    cpu->trace_current_inst = 0;
    uint32_t inst = mips_read32(cpu, pc);
    bool trace_vtfm_pc = mips_trace_vtfm_pc(pc, inst);
    uint32_t old_fpr[32];
    uint32_t old_vfpu[128];
    bool trace_nan_origin =
        mips_nan_origin_enabled(&(uint64_t){0});
    if (trace_nan_origin) {
        for (uint32_t i = 0; i < 32; i++) {
            old_fpr[i] = mips_float_bits(cpu->fpr[i]);
        }
        for (uint32_t i = 0; i < 128; i++) {
            old_vfpu[i] = mips_float_bits(cpu->vfpu[i]);
        }
    }
    if (cpu->trace_enabled) {
        mips_trace_instruction(cpu, pc, trace_next_pc, inst);
    }

    // Resolve branch delay slot paradigm
    cpu->pc = cpu->next_pc;
    cpu->next_pc = cpu->pc + 4;

    uint32_t opcode = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1F;
    uint32_t rt = (inst >> 16) & 0x1F;
    uint32_t rd = (inst >> 11) & 0x1F;
    uint32_t shamt = (inst >> 6) & 0x1F;
    uint32_t funct = inst & 0x3F;
    trace_allegrex_special_candidate(cpu, pc, inst, opcode, rs, rt, rd,
                                     shamt, funct);

    int16_t simm = (int16_t)(inst & 0xFFFF);
    uint16_t imm = inst & 0xFFFF;
    uint32_t target = inst & 0x03FFFFFF;

    bool branch_taken = false;
    uint32_t branch_target = 0;

    cpu->cycles++;

    // WATCHDOG DE DIAGNOSTICO: o hang silencioso logo apos o primeiro
    // sceKernelPowerLock nao gera nenhum printf (toda syscall que retorna
    // sucesso e' muda), entao nao tinhamos como saber ONDE o PC estava preso.
    // Isto imprime PC + a instrucao crua a cada 5 milhoes de ciclos, sem
    // alterar nenhum comportamento de execucao. Remover depois de achar o
    // endereco (ou os poucos enderecos, se for um laco de >1 instrucao).
    {
        static unsigned long long watchdog_ticks = 0;
        watchdog_ticks++;
        if ((watchdog_ticks % 5000000ULL) == 0) {
            printf("[WATCHDOG] ciclo=%llu pc=0x%08X next_pc=0x%08X inst=0x%08X "
                   "a0=0x%08X v0=0x%08X ra=0x%08X sp=0x%08X "
                   "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X t0=0x%08X t1=0x%08X\n",
                   watchdog_ticks, pc, cpu->next_pc, inst,
                   cpu->gpr[4], cpu->gpr[2], cpu->gpr[31], cpu->gpr[29],
                   cpu->gpr[16], cpu->gpr[17], cpu->gpr[18], cpu->gpr[19],
                   cpu->gpr[8], cpu->gpr[9]);
            fflush(stdout);
        }
    }

    switch (opcode) {
        case 0x00: { // SPECIAL / R-Type
            switch (funct) {
                case 0x00: // SLL
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] << shamt;
                    break;
                case 0x02: // SRL
                    if (rd != 0) {
                        uint32_t value = cpu->gpr[rt];
                        cpu->gpr[rd] = rs == 1
                            ? (value >> shamt) |
                              (value << ((32u - shamt) & 31u))
                            : value >> shamt;
                    }
                    break;
                case 0x03: // SRA
                    if (rd != 0) cpu->gpr[rd] = (int32_t)cpu->gpr[rt] >> shamt;
                    break;
                case 0x04: // SLLV
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] << (cpu->gpr[rs] & 0x1F);
                    break;
                case 0x06: // SRLV
                    if (rd != 0) {
                        uint32_t shift = cpu->gpr[rs] & 0x1F;
                        uint32_t value = cpu->gpr[rt];
                        cpu->gpr[rd] = shamt == 1
                            ? (value >> shift) |
                              (value << ((32u - shift) & 31u))
                            : value >> shift;
                    }
                    break;
                case 0x07: // SRAV
                    if (rd != 0) cpu->gpr[rd] = (int32_t)cpu->gpr[rt] >> (cpu->gpr[rs] & 0x1F);
                    break;
                case 0x08: // JR
                    branch_taken = true;
                    branch_target = cpu->gpr[rs];
                    break;
                case 0x09: // JALR
                    if (rd != 0) cpu->gpr[rd] = pc + 8;
                    branch_taken = true;
                    branch_target = cpu->gpr[rs];
                    break;
                case 0x0A: // MOVZ (Move on Zero - PSP Allegrex)
                    if (rd != 0 && cpu->gpr[rt] == 0) {
                        cpu->gpr[rd] = cpu->gpr[rs];
                    }
                    break;
                case 0x0B: // MOVN (Move on Not Zero - PSP Allegrex)
                    if (rd != 0 && cpu->gpr[rt] != 0) {
                        cpu->gpr[rd] = cpu->gpr[rs];
                    }
                    break;
                case 0x0C: // SYSCALL
                    cpu->pc = pc; // Restores exact PC of the syscall instruction for correct HLE routing
                    hle_syscall(cpu);
                    break;
                case 0x0D: // BREAK
                    printf("[MIPS EXCEPTION] BREAK at PC=0x%08X inst=0x%08X "
                           "ra=0x%08X sp=0x%08X bad_vaddr=0x%08X "
                           "last_fault_pc=0x%08X last_fault_inst=0x%08X "
                           "last_fault_access=0x%03X faults=%u\n",
                           pc, inst, cpu->gpr[31], cpu->gpr[29],
                           cpu->bad_vaddr, cpu->last_fault_pc,
                           cpu->last_fault_inst, cpu->last_fault_access,
                           cpu->memory_fault_count);
                    mips_postmortem_dump(cpu, "mips_break");
                    fflush(stdout);
                    cpu->exit_requested = true;
                    break;
                case 0x0F: // SYNC
                    /*
                     * Allegrex has no host-visible memory-ordering action
                     * here. The instruction is still valid and must not
                     * turn into a zero-result operation.
                     */
                    break;
                case 0x10: // MFHI (Move From HI)
                    if (rd != 0) cpu->gpr[rd] = cpu->hi;
                    break;
                case 0x11: // MTHI (Move To HI)
                    cpu->hi = cpu->gpr[rs];
                    break;
                case 0x12: // MFLO (Move From LO)
                    if (rd != 0) cpu->gpr[rd] = cpu->lo;
                    break;
                case 0x13: // MTLO (Move To LO)
                    cpu->lo = cpu->gpr[rs];
                    break;
                case 0x18: { // MULT (Signed Multiply)
                    int64_t res = (int64_t)(int32_t)cpu->gpr[rs] * (int64_t)(int32_t)cpu->gpr[rt];
                    cpu->lo = (uint32_t)(res & 0xFFFFFFFF);
                    cpu->hi = (uint32_t)(res >> 32);
                    break;
                }
                case 0x19: { // MULTU (Unsigned Multiply)
                    uint64_t res = (uint64_t)cpu->gpr[rs] * (uint64_t)cpu->gpr[rt];
                    cpu->lo = (uint32_t)(res & 0xFFFFFFFF);
                    cpu->hi = (uint32_t)(res >> 32);
                    break;
                }
                case 0x1A: { // DIV (Signed Divide)
                    if (cpu->gpr[rt] != 0) {
                        cpu->lo = (uint32_t)((int32_t)cpu->gpr[rs] / (int32_t)cpu->gpr[rt]);
                        cpu->hi = (uint32_t)((int32_t)cpu->gpr[rs] % (int32_t)cpu->gpr[rt]);
                    }
                    break;
                }
                case 0x1B: { // DIVU (Unsigned Divide)
                    if (cpu->gpr[rt] != 0) {
                        cpu->lo = cpu->gpr[rs] / cpu->gpr[rt];
                        cpu->hi = cpu->gpr[rs] % cpu->gpr[rt];
                    }
                    break;
                }
                case 0x16: // CLZ
                    if (rd != 0) {
                        uint32_t value = cpu->gpr[rs];
                        cpu->gpr[rd] = value != 0 ? (uint32_t)__builtin_clz(value)
                                                   : 32u;
                    }
                    break;
                case 0x17: // CLO
                    if (rd != 0) {
                        uint32_t value = ~cpu->gpr[rs];
                        cpu->gpr[rd] = value != 0 ? (uint32_t)__builtin_clz(value)
                                                   : 32u;
                    }
                    break;
                case 0x1C: { // MADD
                    uint64_t accumulator = ((uint64_t)cpu->hi << 32) | cpu->lo;
                    int64_t product = (int64_t)(int32_t)cpu->gpr[rs] *
                                      (int64_t)(int32_t)cpu->gpr[rt];
                    uint64_t result = accumulator + (uint64_t)product;
                    cpu->lo = (uint32_t)result;
                    cpu->hi = (uint32_t)(result >> 32);
                    break;
                }
                case 0x1D: { // MADDU
                    uint64_t accumulator = ((uint64_t)cpu->hi << 32) | cpu->lo;
                    uint64_t product = (uint64_t)cpu->gpr[rs] *
                                       cpu->gpr[rt];
                    uint64_t result = accumulator + product;
                    cpu->lo = (uint32_t)result;
                    cpu->hi = (uint32_t)(result >> 32);
                    break;
                }
                case 0x20: // ADD
                case 0x21: // ADDU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] - cpu->gpr[rt];
                    break;
                case 0x2C: // MAX (signed)
                    if (rd != 0) {
                        cpu->gpr[rd] = (int32_t)cpu->gpr[rs] >
                                (int32_t)cpu->gpr[rt]
                            ? cpu->gpr[rs] : cpu->gpr[rt];
                    }
                    break;
                case 0x2D: // MIN (signed)
                    if (rd != 0) {
                        cpu->gpr[rd] = (int32_t)cpu->gpr[rs] <
                                (int32_t)cpu->gpr[rt]
                            ? cpu->gpr[rs] : cpu->gpr[rt];
                    }
                    break;
                case 0x2E: { // MSUB
                    uint64_t accumulator = ((uint64_t)cpu->hi << 32) | cpu->lo;
                    int64_t product = (int64_t)(int32_t)cpu->gpr[rs] *
                                      (int64_t)(int32_t)cpu->gpr[rt];
                    uint64_t result = accumulator - (uint64_t)product;
                    cpu->lo = (uint32_t)result;
                    cpu->hi = (uint32_t)(result >> 32);
                    break;
                }
                case 0x2F: { // MSUBU
                    uint64_t accumulator = ((uint64_t)cpu->hi << 32) | cpu->lo;
                    uint64_t product = (uint64_t)cpu->gpr[rs] *
                                       cpu->gpr[rt];
                    uint64_t result = accumulator - product;
                    cpu->lo = (uint32_t)result;
                    cpu->hi = (uint32_t)(result >> 32);
                    break;
                }
                case 0x24: // AND
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] & cpu->gpr[rt];
                    break;
                case 0x25: // OR
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] | cpu->gpr[rt];
                    break;
                case 0x26: // XOR
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] ^ cpu->gpr[rt];
                    break;
                case 0x27: // NOR
                    if (rd != 0) cpu->gpr[rd] = ~(cpu->gpr[rs] | cpu->gpr[rt]);
                    break;
                case 0x2A: // SLT
                    if (rd != 0) cpu->gpr[rd] = ((int32_t)cpu->gpr[rs] < (int32_t)cpu->gpr[rt]) ? 1 : 0;
                    break;
                case 0x2B: // SLTU
                    if (rd != 0) cpu->gpr[rd] = (cpu->gpr[rs] < cpu->gpr[rt]) ? 1 : 0;
                    break;
                default:
                    printf("[Interpreter Warning] Unsupported R-Type funct 0x%02X at PC 0x%08X (Inst: 0x%08X)\n", funct, pc, inst);
                    break;
            }
            break;
        }
        case 0x01: { // REGIMM
            bool link = (rt == 0x10 || rt == 0x11 ||
                         rt == 0x12 || rt == 0x13);
            bool likely = (rt == 0x02 || rt == 0x03 ||
                           rt == 0x12 || rt == 0x13);
            bool condition = false;
            if (rt == 0) { // BLTZ
                condition = (int32_t)cpu->gpr[rs] < 0;
            } else if (rt == 1) { // BGEZ
                condition = (int32_t)cpu->gpr[rs] >= 0;
            } else if (rt == 2) { // BLTZL (Branch on Less Than Zero Likely)
                condition = (int32_t)cpu->gpr[rs] < 0;
            } else if (rt == 3) { // BGEZL (Branch on Greater Than or Equal to Zero Likely)
                condition = (int32_t)cpu->gpr[rs] >= 0;
            } else if (rt == 0x10 || rt == 0x12) { // BLTZAL / BLTZALL
                condition = (int32_t)cpu->gpr[rs] < 0;
            } else if (rt == 0x11 || rt == 0x13) { // BGEZAL / BGEZALL
                condition = (int32_t)cpu->gpr[rs] >= 0;
            } else {
                printf("[Interpreter Warning] Unsupported REGIMM rt=0x%02X "
                       "at PC 0x%08X (Inst: 0x%08X)\n", rt, pc, inst);
                break;
            }

            if (condition) {
                if (link) cpu->gpr[31] = pc + 8;
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else if (likely) {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x02: // J
            branch_taken = true;
            branch_target = (pc & 0xF0000000) | (target * 4);
            break;
        case 0x03: // JAL
            cpu->gpr[31] = pc + 8; // Link register r31
            branch_taken = true;
            branch_target = (pc & 0xF0000000) | (target * 4);
            break;
        case 0x04: // BEQ
            if (cpu->gpr[rs] == cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x05: // BNE
            if (cpu->gpr[rs] != cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x06: // BLEZ
            if ((int32_t)cpu->gpr[rs] <= 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x07: // BGTZ
            if ((int32_t)cpu->gpr[rs] > 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x08: // ADDI
        case 0x09: // ADDIU
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] + simm;
            }
            break;
        case 0x0A: // SLTI
            if (rt != 0) {
                cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < simm) ? 1 : 0;
            }
            break;
        case 0x0B: // SLTIU
            if (rt != 0) {
                cpu->gpr[rt] = (cpu->gpr[rs] < (uint32_t)simm) ? 1 : 0;
            }
            break;
        case 0x0C: // ANDI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] & imm;
            }
            break;
        case 0x0D: // ORI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] | imm;
            }
            break;
        case 0x0E: // XORI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] ^ imm;
            }
            break;
        case 0x0F: // LUI
            if (rt != 0) {
                cpu->gpr[rt] = imm << 16;
            }
            break;
        case 0x20: // LB
            if (rt != 0) cpu->gpr[rt] = (int32_t)(int8_t)mips_read8(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x21: // LH
            if (rt != 0) cpu->gpr[rt] = (int32_t)(int16_t)mips_read16(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x23: // LW
            if (rt != 0) cpu->gpr[rt] = mips_read32(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x24: // LBU
            if (rt != 0) cpu->gpr[rt] = mips_read8(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x25: // LHU
            if (rt != 0) cpu->gpr[rt] = mips_read16(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x28: // SB
            mips_write8(cpu, cpu->gpr[rs] + simm, (uint8_t)cpu->gpr[rt]);
            break;
        case 0x29: // SH
            mips_write16(cpu, cpu->gpr[rs] + simm, (uint16_t)cpu->gpr[rt]);
            break;
        case 0x2B: // SW
            mips_write32(cpu, cpu->gpr[rs] + simm, cpu->gpr[rt]);
            break;
        case 0x22: { // LWL (Load Word Left)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t word = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            if (rt != 0) {
                cpu->gpr[rt] = (cpu->gpr[rt] & (0x00FFFFFFu >> shift)) |
                               (word << (24 - shift));
            }
            break;
        }
        case 0x26: { // LWR (Load Word Right)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t word = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            if (rt != 0) {
                cpu->gpr[rt] = (cpu->gpr[rt] &
                                (0xFFFFFF00u << (24 - shift))) |
                               (word >> shift);
            }
            break;
        }
        case 0x2A: { // SWL (Store Word Left)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t mem = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            uint32_t new_mem =
                (cpu->gpr[rt] >> (24 - shift)) |
                (mem & (0xFFFFFF00u << shift));
            mips_write32(cpu, aligned_addr, new_mem);
            break;
        }
        case 0x2E: { // SWR (Store Word Right)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t mem = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            uint32_t new_mem =
                (cpu->gpr[rt] << shift) |
                (mem & (0x00FFFFFFu >> (24 - shift)));
            mips_write32(cpu, aligned_addr, new_mem);
            break;
        }
        case 0x2F: // CACHE: the PSP's cache is not separately emulated.
            break;
        case 0x1C: { // SPECIAL2 (Allegrex custom: clz, clo, halt)
            if (inst == 0x70000000) { // HALT
                cpu->exit_requested = true;
                return;
            }
            switch (funct) {
                case 0x20: // CLZ
                    if (rd != 0) {
                        uint32_t val = cpu->gpr[rs];
                        cpu->gpr[rd] = val ? __builtin_clz(val) : 32;
                    }
                    break;
                case 0x21: // CLO
                    if (rd != 0) {
                        uint32_t val = ~cpu->gpr[rs];
                        cpu->gpr[rd] = val ? __builtin_clz(val) : 32;
                    }
                    break;
                case 0x24: // MFIC - Move From Interrupt Controller (Allegrex)
                    if (rt != 0) cpu->gpr[rt] = cpu->ic_state;
                    break;
                case 0x26: // MTIC - Move To Interrupt Controller (Allegrex)
                    cpu->ic_state = cpu->gpr[rt];
                    break;
                default:
                    printf("[Interpreter Warning] Unsupported SPECIAL2 funct 0x%02X (inst=0x%08X, rs=%d, rt=%d, rd=%d) at PC 0x%08X\n", funct, inst, rs, rt, rd, pc);
                    break;
            }
            break;
        }
        case 0x14: { // BEQL (Branch on Equal Likely)
            if (cpu->gpr[rs] == cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x15: { // BNEL (Branch on Not Equal Likely)
            if (cpu->gpr[rs] != cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x16: { // BLEZL (Branch on Less Than or Equal to Zero Likely)
            if ((int32_t)cpu->gpr[rs] <= 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x17: { // BGTZL (Branch on Greater Than Zero Likely)
            if ((int32_t)cpu->gpr[rs] > 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }

        case 0x11: { // COP1 (FPU Single/Compare/Branch)
            uint32_t sub_op = rs;
            if (sub_op == 0x00) { // MFC1
                if (rt != 0) cpu->gpr[rt] = mips_float_bits(cpu->fpr[rd]);
            } else if (sub_op == 0x04) { // MTC1
                memcpy(&cpu->fpr[rd], &cpu->gpr[rt], sizeof(cpu->gpr[rt]));
            } else if (sub_op == 0x02) { // CFC1
                if (rt != 0) cpu->gpr[rt] = cpu->fcr31;
            } else if (sub_op == 0x06) { // CTC1
                cpu->fcr31 = cpu->gpr[rt];
            } else if (sub_op == 0x08) { // BC1 branches
                uint32_t cc = (rt >> 2) & 7;
                uint32_t bit = (cc == 0) ? 23 : (24 + cc);
                bool cond = (cpu->fcr31 & (1 << bit)) != 0;
                uint32_t branch_type = rt & 3;
                if (branch_type == 0) { // BC1F
                    if (!cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    }
                } else if (branch_type == 1) { // BC1T
                    if (cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    }
                } else if (branch_type == 2) { // BC1FL
                    if (!cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    } else {
                        cpu->pc = pc + 8;
                        cpu->next_pc = pc + 12;
                    }
                } else if (branch_type == 3) { // BC1TL
                    if (cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    } else {
                        cpu->pc = pc + 8;
                        cpu->next_pc = pc + 12;
                    }
                }
            } else if (sub_op == 0x10) { // Format Single (.s)
                float fs_val = cpu->fpr[rd];
                float ft_val = cpu->fpr[rt];
                uint32_t fd = shamt;
                switch (funct) {
                    case 0x00: // ADD.S
                        cpu->fpr[fd] = fs_val + ft_val;
                        break;
                    case 0x01: // SUB.S
                        cpu->fpr[fd] = fs_val - ft_val;
                        break;
                    case 0x02: // MUL.S
                        if ((isinf(fs_val) && ft_val == 0.0f) ||
                            (isinf(ft_val) && fs_val == 0.0f)) {
                            const uint32_t canonical_nan = 0x7FC00000u;
                            memcpy(&cpu->fpr[fd], &canonical_nan,
                                   sizeof(canonical_nan));
                        } else {
                            cpu->fpr[fd] = fs_val * ft_val;
                        }
                        break;
                    case 0x03: // DIV.S
                        cpu->fpr[fd] = fs_val / ft_val;
                        break;
                    case 0x04: // SQRT.S
                        cpu->fpr[fd] = sqrtf(fs_val);
                        break;
                    case 0x05: // ABS.S
                        cpu->fpr[fd] = fabsf(fs_val);
                        break;
                    case 0x06: // MOV.S
                        cpu->fpr[fd] = fs_val;
                        break;
                    case 0x07: // NEG.S
                        cpu->fpr[fd] = -fs_val;
                        break;
                    case 0x0C: // ROUND.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)roundf(fs_val);
                        break;
                    case 0x0D: // TRUNC.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)fs_val;
                        break;
                    case 0x0E: // CEIL.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)ceilf(fs_val);
                        break;
                    case 0x0F: // FLOOR.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)floorf(fs_val);
                        break;
                    case 0x20: // CVT.S.W (colocado aqui por engano - fmt=S+funct=0x20
                               // nao e uma instrucao real que compilador nenhum gera;
                               // o CVT.S.W de verdade agora e tratado em fmt=W/sub_op 0x14
                               // logo abaixo. Deixado aqui sem alteracao de comportamento
                               // pra nao mudar nada que ja funcionava.
                        cpu->fpr[fd] = (float)*(int32_t*)&cpu->fpr[rd];
                        break;
                    case 0x24: // CVT.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)fs_val;
                        break;
                    default:
                        if (funct >= 0x30 && funct <= 0x3F) { // c.cond.s comparisons
                            bool cmp_cond = false;
                            switch (funct & 0xF) {
                                case 0x0: cmp_cond = false; break; // c.f.s
                                case 0x2: cmp_cond = (fs_val == ft_val); break; // c.eq.s
                                case 0x4: cmp_cond = (fs_val < ft_val); break; // c.lt.s (c.olt.s)
                                case 0x6: cmp_cond = (fs_val <= ft_val); break; // c.le.s (c.ole.s)
                                case 0xC: cmp_cond = (fs_val < ft_val); break; // c.lt.s (c.ult.s)
                                case 0xE: cmp_cond = (fs_val <= ft_val); break; // c.le.s (c.ule.s)
                                default: cmp_cond = (fs_val == ft_val); break;
                            }
                            uint32_t cc = (inst >> 8) & 7;
                            uint32_t bit = (cc == 0) ? 23 : (24 + cc);
                            cpu->fcr31 = (cpu->fcr31 & ~(1 << bit)) | (cmp_cond ? (1 << bit) : 0);
                        } else {
                            printf("[Interpreter Warning] Unsupported COP1 Single funct 0x%02X at PC 0x%08X\n", funct, pc);
                        }
                        break;
                }
            } else if (sub_op == 0x14) { // Format Word (.w) - operando fonte
                // e' um inteiro de 32 bits guardado nos bits de fpr[rd] (nao
                // um float). Antes essa combinacao (fmt=W) nao tinha
                // tratamento nenhum e caia no 'else' generico de baixo -
                // que e' exatamente o aviso "Unsupported COP1 sub_op 0x14"
                // que aparecia nos logs. O CVT.S.W (converter inteiro pra
                // float) que existia antes estava colocado por engano
                // dentro do bloco fmt=S (sub_op 0x10), onde a instrucao
                // correspondente nunca e' realmente gerada por compilador
                // nenhum (fmt=S + funct 0x20 seria "CVT.S.S", que nao
                // existe) - o real CVT.S.W sempre vem com fmt=W, aqui.
                int32_t fs_int = *(int32_t*)&cpu->fpr[rd];
                uint32_t fd = shamt;
                switch (funct) {
                    case 0x20: // CVT.S.W - converte o inteiro fs_int para float, guarda em fd
                        cpu->fpr[fd] = (float)fs_int;
                        break;
                    default:
                        printf("[Interpreter Warning] Unsupported COP1 Word funct 0x%02X at PC 0x%08X\n", funct, pc);
                        break;
                }
            } else {
                printf("[Interpreter Warning] Unsupported COP1 sub_op 0x%02X at PC 0x%08X\n", sub_op, pc);
            }
            break;
        }
                case 0x12: { // COP2 (VFPU register moves)
            if (rs == 0x00) { // MFC2
                if (rt != 0) cpu->gpr[rt] = *(uint32_t*)&cpu->vfpu[rd];
                    } else if (rs == 0x03) { // MFV: VFPU scalar register -> GPR
                        uint32_t vfpu_reg = inst & 0xFF;
                        if (rt != 0) {
                            if (vfpu_reg < 128) {
                                memcpy(&cpu->gpr[rt], &cpu->vfpu[vfpu_reg],
                                       sizeof(uint32_t));
                            } else if (vfpu_reg < 132) {
                                cpu->gpr[rt] = cpu->vcr[vfpu_reg - 128];
                            }
                        }
                    } else if (rs == 0x04) { // MTC2
                        *(uint32_t*)&cpu->vfpu[rd] = cpu->gpr[rt];
                    } else if (rs == 0x07) { // MTV: GPR -> VFPU scalar register
                        uint32_t vfpu_reg = inst & 0xFF;
                        if (vfpu_reg < 128) {
                            memcpy(&cpu->vfpu[vfpu_reg], &cpu->gpr[rt], sizeof(uint32_t));
                        } else if (vfpu_reg < 132) {
                            uint32_t ctrl = vfpu_reg - 128;
                            uint32_t mask = ctrl == 0 || ctrl == 1
                                ? 0x000FFFFFu
                                : (ctrl == 2 ? 0x00000FFFu : 0x0000003Fu);
                            cpu->vcr[ctrl] = cpu->gpr[rt] & mask;
                        }
                    } else if (rs == 0x02) { // CFC2
                        if (rt != 0) {
                            if (rd < 4) cpu->gpr[rt] = cpu->vcr[rd];
                    else cpu->gpr[rt] = 0;
                }
            } else if (rs == 0x06) { // CTC2
                if (rd < 4) cpu->vcr[rd] = cpu->gpr[rt];
            }
            break;
        }
        case 0x18: { // VFPU vadd / vsub / vdiv
            static unsigned int vfpu_binary_count = 0;
            vfpu_binary_count++;
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;
            uint32_t dim = 1u + ((inst >> 7) & 1u) +
                           (((inst >> 15) & 1u) << 1);
            if (format == 0 || format == 1) {
                uint8_t dst_regs[4], src_regs[4], target_regs[4];
                float source[4] = {0};
                float target[4] = {0};
                float result[4] = {0};
                vfpu_get_vector_regs(dst_regs, dim, vd);
                vfpu_get_vector_regs(src_regs, dim, vs);
                vfpu_get_vector_regs(target_regs, dim, vt);
                for (uint32_t i = 0; i < dim; i++) {
                    source[i] = cpu->vfpu[src_regs[i]];
                    target[i] = cpu->vfpu[target_regs[i]];
                }
                vfpu_apply_prefix_st(source, cpu->vcr[0], dim);
                vfpu_apply_prefix_st(target, cpu->vcr[1], dim);
                for (uint32_t i = 0; i < dim; i++) {
                    result[i] = format == 0
                        ? source[i] + target[i]
                        : source[i] - target[i];
                }
                uint32_t destination_prefix = cpu->vcr[2];
                vfpu_apply_prefix_d(result, dim, destination_prefix);
                for (uint32_t i = 0; i < dim; i++) {
                    if ((destination_prefix & (1u << (8 + i))) == 0) {
                        cpu->vfpu[dst_regs[i]] = result[i];
                    }
                }
                vfpu_eat_prefixes(cpu);
            } else if (format == 7) { // vdiv
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128 && cpu->vfpu[vt + i] != 0.0f) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] / cpu->vfpu[vt + i];
                    }
                }
            }
            if ((vfpu_binary_count <= 32 || (vfpu_binary_count % 4096) == 0) &&
                vd < 128) {
                printf("[VFPU DEBUG] binary PC=0x%08X op=0x18 format=%u "
                       "vd=%u result={%08X,%08X,%08X,%08X}\n",
                       pc, format, vd,
                       *(uint32_t *)&cpu->vfpu[vd],
                       vd + 1 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 1] : 0,
                       vd + 2 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 2] : 0,
                       vd + 3 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 3] : 0);
            }
            break;
        }
        case 0x19: { // VFPU vmul / vdot / vhdp
            static unsigned int vfpu_mul_count = 0;
            vfpu_mul_count++;
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;
            if (format == 0) { // vmul.q
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] * cpu->vfpu[vt + i];
                    }
                }
            } else if (format == 1) { // vdot (Dot Product)
                uint32_t size =
                    ((inst >> 7) & 1) + ((inst >> 14) & 2) + 1;
                uint8_t dst_regs[4], src_regs[4], target_regs[4];
                float source[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float target[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                vfpu_get_vector_regs(dst_regs, 1, vd);
                vfpu_get_vector_regs(src_regs, size, vs);
                vfpu_get_vector_regs(target_regs, size, vt);
                for (uint32_t i = 0; i < size; i++) {
                    source[i] = cpu->vfpu[src_regs[i]];
                    target[i] = cpu->vfpu[target_regs[i]];
                }
                float sum = 0.0f;
                for (int i = 0; i < 4; i++) {
                    sum += source[i] * target[i];
                }
                cpu->vfpu[dst_regs[0]] = sum;
            } else if (format == 4) { // vhdp.q (Homogeneous Dot Product)
                float sum = 0.0f;
                for (int i = 0; i < 4; i++) {
                    if (vs+i < 128) {
                        float vt_val = (i == 3) ? 1.0f : ((vt+i < 128) ? cpu->vfpu[vt + i] : 0.0f);
                        sum += cpu->vfpu[vs + i] * vt_val;
                    }
                }
                if (vd < 128) {
                    cpu->vfpu[vd] = sum;
                }
            }
            if ((vfpu_mul_count <= 32 || (vfpu_mul_count % 4096) == 0) &&
                vd < 128) {
                printf("[VFPU DEBUG] binary PC=0x%08X op=0x19 format=%u "
                       "vd=%u result={%08X,%08X,%08X,%08X}\n",
                       pc, format, vd,
                       *(uint32_t *)&cpu->vfpu[vd],
                       vd + 1 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 1] : 0,
                       vd + 2 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 2] : 0,
                       vd + 3 < 128 ? *(uint32_t *)&cpu->vfpu[vd + 3] : 0);
            }
            break;
        }
        case 0x31: { // LWC1
            uint32_t address = cpu->gpr[rs] + simm;
            uint32_t value = mips_read32(cpu, address);
            memcpy(&cpu->fpr[rt], &value, sizeof(value));
            mips_trace_nan_load(cpu, pc, inst, address, value, rt);
            break;
        }
        case 0x39: { // SWC1
            mips_write32(cpu, cpu->gpr[rs] + simm, *(uint32_t*)&cpu->fpr[rt]);
            break;
        }
        case 0x32: { // LV.S
            uint32_t vt = ((inst >> 16) & 0x1Fu) | ((inst & 3u) << 5);
            uint32_t addr = cpu->gpr[rs] +
                            (uint32_t)(int32_t)(int16_t)(inst & 0xFFFCu);
            uint8_t regs[4];
            uint32_t value = mips_read32(cpu, addr);
            vfpu_get_vector_regs(regs, 1, vt);
            memcpy(&cpu->vfpu[regs[0]], &value, sizeof(value));
            break;
        }
        case 0x36: { // LV.Q Allegrex official Vector Load
            uint32_t vt = ((inst >> 16) & 0x1Fu) | ((inst & 1u) << 5);
            uint32_t addr = cpu->gpr[rs] +
                            (uint32_t)(int32_t)(int16_t)(inst & 0xFFFCu);
            uint8_t regs[4];
            uint32_t values[4];
            vfpu_get_vector_regs(regs, 4, vt);
            for (uint32_t i = 0; i < 4; i++) {
                values[i] = mips_read32(cpu, addr + i * 4);
            }
            for (uint32_t i = 0; i < 4; i++) {
                memcpy(&cpu->vfpu[regs[i]], &values[i], sizeof(values[i]));
            }
            break;
        }
        case 0x37: { // VFPU prefixes and scalar immediates
            uint32_t type = (inst >> 23) & 7;
            if (type <= 5) {
                uint32_t prefix_index = (inst >> 24) & 3;
                if (prefix_index < 3) {
                    uint32_t prefix = inst & 0x000FFFFFu;
                    if (prefix_index == 2) prefix &= 0x00000FFFu;
                    cpu->vcr[prefix_index] = prefix;
                }
            } else {
                uint32_t vt = (inst >> 16) & 0x1F;
                uint8_t dst_regs[4];
                vfpu_get_vector_regs(dst_regs, 1, vt);
                float value;
                if (type == 6) {
                    value = (float)(int16_t)(inst & 0xFFFFu);
                } else {
                    uint32_t half = inst & 0xFFFFu;
                    uint32_t sign = (half & 0x8000u) << 16;
                    uint32_t exponent = (half >> 10) & 0x1Fu;
                    uint32_t fraction = half & 0x3FFu;
                    uint32_t bits;
                    if (exponent == 0) {
                        if (fraction == 0) {
                            bits = sign;
                        } else {
                            int32_t normalized_exponent = -14;
                            while ((fraction & 0x400u) == 0) {
                                fraction <<= 1;
                                normalized_exponent--;
                            }
                            fraction &= 0x3FFu;
                            bits = sign |
                                   ((uint32_t)(normalized_exponent + 127) << 23) |
                                   (fraction << 13);
                        }
                    } else if (exponent == 0x1Fu) {
                        bits = sign | 0x7F800000u | (fraction << 13);
                    } else {
                        bits = sign | ((exponent + 112u) << 23) |
                               (fraction << 13);
                    }
                    memcpy(&value, &bits, sizeof(value));
                }
                if ((cpu->vcr[2] & (1u << 8)) == 0) {
                    vfpu_apply_prefix_d(&value, 1, cpu->vcr[2]);
                    memcpy(&cpu->vfpu[dst_regs[0]], &value, sizeof(value));
                }
                vfpu_eat_prefixes(cpu);
            }
            break;
        }
        case 0x3A: { // SV.S
            uint32_t vt = ((inst >> 16) & 0x1Fu) | ((inst & 3u) << 5);
            uint32_t addr = cpu->gpr[rs] +
                            (uint32_t)(int32_t)(int16_t)(inst & 0xFFFCu);
            uint8_t regs[4];
            uint32_t value;
            vfpu_get_vector_regs(regs, 1, vt);
            memcpy(&value, &cpu->vfpu[regs[0]], sizeof(value));
            mips_write32(cpu, addr, value);
            break;
        }
        case 0x3C: { // COP2 matrix operations
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;

            if (format == 0) { // vmmul (Matrix Multiplication - Pair, Triple, Quad)
                int dim = 2;
                uint32_t bit15 = (inst >> 15) & 1;
                uint32_t bit7 = (inst >> 7) & 1;
                if (bit15 == 0) dim = 2;
                else if (bit7 == 0) dim = 3;
                else dim = 4;

                float temp[16] = {0};
                for (int r = 0; r < dim; r++) {
                    for (int c = 0; c < dim; c++) {
                        float sum = 0.0f;
                        for (int k = 0; k < dim; k++) {
                            // VFPU matrices are column-major in storage
                            float val_s = cpu->vfpu[vs + k * 4 + r];
                            float val_t = cpu->vfpu[vt + c * 4 + k];
                            sum += val_s * val_t;
                        }
                        temp[c * 4 + r] = sum;
                    }
                }
                for (int i = 0; i < dim * 4; i++) {
                    if (vd + i < 128) cpu->vfpu[vd + i] = temp[i];
                }
            } else if (format >= 1 && format <= 3) { // vtfm / vhtfm
                uint32_t ins = (inst >> 23) & 3;
                uint32_t matrix_size = ins + 1;
                uint32_t vector_size =
                    1 + ((inst >> 7) & 1) + (((inst >> 14) & 2));
                uint32_t input_size =
                    vector_size < matrix_size ? vector_size : matrix_size;
                uint8_t matrix_regs[16], vector_regs[4], dst_regs[4];
                float matrix[16] = {0};
                float vector[4] = {0};
                float raw_vector[4] = {0};
                float result[4] = {0};

                vfpu_get_matrix_regs(matrix_regs, matrix_size, vs);
                vfpu_get_vector_regs(vector_regs, matrix_size, vt);
                vfpu_get_vector_regs(dst_regs, matrix_size, vd);
                for (uint32_t i = 0; i < matrix_size; i++) {
                    vector[i] = cpu->vfpu[vector_regs[i]];
                    for (uint32_t j = 0; j < matrix_size; j++) {
                        matrix[j * 4 + i] =
                            cpu->vfpu[matrix_regs[j * 4 + i]];
                    }
                }
                memcpy(raw_vector, vector, sizeof(raw_vector));

                for (uint32_t i = 0; i < ins; i++) {
                    for (uint32_t j = 0; j < input_size; j++) {
                        result[i] += matrix[j * 4 + i] * vector[j];
                    }
                    if (ins >= vector_size) {
                        result[i] += matrix[ins * 4 + i];
                    }
                }

                float final_matrix_row[4] = {0};
                for (uint32_t j = 0; j < 4; j++) {
                    if (j < matrix_size) {
                        final_matrix_row[j] = matrix[ins * 4 + j];
                    }
                }
                vfpu_apply_prefix_st(final_matrix_row, cpu->vcr[0], 4);

                uint32_t t_prefix = cpu->vcr[1];
                for (uint32_t i = 1; i < 4; i++) {
                    if (i >= vector_size) {
                        t_prefix = (t_prefix & ~(3u << (i * 2))) |
                                   (3u << (i * 2)) | (1u << (12 + i));
                    }
                }
                if (ins >= vector_size) {
                    t_prefix = (t_prefix & ~(3u << (ins * 2))) |
                               (1u << (ins * 2)) | (1u << (12 + ins));
                }
                vfpu_apply_prefix_st(vector, t_prefix, 4);
                for (uint32_t j = 0; j < 4; j++) {
                    result[ins] += final_matrix_row[j] * vector[j];
                }

                uint32_t d_prefix = cpu->vcr[2];
                mips_trace_vtfm_hit(pc, inst, matrix_size, vector_size,
                                    cpu->vcr[0], cpu->vcr[1], d_prefix,
                                    matrix, raw_vector, result,
                                    trace_vtfm_pc);
                bool result_has_nan = false;
                for (uint32_t i = 0; i < matrix_size; i++) {
                    result_has_nan |= mips_bits_are_nan(
                        vfpu_float_bits(result[i]));
                }
                if (result_has_nan) {
                    fprintf(stderr,
                            "[VFPU TFM NAN] pc=%08X inst=%08X kind=%s "
                            "size=%u vsize=%u pfx=%08X,%08X,%08X M=",
                            pc, inst, vector_size == ins ? "vhtfm" : "vtfm",
                            matrix_size, vector_size, cpu->vcr[0],
                            cpu->vcr[1], d_prefix);
                    for (uint32_t i = 0; i < matrix_size * matrix_size; i++) {
                        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                                vfpu_float_bits(matrix[i]));
                    }
                    fprintf(stderr, " V=");
                    for (uint32_t i = 0; i < matrix_size; i++) {
                        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                                vfpu_float_bits(raw_vector[i]));
                    }
                    fprintf(stderr, " D=");
                    for (uint32_t i = 0; i < matrix_size; i++) {
                        fprintf(stderr, "%s%08X", i == 0 ? "" : ",",
                                vfpu_float_bits(result[i]));
                    }
                    fputc('\n', stderr);
                }
                vfpu_apply_prefix_d(result, matrix_size, d_prefix);
                for (uint32_t i = 0; i < matrix_size; i++) {
                    if ((d_prefix & (1u << (8 + i))) == 0) {
                        cpu->vfpu[dst_regs[i]] = result[i];
                    }
                }
                vfpu_eat_prefixes(cpu);
            } else if (format == 7) { // vmidt / vmzero (Identity / Zero Matrices - Pair, Triple, Quad)
                uint32_t op_type = (inst >> 16) & 0x7F;
                int dim = 2;
                uint32_t bit15 = (inst >> 15) & 1;
                uint32_t bit7 = (inst >> 7) & 1;
                if (bit15 == 0) dim = 2;
                else if (bit7 == 0) dim = 3;
                else dim = 4;

                if (op_type == 3) { // vmidt
                    for (int c = 0; c < dim; c++) {
                        for (int r = 0; r < dim; r++) {
                            if (vd + c * 4 + r < 128) {
                                cpu->vfpu[vd + c * 4 + r] = (r == c) ? 1.0f : 0.0f;
                            }
                        }
                    }
                } else if (op_type == 6) { // vmzero
                    for (int c = 0; c < dim; c++) {
                        for (int r = 0; r < dim; r++) {
                            if (vd + c * 4 + r < 128) {
                                cpu->vfpu[vd + c * 4 + r] = 0.0f;
                            }
                        }
                    }
                }
            }
            break;
        }
        case 0x1B: // VFPU3: vcmp, vmin, vmax, vscmp, vsge, vslt
            vfpu_execute_vfpu3(cpu, pc, inst);
            break;
        case 0x3E: { // SV.Q Allegrex official Vector Store
            uint32_t vt = ((inst >> 16) & 0x1Fu) | ((inst & 1u) << 5);
            uint32_t addr = cpu->gpr[rs] +
                            (uint32_t)(int32_t)(int16_t)(inst & 0xFFFCu);
            uint8_t regs[4];
            uint32_t values[4];
            vfpu_get_vector_regs(regs, 4, vt);
            for (uint32_t i = 0; i < 4; i++) {
                memcpy(&values[i], &cpu->vfpu[regs[i]], sizeof(values[i]));
            }
            for (uint32_t i = 0; i < 4; i++) {
                mips_write32(cpu, addr + i * 4, values[i]);
            }
            break;
        }
        case 0x34: { // VFPU monadic / conversion operations
            static unsigned int vfpu_trace_count = 0;
            vfpu_trace_count++;
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t bit7 = (inst >> 7) & 1;
            uint32_t bit15 = (inst >> 15) & 1;
            uint32_t dim = 1 + bit7 + bit15 * 2;
            if (vfpu_trace_count <= 32 || (vfpu_trace_count % 4096) == 0) {
                printf("[VFPU DEBUG] monadic PC=0x%08X inst=0x%08X "
                       "rs=0x%02X rt=0x%02X vd=%u vs=%u dim=%u "
                       "in={%08X,%08X,%08X,%08X}\n",
                       pc, inst, rs, rt, vd, vs, dim,
                       *(uint32_t *)&cpu->vfpu[vs],
                       vs + 1 < 128 ? *(uint32_t *)&cpu->vfpu[vs + 1] : 0,
                       vs + 2 < 128 ? *(uint32_t *)&cpu->vfpu[vs + 2] : 0,
                       vs + 3 < 128 ? *(uint32_t *)&cpu->vfpu[vs + 3] : 0);
            }
            
            if (rs == 0x10) { // vf2in (float to int)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        float val = cpu->vfpu[vs + i];
                        *(int32_t*)&cpu->vfpu[vd + i] = (int32_t)roundf(val);
                    }
                }
            } else if (rs == 0x14) { // vi2f (int to float)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        cpu->vfpu[vd + i] = (float)val;
                    }
                }
            } else if (rs == 0x01 && rt == 0x1C) { // vi2uc (int to unsigned char)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        if (val < 0) val = 0;
                        if (val > 255) val = 255;
                        *(int32_t*)&cpu->vfpu[vd + i] = val;
                    }
                }
            } else if (rs == 0x01 && rt == 0x1F) { // vi2s (int to short)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        if (val < -32768) val = -32768;
                        if (val > 32767) val = 32767;
                        *(int32_t*)&cpu->vfpu[vd + i] = val;
                    }
                }
            } else if (rs == 0x00) { // Unary operations and vector move
                switch (rt) {
                    case 0: { // vmov
                        uint8_t dst_regs[4], src_regs[4];
                        float values[4];
                        vfpu_get_vector_regs(dst_regs, dim, vd);
                        vfpu_get_vector_regs(src_regs, dim, vs);
                        for (uint32_t i = 0; i < dim; i++) {
                            memcpy(&values[i], &cpu->vfpu[src_regs[i]], sizeof(values[i]));
                        }
                        for (uint32_t i = 0; i < dim; i++) {
                            memcpy(&cpu->vfpu[dst_regs[i]], &values[i], sizeof(values[i]));
                        }
                        break;
                    }
                    case 1: // vabs
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) cpu->vfpu[vd + i] = fabsf(cpu->vfpu[vs + i]);
                        }
                        break;
                    case 2: // vneg
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) cpu->vfpu[vd + i] = -cpu->vfpu[vs + i];
                        }
                        break;
                    case 6: // vzero
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128) cpu->vfpu[vd + i] = 0.0f;
                        }
                        break;
                    case 7: // vone
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128) cpu->vfpu[vd + i] = 1.0f;
                        }
                        break;
                    case 16: // vrcp (reciprocal)
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val != 0.0f) ? (1.0f / val) : 0.0f;
                            }
                        }
                        break;
                    case 17: // vrsq (reciprocal square root)
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val > 0.0f) ? (1.0f / sqrtf(val)) : 0.0f;
                            }
                        }
                        break;
                    case 18: // vsin
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                cpu->vfpu[vd + i] = sinf(cpu->vfpu[vs + i] * 1.57079632679f);
                            }
                        }
                        break;
                    case 19: // vcos
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                cpu->vfpu[vd + i] = cosf(cpu->vfpu[vs + i] * 1.57079632679f);
                            }
                        }
                        break;
                    case 22: // vsqrt
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val >= 0.0f) ? sqrtf(val) : 0.0f;
                            }
                        }
                        break;
                    case 23: // vasin (VFPU angles are scaled by 2/pi)
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                cpu->vfpu[vd + i] =
                                    asinf(cpu->vfpu[vs + i]) * 0.63661977236758134f;
                            }
                        }
                        break;
                    default:
                        printf("[Interpreter Warning] Unsupported COP2 monadic rt 0x%02X at PC 0x%08X\n", rt, pc);
                        break;
                }
            }
            break;
        }
                case 0x3F: { // VFLUSH
                    if ((inst & 0xFFFF0000u) != 0xFFFF0000u) {
                        cpu->vcr[0] = 0xE4;
                        cpu->vcr[1] = 0xE4;
                        cpu->vcr[2] = 0;
                    }
                    break;
                }
                case 0x1F: { // SPECIAL3
            switch (funct) {
                case 0x00: { // EXT rt, rs, pos, size
                    uint32_t pos = shamt;
                    uint32_t size = rd + 1;
                    uint32_t mask = (size < 32) ? ((1u << size) - 1) : 0xFFFFFFFFu;
                    if (rt != 0) cpu->gpr[rt] = (cpu->gpr[rs] >> pos) & mask;
                    break;
                }
                case 0x04: { // INS rt, rs, pos, size  (msb = rd, campo vai de pos ate msb)
                    uint32_t pos = shamt;
                    uint32_t msb = rd;
                    uint32_t size = msb - pos + 1;
                    uint32_t mask = (size < 32) ? ((1u << size) - 1) : 0xFFFFFFFFu;
                    if (rt != 0) {
                        uint32_t inserted = (cpu->gpr[rs] & mask) << pos;
                        cpu->gpr[rt] = (cpu->gpr[rt] & ~(mask << pos)) | inserted;
                    }
                    break;
                }
                case 0x20: { // BSHFL (sub-opcode selecionado por shamt)
                    switch (shamt) {
                        case 0x02: // WSBH - word swap bytes within halfwords
                            if (rd != 0) {
                                uint32_t v = cpu->gpr[rt];
                                cpu->gpr[rd] = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
                            }
                            break;
                        case 0x10: // SEB - sign-extend byte
                            if (rd != 0) cpu->gpr[rd] = (int32_t)(int8_t)(cpu->gpr[rt] & 0xFF);
                            break;
                        case 0x18: // SEH - sign-extend halfword
                            if (rd != 0) cpu->gpr[rd] = (int32_t)(int16_t)(cpu->gpr[rt] & 0xFFFF);
                            break;
                        case 0x14: { // BITREV - reverse all 32 bits
                            if (rd != 0) {
                                uint32_t value = cpu->gpr[rt];
                                value = ((value & 0x55555555u) << 1) |
                                        ((value >> 1) & 0x55555555u);
                                value = ((value & 0x33333333u) << 2) |
                                        ((value >> 2) & 0x33333333u);
                                value = ((value & 0x0F0F0F0Fu) << 4) |
                                        ((value >> 4) & 0x0F0F0F0Fu);
                                value = ((value & 0x00FF00FFu) << 8) |
                                        ((value >> 8) & 0x00FF00FFu);
                                cpu->gpr[rd] = (value << 16) | (value >> 16);
                            }
                            break;
                        }
                        default:
                            printf("[Interpreter Warning] Unsupported SPECIAL3 BSHFL shamt 0x%02X at PC 0x%08X\n", shamt, pc);
                            break;
                    }
                    break;
                }
                default:
                    printf("[Interpreter Warning] Unsupported SPECIAL3 funct 0x%02X (shamt=0x%02X) at PC 0x%08X\n", funct, shamt, pc);
                    break;
            }
            break;
        }
        default:
            printf("[Interpreter Warning] Unsupported opcode 0x%02X at PC "
                   "0x%08X (Inst: 0x%08X)\n", opcode, pc, inst);
            break;
    }

    // Process branch delay slot
    if (branch_taken) {
        cpu->next_pc = branch_target;
    }
    if (trace_nan_origin) {
        mips_trace_nan_registers(cpu, pc, inst, old_fpr, old_vfpu);
    }
}
