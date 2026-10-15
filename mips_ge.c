#include "mips_ge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mips_ge_init(MIPS_GraphicsEngine *ge) {
    if (ge == NULL) return;
    memset(ge, 0, sizeof(MIPS_GraphicsEngine));
    ge->base_addr = 0;
    ge->offset_addr = 0;
    ge->edram_translation = 0x400;
    ge->stack_ptr = 0;
    ge->running = false;
    ge->finished = false;
    ge->signal_raised = false;
    ge->draw_callback = NULL;
    ge->video_system = NULL;
}

void mips_ge_begin_list(MIPS_GraphicsEngine *ge) {
    if (ge == NULL) return;
    ge->offset_addr = 0;
    ge->bbox_result = false;
    ge->stack_ptr = 0;
}

uint32_t mips_ge_set_addr_translation(MIPS_GraphicsEngine *ge, uint32_t new_size) {
    if (ge == NULL) return 0x800001FEu;

    bool invalid_range = new_size != 0 && (new_size < 0x200 || new_size > 0x1000);
    bool not_power_of_two = (new_size & (new_size - 1)) != 0;
    if (invalid_range || not_power_of_two) return 0x800001FEu;

    uint32_t previous_size = ge->edram_translation;
    ge->edram_translation = new_size;
    return previous_size;
}

void mips_ge_decode_vtype(MIPS_GraphicsEngine *ge, uint32_t vtype_val) {
    ge->regs[CMD_VTYPE] = vtype_val;

    // PSP GE VTYPE: texture bits 0-1, color 2-4, normal 5-6, position 7-8.
    ge->vtype.tex_fmt = vtype_val & 3;
    ge->vtype.col_fmt = (vtype_val >> 2) & 7;
    ge->vtype.normal_fmt = (vtype_val >> 5) & 3;
    ge->vtype.pos_fmt = (vtype_val >> 7) & 3;
    // Weight bits 9-10; index bits 11-12.
    ge->vtype.weight_fmt = (vtype_val >> 9) & 3;
    // Index format (bits 11-12)
    ge->vtype.index_fmt = (vtype_val >> 11) & 3;

    // Number of weights (bits 14-16)
    ge->vtype.weight_count = ((vtype_val >> 14) & 7) + 1;
    // Number of morphing vertices (bits 18-20)
    ge->vtype.morph_count = ((vtype_val >> 18) & 7) + 1;
    // Transform bypass flag (bit 23)
    ge->vtype.transform_bypass = (vtype_val & (1 << 23)) != 0;
}

static int get_format_size(int fmt) {
    switch (fmt) {
        case 1: return 1; // 8-bit / byte
        case 2: return 2; // 16-bit / short
        case 3: return 4; // 32-bit / float (or word)
        default: return 0;
    }
}

static int get_format_alignment(int fmt) {
    return fmt == 3 ? 4 : fmt == 2 ? 2 : fmt == 1 ? 1 : 0;
}

static int get_color_size(int fmt) {
    switch (fmt) {
        case 4: return 2; // 5650 16-bit
        case 5: return 2; // 5551 16-bit
        case 6: return 2; // 4444 16-bit
        case 7: return 4; // 8888 32-bit
        default: return 0;
    }
}

static int get_color_alignment(int fmt) {
    return fmt == 7 ? 4 : (fmt >= 4 && fmt <= 6) ? 2 : 0;
}

static int align_vertex_offset(int offset, int alignment) {
    if (alignment <= 1) return offset;
    return (offset + alignment - 1) & ~(alignment - 1);
}

static uint32_t ge_relative_address(const MIPS_GraphicsEngine *ge, uint32_t data) {
    uint32_t base_extended = (ge->base_addr & 0x0F000000) |
                             (data & 0x00FFFFFF);
    return (ge->offset_addr + base_extended) & 0x0FFFFFFF;
}

static bool ge_is_valid_command_address(MIPS_CPU *cpu, uint32_t address) {
    return (address & 3) == 0 &&
           mips_get_ptr_range(cpu, address, sizeof(uint32_t)) != NULL;
}

static bool ge_should_trace_command(uint32_t pc) {
    static bool initialized;
    static bool range_valid;
    static uint32_t trace_start;
    static uint32_t trace_end;
    static unsigned int trace_count;
    if (!initialized) {
        initialized = true;
        const char *range = getenv("MIPS_GE_TRACE_COMMAND_RANGE");
        if (range != NULL && *range != '\0') {
            char *separator = NULL;
            char *endptr = NULL;
            unsigned long start = strtoul(range, &separator, 0);
            unsigned long end = separator != NULL && *separator == '-' ?
                strtoul(separator + 1, &endptr, 0) : 0;
            if (separator == NULL || *separator != '-' || endptr == NULL ||
                *endptr != '\0' || start > UINT32_MAX ||
                end > UINT32_MAX || start >= end ||
                (start & 3) != 0 || (end & 3) != 0) {
                fprintf(stderr,
                        "[GE Trace Error] Invalid MIPS_GE_TRACE_COMMAND_RANGE "
                        "'%s'; expected aligned START-END addresses.\n",
                        range);
            } else {
                trace_start = (uint32_t)start;
                trace_end = (uint32_t)end;
                range_valid = true;
            }
        }
    }
    if (!range_valid || pc < trace_start || pc >= trace_end ||
        trace_count >= 256) {
        return false;
    }
    trace_count++;
    return true;
}

static float ge_decode_float24(uint32_t value);

static void ge_transform_point_4x3(float out[3], const float in[3],
                                   const float matrix[12]) {
    out[0] = in[0] * matrix[0] + in[1] * matrix[3] +
             in[2] * matrix[6] + matrix[9];
    out[1] = in[0] * matrix[1] + in[1] * matrix[4] +
             in[2] * matrix[7] + matrix[10];
    out[2] = in[0] * matrix[2] + in[1] * matrix[5] +
             in[2] * matrix[8] + matrix[11];
}

static void ge_transform_point_4x4(float out[4], const float in[3],
                                   const float matrix[16]) {
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] +
             in[2] * matrix[8] + matrix[12];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] +
             in[2] * matrix[9] + matrix[13];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] +
             in[2] * matrix[10] + matrix[14];
    out[3] = in[0] * matrix[3] + in[1] * matrix[7] +
             in[2] * matrix[11] + matrix[15];
}

static bool ge_bbox_vertex_visible(MIPS_GraphicsEngine *ge,
                                  const float position[3],
                                  int inside_count[6]) {
    float clip[4];
    float w;

    if (ge->vtype.transform_bypass) {
        clip[0] = position[0];
        clip[1] = position[1];
        clip[2] = position[2];
        clip[3] = 1.0f;
        w = 1.0f;
    } else {
        float world[3];
        float view[3];
        ge_transform_point_4x3(world, position, ge->world_matrix);
        ge_transform_point_4x3(view, world, ge->view_matrix);
        ge_transform_point_4x4(clip, view, ge->proj_matrix);
        for (int i = 0; i < 4; i++) {
            if (!isfinite(clip[i])) return false;
        }
        if (fabsf(clip[3]) < 1.0e-7f) return false;
        w = clip[3];
        if (clip[2] >= -w) inside_count[4]++;
        if (clip[2] <= w) inside_count[5]++;
    }

    float draw_x;
    float draw_y;
    if (ge->vtype.transform_bypass) {
        draw_x = clip[0];
        draw_y = clip[1];
    } else {
        draw_x = clip[0] * ge_decode_float24(ge->regs[CMD_VIEWPORTXSCALE]) +
                 ge_decode_float24(ge->regs[CMD_VIEWPORTXCENTER]) * w;
        draw_y = clip[1] * ge_decode_float24(ge->regs[CMD_VIEWPORTYSCALE]) +
                 ge_decode_float24(ge->regs[CMD_VIEWPORTYCENTER]) * w;
    }

    int region1_x = ge->regs[CMD_REGION1] & 0x3FF;
    int region1_y = (ge->regs[CMD_REGION1] >> 10) & 0x3FF;
    int region2_x = ge->regs[CMD_REGION2] & 0x3FF;
    int region2_y = (ge->regs[CMD_REGION2] >> 10) & 0x3FF;
    int scissor1_x = ge->regs[CMD_SCISSOR1] & 0x3FF;
    int scissor1_y = (ge->regs[CMD_SCISSOR1] >> 10) & 0x3FF;
    int scissor2_x = ge->regs[CMD_SCISSOR2] & 0x3FF;
    int scissor2_y = (ge->regs[CMD_SCISSOR2] >> 10) & 0x3FF;
    float offset_x = (float)(ge->regs[CMD_OFFSETX] & 0xFFFF) / 16.0f;
    float offset_y = (float)(ge->regs[CMD_OFFSETY] & 0xFFFF) / 16.0f;
    float left = offset_x + (region1_x > scissor1_x ?
                             region1_x : scissor1_x) - 1.0f;
    float top = offset_y + (region1_y > scissor1_y ?
                            region1_y : scissor1_y) - 1.0f;
    float right = offset_x + (region2_x < scissor2_x ?
                              region2_x : scissor2_x) + 1.0f;
    float bottom = offset_y + (region2_y < scissor2_y ?
                               region2_y : scissor2_y) + 1.0f;
    if (right >= 4096.0f || bottom >= 4096.0f ||
        left < 1.0f || top < 1.0f) {
        return true;
    }

    if (draw_x >= left * w) inside_count[0]++;
    if (draw_x <= right * w) inside_count[1]++;
    if (draw_y >= top * w) inside_count[2]++;
    if (draw_y <= bottom * w) inside_count[3]++;
    return true;
}

static void ge_bbox_advance_vertices(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu,
                                     uint32_t vertex_count,
                                     const GE_VertexLayout *layout) {
    bool indexed = ge->vtype.index_fmt != 0;
    uint32_t index_size = ge->vtype.index_fmt == 1 ? 1u :
                          ge->vtype.index_fmt == 2 ? 2u : 0u;
    if (layout->stride <= 0 || (indexed && index_size == 0)) return;
    uint64_t consumed = (uint64_t)vertex_count *
        (indexed ? index_size : (uint32_t)layout->stride);
    uint32_t address = indexed ? ge->iaddr : ge->vaddr;
    if (consumed > UINT32_MAX || (uint64_t)address + consumed > UINT32_MAX ||
        mips_get_ptr_range(cpu, address, (size_t)consumed) == NULL) {
        return;
    }
    if (indexed) {
        ge->iaddr += (uint32_t)consumed;
    } else {
        ge->vaddr += (uint32_t)consumed;
    }
}

static bool ge_test_bounding_box(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu,
                                 uint32_t vertex_count) {
    if (vertex_count == 0) return false;

    GE_VertexLayout layout;
    mips_ge_get_vertex_layout(ge, &layout);
    if (layout.stride <= 0) return true;
    uint32_t tested_count = vertex_count;
    uint32_t first_vertex = 0;
    if (vertex_count > 512) {
        tested_count = 256;
        first_vertex = vertex_count - 512;
    } else if (vertex_count > 256) {
        tested_count = vertex_count - 256;
    }

    if (ge->vtype.pos_fmt == 0 || ge->vtype.weight_fmt != 0 ||
        ge->vtype.morph_count > 1) {
        ge_bbox_advance_vertices(ge, cpu, vertex_count, &layout);
        return true;
    }

    const bool indexed = ge->vtype.index_fmt != 0;
    const uint32_t index_size = ge->vtype.index_fmt == 1 ? 1u :
                                ge->vtype.index_fmt == 2 ? 2u : 0u;
    if (indexed && index_size == 0) return true;

    uint64_t consumed = (uint64_t)vertex_count *
                        (indexed ? index_size : (uint32_t)layout.stride);
    if (consumed > UINT32_MAX ||
        (!indexed && (uint64_t)ge->vaddr + consumed > UINT32_MAX) ||
        (indexed && (uint64_t)ge->iaddr + consumed > UINT32_MAX)) {
        return true;
    }
    if (indexed &&
        mips_get_ptr_range(cpu, ge->iaddr, (size_t)consumed) == NULL) {
        return true;
    }

    int inside_count[6] = {0};
    bool can_classify = true;
    for (uint32_t i = 0; i < tested_count; i++) {
        uint32_t vertex_index = first_vertex + i;
        if (indexed) {
            const uint8_t *index_data =
                mips_get_ptr_range(cpu, ge->iaddr + i * index_size,
                                   index_size);
            if (index_data == NULL) return true;
            if (index_size == 1) {
                vertex_index = index_data[0];
            } else {
                uint16_t index;
                memcpy(&index, index_data, sizeof(index));
                vertex_index = index;
            }
        }

        uint64_t vertex_address = (uint64_t)ge->vaddr +
                                  (uint64_t)vertex_index * layout.stride;
        if (vertex_address > UINT32_MAX) return true;
        const uint8_t *vertex = mips_get_ptr_range(
            cpu, (uint32_t)vertex_address, (size_t)layout.stride);
        if (vertex == NULL) return true;

        float position[3];
        const uint8_t *pos = vertex + layout.position_offset;
        switch (ge->vtype.pos_fmt) {
            case 1:
                for (int axis = 0; axis < 3; axis++) {
                    position[axis] = (float)(int8_t)pos[axis] *
                        (ge->vtype.transform_bypass ? 1.0f : (1.0f / 128.0f));
                }
                break;
            case 2:
                for (int axis = 0; axis < 3; axis++) {
                    int16_t component;
                    memcpy(&component, pos + axis * sizeof(component),
                           sizeof(component));
                    position[axis] = (float)component *
                        (ge->vtype.transform_bypass ? 1.0f :
                         (1.0f / 32768.0f));
                }
                break;
            case 3:
                memcpy(position, pos, sizeof(position));
                break;
            default:
                return true;
        }
        if (!ge_bbox_vertex_visible(ge, position, inside_count)) {
            can_classify = false;
            break;
        }
    }

    bool visible = can_classify;
    int planes = (ge->regs[CMD_DEPTHCLIPENABLE] & 1) ? 6 : 4;
    if (can_classify) {
        for (int i = 0; i < planes; i++) {
            if (inside_count[i] == 0) visible = false;
        }
    }

    if (indexed) {
        ge->iaddr += (uint32_t)consumed;
    } else {
        ge->vaddr += (uint32_t)consumed;
    }
    return visible;
}

static float ge_decode_float24(uint32_t value) {
    uint32_t bits = value << 8;
    float decoded;
    memcpy(&decoded, &bits, sizeof(decoded));
    return decoded;
}

void mips_ge_get_vertex_layout(const MIPS_GraphicsEngine *ge, GE_VertexLayout *layout) {
    if (layout == NULL) return;
    memset(layout, 0, sizeof(*layout));
    if (ge == NULL) return;

    int stride = 0;
    int biggest_alignment = 1;
    int alignment = get_format_alignment(ge->vtype.weight_fmt);
    layout->weight_offset = stride;
    stride += get_format_size(ge->vtype.weight_fmt) * ge->vtype.weight_count;
    if (alignment > biggest_alignment) biggest_alignment = alignment;

    alignment = get_format_alignment(ge->vtype.tex_fmt);
    stride = align_vertex_offset(stride, alignment);
    layout->tex_offset = stride;
    stride += get_format_size(ge->vtype.tex_fmt) * 2;
    if (alignment > biggest_alignment) biggest_alignment = alignment;

    alignment = get_color_alignment(ge->vtype.col_fmt);
    stride = align_vertex_offset(stride, alignment);
    layout->color_offset = stride;
    stride += get_color_size(ge->vtype.col_fmt);
    if (alignment > biggest_alignment) biggest_alignment = alignment;

    alignment = get_format_alignment(ge->vtype.normal_fmt);
    stride = align_vertex_offset(stride, alignment);
    layout->normal_offset = stride;
    stride += get_format_size(ge->vtype.normal_fmt) * 3;
    if (alignment > biggest_alignment) biggest_alignment = alignment;

    alignment = get_format_alignment(ge->vtype.pos_fmt);
    stride = align_vertex_offset(stride, alignment);
    layout->position_offset = stride;
    stride += ge->vtype.pos_fmt == 0 ? 3 :
              get_format_size(ge->vtype.pos_fmt) * 3;
    if (alignment > biggest_alignment) biggest_alignment = alignment;

    layout->stride = align_vertex_offset(stride, biggest_alignment) *
                     ge->vtype.morph_count;
}

int mips_ge_vertex_stride(const MIPS_GraphicsEngine *ge) {
    GE_VertexLayout layout;
    mips_ge_get_vertex_layout(ge, &layout);
    return layout.stride;
}

void mips_ge_execute_command(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu, uint32_t cmd_word) {
    uint8_t cmd = (cmd_word >> 24) & 0xFF;
    uint32_t arg = cmd_word & 0x00FFFFFF;
    static int matrix_trace_enabled = -1;
    static unsigned int matrix_trace_count = 0;
    if (matrix_trace_enabled < 0) {
        const char *trace_setting = getenv("MIPS_GE_TRACE_MATRIX");
        matrix_trace_enabled = trace_setting != NULL &&
                               strcmp(trace_setting, "1") == 0;
    }

    const char *matrix_name = NULL;
    uint32_t matrix_index = 0;
    switch (cmd) {
        case CMD_WORLDMATRIXNUMBER:
        case CMD_WORLDMATRIXDATA:
            matrix_name = "world";
            matrix_index = ge->world_matrix_index;
            break;
        case CMD_VIEWMATRIXNUMBER:
        case CMD_VIEWMATRIXDATA:
            matrix_name = "view";
            matrix_index = ge->view_matrix_index;
            break;
        case CMD_PROJMATRIXNUMBER:
        case CMD_PROJMATRIXDATA:
            matrix_name = "projection";
            matrix_index = ge->proj_matrix_index;
            break;
        default:
            break;
    }
    if (matrix_trace_enabled && matrix_name != NULL &&
        matrix_trace_count < 512) {
        matrix_trace_count++;
        if (cmd == CMD_WORLDMATRIXNUMBER ||
            cmd == CMD_VIEWMATRIXNUMBER ||
            cmd == CMD_PROJMATRIXNUMBER) {
            fprintf(stderr,
                    "[GE Matrix Trace] pc=0x%08X matrix=%s op=number "
                    "raw=0x%06X index=%u\n",
                    ge->pc - 4, matrix_name, arg, arg & 0x0F);
        } else {
            fprintf(stderr,
                    "[GE Matrix Trace] pc=0x%08X matrix=%s op=data "
                    "raw=0x%06X index=%u value=%.8g\n",
                    ge->pc - 4, matrix_name, arg, matrix_index,
                    ge_decode_float24(arg));
        }
    }

    ge->regs[cmd] = arg;
    if (cmd >= CMD_TSIZE0 && cmd <= CMD_TSIZE7) {
        ge->texture_size_pc[cmd - CMD_TSIZE0] = ge->pc >= 4
            ? ge->pc - 4 : 0;
    }

    switch (cmd) {
        case CMD_NOP:
            break;

        case CMD_BBOX: {
            uint32_t vertex_count = arg & 0xFFFF;
            uint32_t vertex_address = ge->vaddr;
            uint32_t index_address = ge->iaddr;
            ge->bbox_result = ge_test_bounding_box(ge, cpu, vertex_count);
            static unsigned int bbox_log_count = 0;
            bbox_log_count++;
            if (bbox_log_count <= 64 || (bbox_log_count % 4096) == 0) {
                printf("[GE CTRL] BBOX list_pc=0x%08X count=%u result=%s "
                       "vaddr=0x%08X->0x%08X iaddr=0x%08X->0x%08X "
                       "vtype=0x%06X\n",
                       ge->pc - 4, vertex_count,
                       ge->bbox_result ? "visible" : "culled",
                       vertex_address, ge->vaddr, index_address, ge->iaddr,
                       ge->regs[CMD_VTYPE]);
            }
            break;
        }

        case CMD_BASE:
            // bits 16-20 are used as the upper 4 bits for a 28-bit address base
            ge->base_addr = (arg << 8) & 0x0F000000;
            break;

        case CMD_OFFSET:
            ge->offset_addr = arg << 8;
            break;

        case CMD_ORIGIN:
            ge->offset_addr = (ge->pc - 4) & 0x0FFFFFFF;
            {
                static unsigned int origin_log_count = 0;
                origin_log_count++;
                if (origin_log_count <= 64 ||
                    (origin_log_count % 4096) == 0) {
                    printf("[GE CTRL] ORIGIN list_pc=0x%08X offset=0x%08X\n",
                           ge->pc - 4, ge->offset_addr);
                }
            }
            break;

        case CMD_WORLDMATRIXNUMBER:
            ge->world_matrix_index = arg & 0x0F;
            break;

        case CMD_WORLDMATRIXDATA:
            if (ge->world_matrix_index < 12) {
                ge->world_matrix[ge->world_matrix_index] =
                    ge_decode_float24(arg);
            }
            ge->world_matrix_index++;
            break;

        case CMD_VIEWMATRIXNUMBER:
            ge->view_matrix_index = arg & 0x0F;
            break;

        case CMD_VIEWMATRIXDATA:
            if (ge->view_matrix_index < 12) {
                ge->view_matrix[ge->view_matrix_index] =
                    ge_decode_float24(arg);
            }
            ge->view_matrix_index++;
            break;

        case CMD_PROJMATRIXNUMBER:
            ge->proj_matrix_index = arg & 0x0F;
            break;

        case CMD_PROJMATRIXDATA:
            if (ge->proj_matrix_index < 16) {
                ge->proj_matrix[ge->proj_matrix_index] =
                    ge_decode_float24(arg);
            }
            ge->proj_matrix_index++;
            break;

        case CMD_VTYPE:
            mips_ge_decode_vtype(ge, arg);
            {
                static unsigned int vtype_log_count = 0;
                vtype_log_count++;
                if (vtype_log_count <= 64 || (vtype_log_count % 4096) == 0) {
                    printf("[GE VTYPE] list_pc=0x%08X raw=0x%06X "
                           "tex=%d color=%d normal=%d pos=%d weight=%d "
                           "index=%d weights=%d morph=%d through=%d stride=%d\n",
                           ge->pc - 4, arg, ge->vtype.tex_fmt, ge->vtype.col_fmt,
                           ge->vtype.normal_fmt, ge->vtype.pos_fmt,
                           ge->vtype.weight_fmt, ge->vtype.index_fmt,
                           ge->vtype.weight_count, ge->vtype.morph_count,
                           ge->vtype.transform_bypass ? 1 : 0,
                           mips_ge_vertex_stride(ge));
                }
            }
            break;

        case CMD_LOADCLUT: {
            uint32_t blocks = arg & 0x3Fu;
            if ((arg & 0x7Fu) == 0x40u) blocks = 0x40u;
            uint32_t load_bytes = blocks * 32u;
            if (load_bytes == 0) break;

            uint32_t clut_address =
                (ge->regs[CMD_CLUTADDR] & 0x00FFFFF0u) |
                ((ge->regs[CMD_CLUTADDRUPPER] << 8) & 0x0F000000u);
            const void *source = cpu != NULL
                ? mips_get_ptr_range(cpu, clut_address, load_bytes) : NULL;
            if (source != NULL) {
                memcpy(ge->clut, source, load_bytes);
            } else {
                memset(ge->clut, 0, load_bytes);
                fprintf(stderr,
                        "[GE CLUT ERROR] LOADCLUT address=0x%08X "
                        "bytes=%u is outside emulated memory; using zeros.\n",
                        clut_address, load_bytes);
            }
            ge->clut_loaded_bytes = load_bytes;
            ge->clut_loaded = true;

            static unsigned int clut_load_log_count;
            const char *trace_setting = getenv("MIPS_GE_TRACE_TEXTURE_STATE");
            if (trace_setting != NULL && strcmp(trace_setting, "1") == 0 &&
                clut_load_log_count < 64) {
                clut_load_log_count++;
                fprintf(stderr,
                        "[GE CLUT TRACE] pc=%08X addr=%08X bytes=%u "
                        "format=%u source_valid=%u\n",
                        ge->pc >= 4 ? ge->pc - 4 : 0, clut_address,
                        load_bytes, ge->regs[CMD_CLUTFORMAT] & 3u,
                        source != NULL ? 1u : 0u);
            }
            break;
        }

        case CMD_VADR:
            ge->vaddr = ge_relative_address(ge, arg);
            break;

        case CMD_IADR:
            ge->iaddr = ge_relative_address(ge, arg);
            break;

        case CMD_JUMP: {
            uint32_t target_pc = ge_relative_address(ge, arg & 0x00FFFFFC);
            static unsigned int jump_log_count = 0;
            jump_log_count++;
            if (jump_log_count <= 64 || (jump_log_count % 4096) == 0) {
                printf("[GE CTRL] JUMP list_pc=0x%08X raw=0x%06X "
                       "base=0x%08X offset=0x%08X target=0x%08X\n",
                       ge->pc - 4, arg, ge->base_addr, ge->offset_addr, target_pc);
            }
            if (cpu != NULL && !ge_is_valid_command_address(cpu, target_pc)) {
                fprintf(stderr,
                        "[GE Error] JUMP target 0x%08X is unaligned or unmapped; "
                        "stopping display list.\n",
                        target_pc);
                ge->running = false;
                ge->finished = true;
                break;
            }
            ge->pc = target_pc;
            break;
        }

        case CMD_BJUMP: {
            if (ge->bbox_result) break;
            uint32_t target_pc = ge_relative_address(ge, arg & 0x00FFFFFC);
            static unsigned int bjump_log_count = 0;
            bjump_log_count++;
            if (bjump_log_count <= 64 || (bjump_log_count % 4096) == 0) {
                printf("[GE CTRL] BJUMP list_pc=0x%08X raw=0x%06X "
                       "base=0x%08X offset=0x%08X target=0x%08X\n",
                       ge->pc - 4, arg, ge->base_addr, ge->offset_addr,
                       target_pc);
            }
            if (cpu != NULL && !ge_is_valid_command_address(cpu, target_pc)) {
                fprintf(stderr,
                        "[GE Error] BJUMP target 0x%08X is unaligned or "
                        "unmapped; stopping display list.\n",
                        target_pc);
                ge->running = false;
                ge->finished = true;
                break;
            }
            ge->pc = target_pc;
            break;
        }

        case CMD_CALL: {
            uint32_t target_pc = ge_relative_address(ge, arg & 0x00FFFFFC);
            static unsigned int call_log_count = 0;
            call_log_count++;
            if (call_log_count <= 64 || (call_log_count % 4096) == 0) {
                printf("[GE CTRL] CALL list_pc=0x%08X raw=0x%06X "
                       "base=0x%08X offset=0x%08X target=0x%08X return=0x%08X\n",
                       ge->pc - 4, arg, ge->base_addr, ge->offset_addr,
                       target_pc, ge->pc);
            }
            if (cpu != NULL && !ge_is_valid_command_address(cpu, target_pc)) {
                fprintf(stderr,
                        "[GE Error] CALL target 0x%08X is unaligned or unmapped; "
                        "stopping display list.\n",
                        target_pc);
                ge->running = false;
                ge->finished = true;
                break;
            }
            if (ge->stack_ptr < 0 || ge->stack_ptr >= 64) {
                fprintf(stderr,
                        "[GE Error] CALL stack overflow at list PC 0x%08X; "
                        "stopping display list.\n",
                        ge->pc - 4);
                ge->running = false;
                ge->finished = true;
                break;
            }
            ge->stack[ge->stack_ptr] = ge->pc;
            ge->stack_offset[ge->stack_ptr] = ge->offset_addr;
            ge->stack_ptr++;
            ge->pc = target_pc;
            break;
        }

        case CMD_RET:
            if (ge->stack_ptr > 0) {
                ge->stack_ptr--;
                ge->pc = ge->stack[ge->stack_ptr];
                ge->offset_addr = ge->stack_offset[ge->stack_ptr];
                {
                    static unsigned int ret_log_count = 0;
                    ret_log_count++;
                    if (ret_log_count <= 64 ||
                        (ret_log_count % 4096) == 0) {
                        printf("[GE CTRL] RET list_pc=0x%08X "
                               "target=0x%08X offset=0x%08X depth=%d\n",
                               ge->pc - 4, ge->pc, ge->offset_addr,
                               ge->stack_ptr);
                    }
                }
            } else {
                ge->running = false; // Empty stack return stops execution
            }
            break;

        case CMD_END:
            ge->running = false;
            break;

        case CMD_FINISH:
            ge->finished = true;
            ge->running = false;
            break;

        case CMD_SIGNAL:
            ge->signal_raised = true;
            break;

        case CMD_PRIM: {
            int prim_type = (arg >> 16) & 7;
            int vertex_count = arg & 0xFFFF;

            int stride = mips_ge_vertex_stride(ge);

            const char* prim_names[] = {
                "Points", "Lines", "Line Strips", "Triangles", 
                "Triangle Strips", "Triangle Fans", "Sprites"
            };

            static unsigned int prim_log_count = 0;
            prim_log_count++;
            if (prim_log_count <= 64 || (prim_log_count % 4096) == 0) {
                printf("[GE Render] list_pc=0x%08X raw=0x%06X "
                       "Primitive=%s Vertices=%d VADDR=0x%08X IADDR=0x%08X "
                       "Stride=%d VTYPE=0x%06X "
                       "TransformBypass=%s\n",
                       ge->pc - 4, arg, prim_names[prim_type & 7], vertex_count,
                       ge->vaddr, ge->iaddr, stride, ge->regs[CMD_VTYPE],
                       ge->vtype.transform_bypass ? "2D" : "3D");
            }

            // Auditing first 3 vertices
            for (int v = 0; vertex_count > 0 && v < 3 && v < vertex_count; v++) {
                uint32_t curr_vert_addr = ge->vaddr + (v * stride);
                if (ge->vtype.pos_fmt == 3) { // 32-bit floats
                    // Calculate position offset (it's at the end of the stride)
                    int pos_offset = stride - 12; // 3 floats = 12 bytes
                    float *x_ptr = (float*)mips_get_ptr(cpu, curr_vert_addr + pos_offset);
                    if (x_ptr) {
                        float x = x_ptr[0];
                        float y = x_ptr[1];
                        float z = x_ptr[2];
                        printf("  -> Vertex %d (Unpacked Float): X=%.2f, Y=%.2f, Z=%.2f\n", v, x, y, z);
                    }
                }
            }
            if (vertex_count > 3) {
                printf("  -> ... (remaining %d vertices processed)\n", vertex_count - 3);
            }

            // Route to dynamically-bound high performance OpenGL ES 3.2 renderer
            if (ge->draw_callback != NULL) {
                ge->draw_callback(ge->video_system, cpu, ge, prim_type, vertex_count);
            } else {
                // If no rendering context is bound, we still advance the vertex pointer
                ge->vaddr += vertex_count * stride;
            }
            break;
        }

        default:
            break;
    }
}

void mips_ge_run_list(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu, uint32_t start_pc, uint32_t stall_pc) {
    if (ge == NULL || cpu == NULL) return;

    if (!ge_is_valid_command_address(cpu, start_pc)) {
        static unsigned int invalid_start_count = 0;
        invalid_start_count++;
        if (invalid_start_count <= 32 || (invalid_start_count % 4096) == 0) {
            printf("[GE DEBUG] invalid start #%u list=0x%08X stall=0x%08X "
                   "cpu_pc=0x%08X ra=0x%08X t9=0x%08X sp=0x%08X "
                   "a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                   invalid_start_count, start_pc, stall_pc, cpu->pc,
                   cpu->gpr[31], cpu->gpr[25], cpu->gpr[29],
                   cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
            printf("[GE DEBUG] fpr={%08X,%08X,%08X,%08X} "
                   "vfpu={%08X,%08X,%08X,%08X}\n",
                   *(uint32_t *)&cpu->fpr[0], *(uint32_t *)&cpu->fpr[1],
                   *(uint32_t *)&cpu->fpr[2], *(uint32_t *)&cpu->fpr[3],
                   *(uint32_t *)&cpu->vfpu[0], *(uint32_t *)&cpu->vfpu[1],
                   *(uint32_t *)&cpu->vfpu[2], *(uint32_t *)&cpu->vfpu[3]);
        }
        printf("[GE Error] Invalid display-list start address: 0x%08X\n", start_pc);
        ge->running = false;
        ge->finished = true;
        return;
    }
    if (stall_pc != 0 && !ge_is_valid_command_address(cpu, stall_pc)) {
        printf("[GE Error] Invalid stall address: 0x%08X\n", stall_pc);
        stall_pc = 0;
    }

    ge->pc = start_pc;
    ge->stall_pc = stall_pc;
    ge->running = true;
    ge->finished = false;
    ge->signal_raised = false;

    // Process loop
    while (ge->running) {
        if (!ge_is_valid_command_address(cpu, ge->pc)) {
            fprintf(stderr,
                    "[GE Error] Display-list PC 0x%08X is unaligned or unmapped; "
                    "stopping display list.\n",
                    ge->pc);
            ge->running = false;
            ge->finished = true;
            break;
        }

        if (ge->pc == ge->stall_pc) {
            printf("[GE Engine] Thread hit Stall Address: 0x%08X. Suspending list.\n", ge->pc);
            break;
        }

        uint32_t cmd_word = mips_read32(cpu, ge->pc);
        uint32_t current_cmd_pc = ge->pc;
        if (ge_should_trace_command(current_cmd_pc)) {
            fprintf(stderr,
                    "[GE Trace Command] pc=0x%08X raw=0x%08X cmd=0x%02X "
                    "arg=0x%06X base=0x%08X offset=0x%08X "
                    "vaddr=0x%08X iaddr=0x%08X vtype=0x%06X\n",
                    current_cmd_pc, cmd_word, cmd_word >> 24,
                    cmd_word & 0x00FFFFFF, ge->base_addr, ge->offset_addr,
                    ge->vaddr, ge->iaddr, ge->regs[CMD_VTYPE]);
        }

        // Increment default program counter
        ge->pc += 4;

        mips_ge_execute_command(ge, cpu, cmd_word);

        // Safety break if PC jumps to 0
        if (ge->pc == 0 && ge->running) {
            printf("[GE Error] List jumped to 0x00000000 at list PC: 0x%08X. Stopping.\n", current_cmd_pc);
            ge->running = false;
            break;
        }
    }
}
