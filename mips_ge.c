#include "mips_ge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mips_ge_init(MIPS_GraphicsEngine *ge) {
    if (ge == NULL) return;
    memset(ge, 0, sizeof(MIPS_GraphicsEngine));
    ge->base_addr = 0;
    ge->offset_addr = 0;
    ge->stack_ptr = 0;
    ge->running = false;
    ge->finished = false;
    ge->signal_raised = false;
    ge->draw_callback = NULL;
    ge->video_system = NULL;
}

void mips_ge_decode_vtype(MIPS_GraphicsEngine *ge, uint32_t vtype_val) {
    ge->regs[CMD_VTYPE] = vtype_val;

    // Position format (bits 0-1)
    ge->vtype.pos_fmt = vtype_val & 3;
    // Texture coordinate format (bits 2-3)
    ge->vtype.tex_fmt = (vtype_val >> 2) & 3;
    // Color format (bits 5-7)
    ge->vtype.col_fmt = (vtype_val >> 5) & 7;
    // Normal format (bits 8-9)
    ge->vtype.normal_fmt = (vtype_val >> 8) & 3;
    // Weight format (bits 10-11)
    ge->vtype.weight_fmt = (vtype_val >> 10) & 3;
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

static int get_color_size(int fmt) {
    switch (fmt) {
        case 4: return 2; // 5650 16-bit
        case 5: return 2; // 5551 16-bit
        case 6: return 2; // 4444 16-bit
        case 7: return 4; // 8888 32-bit
        default: return 0;
    }
}

void mips_ge_execute_command(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu, uint32_t cmd_word) {
    uint8_t cmd = (cmd_word >> 24) & 0xFF;
    uint32_t arg = cmd_word & 0x00FFFFFF;

    ge->regs[cmd] = arg;

    switch (cmd) {
        case CMD_NOP:
            break;

        case CMD_BASE:
            // bits 16-20 are used as the upper 4 bits for a 28-bit address base
            ge->base_addr = (arg << 8) & 0x0F000000;
            break;

        case CMD_OFFSET:
            ge->offset_addr = arg;
            break;

        case CMD_VTYPE:
            mips_ge_decode_vtype(ge, arg);
            break;

        case CMD_VADR:
            ge->vaddr = ((ge->base_addr & 0x0F000000) | (arg & 0x00FFFFFF));
            break;

        case CMD_IADR:
            ge->iaddr = ((ge->base_addr & 0x0F000000) | (arg & 0x00FFFFFF));
            break;

        case CMD_JUMP: {
            uint32_t target_pc = ((ge->base_addr & 0x0F000000) | (arg & 0x00FFFFFF)) + ge->offset_addr;
            ge->pc = target_pc;
            break;
        }

        case CMD_CALL: {
            uint32_t target_pc = ((ge->base_addr & 0x0F000000) | (arg & 0x00FFFFFF)) + ge->offset_addr;
            if (ge->stack_ptr < 64) {
                ge->stack[ge->stack_ptr++] = ge->pc; // Save return address (already incremented)
            }
            ge->pc = target_pc;
            break;
        }

        case CMD_RET:
            if (ge->stack_ptr > 0) {
                ge->pc = ge->stack[--ge->stack_ptr];
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

            // Calculate stride of a single vertex in bytes
            int stride = 0;

            // Weights
            if (ge->vtype.weight_fmt != 0) {
                stride += get_format_size(ge->vtype.weight_fmt) * ge->vtype.weight_count;
            }
            // Texture coordinates
            if (ge->vtype.tex_fmt != 0) {
                stride += get_format_size(ge->vtype.tex_fmt) * 2;
            }
            // Colors
            if (ge->vtype.col_fmt != 0) {
                stride += get_color_size(ge->vtype.col_fmt);
            }
            // Normals
            if (ge->vtype.normal_fmt != 0) {
                stride += get_format_size(ge->vtype.normal_fmt) * 3;
            }
            // Position
            if (ge->vtype.pos_fmt != 0) {
                stride += get_format_size(ge->vtype.pos_fmt) * 3;
            }

            const char* prim_names[] = {
                "Points", "Lines", "Line Strips", "Triangles", 
                "Triangle Strips", "Triangle Fans", "Sprites"
            };

            printf("[GE Render] Primitive Kick: %s, Vertices: %d, Base VADDR: 0x%08X, Stride: %d bytes, TransformBypass: %s\n",
                   prim_names[prim_type & 7], vertex_count, ge->vaddr, stride,
                   ge->vtype.transform_bypass ? "2D" : "3D");

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

    ge->pc = start_pc;
    ge->stall_pc = stall_pc;
    ge->running = true;
    ge->finished = false;
    ge->signal_raised = false;

    // Process loop
    while (ge->running) {
        if (ge->pc == ge->stall_pc) {
            printf("[GE Engine] Thread hit Stall Address: 0x%08X. Suspending list.\n", ge->pc);
            break;
        }

        uint32_t cmd_word = mips_read32(cpu, ge->pc);
        uint32_t current_cmd_pc = ge->pc;

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
