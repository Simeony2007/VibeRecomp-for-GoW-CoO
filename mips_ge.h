#ifndef MIPS_GE_H
#define MIPS_GE_H

#include "mips_cpu.h"
#include <stdint.h>
#include <stdbool.h>

#define GE_MAX_LISTS 16

/* 
 * Graphics Engine (GE) Command Register Constants 
 */
#define CMD_NOP         0x00
#define CMD_VADR        0x01
#define CMD_IADR        0x02
#define CMD_PRIM        0x04
#define CMD_BEZIER      0x05
#define CMD_SPLINE      0x06
#define CMD_BBOX        0x07
#define CMD_JUMP        0x08
#define CMD_BJUMP       0x09
#define CMD_CALL        0x0A
#define CMD_RET         0x0B
#define CMD_END         0x0C
#define CMD_SIGNAL      0x0E
#define CMD_FINISH      0x0F
#define CMD_BASE        0x10
#define CMD_VTYPE       0x12
#define CMD_OFFSET      0x13
#define CMD_ORIGIN      0x14
#define CMD_REGION1     0x15
#define CMD_REGION2     0x16

#define CMD_FBP         0x9C
#define CMD_FBW         0x9D
#define CMD_ZBP         0x9E
#define CMD_ZBW         0x9F

#define CMD_TBP0        0xA0
#define CMD_TBW0        0xA8
#define CMD_TSIZE0      0xB8
#define CMD_TMAP        0xC0
#define CMD_TMODE       0xC2
#define CMD_TPSM        0xC3
#define CMD_TFLUSH      0xCB
#define CMD_TSYNC       0xCC
#define CMD_PSM         0xD2
#define CMD_CLEAR       0xD3
#define CMD_SCISSOR1    0xD4
#define CMD_SCISSOR2    0xD5

/*
 * Decoded Vertex Type Structure 
 */
typedef struct {
    int pos_fmt;     // 0: none, 1: 8-bit, 2: 16-bit, 3: float (32-bit)
    int tex_fmt;     // 0: none, 1: 8-bit, 2: 16-bit, 3: float (32-bit)
    int col_fmt;     // 0: none, 4: 16-bit 5650, 5: 16-bit 5551, 6: 16-bit 4444, 7: 32-bit 8888
    int normal_fmt;  // 0: none, 1: 8-bit, 2: 16-bit, 3: float (32-bit)
    int weight_fmt;  // 0: none, 1: 8-bit, 2: 16-bit, 3: float (32-bit)
    int index_fmt;   // 0: none, 1: 8-bit, 2: 16-bit
    int weight_count;// 1-8
    int morph_count; // 1-8
    bool transform_bypass; // true: 2D coordinates, false: 3D coordinates
} GE_VertexType;

struct MIPS_GraphicsEngine;

// Callback for drawing primitives dynamically (e.g. OpenGL ES 3.2 mapping)
typedef void (*GEDrawCallback)(void *video_system, MIPS_CPU *cpu, struct MIPS_GraphicsEngine *ge, int prim_type, int vertex_count);

/*
 * Graphics Engine State Representation
 */
typedef struct MIPS_GraphicsEngine {
    uint32_t regs[256];     // 256 Command/register parameters
    uint32_t base_addr;     // Upper bits base offset for jumps/calls
    uint32_t offset_addr;   // Additional list offset
    uint32_t vaddr;         // Vertex buffer address register
    uint32_t iaddr;         // Index buffer address register

    // Call stack for list subroutines (CMD_CALL -> CMD_RET)
    uint32_t stack[64];
    int stack_ptr;

    // Display list control
    uint32_t pc;            // Current instruction command pointer in RAM
    uint32_t stall_pc;      // Stop list parsing if pc reaches stall pointer
    bool running;           // Is active
    bool finished;          // Render finished (CMD_FINISH)
    bool signal_raised;     // Signal interrupt reached (CMD_SIGNAL)

    GE_VertexType vtype;    // Decoded current vertex type parameters

    // Accelerated render binding
    void *video_system;
    GEDrawCallback draw_callback;
} MIPS_GraphicsEngine;

/*
 * Display List Handle structure managed by the OS (HLE)
 */
typedef struct {
    MIPS_GraphicsEngine ge;
    int id;                 // Display list ID
    bool active;
    uint32_t start_addr;
    uint32_t stall_addr;
} GE_DisplayList;

void mips_ge_init(MIPS_GraphicsEngine *ge);
void mips_ge_decode_vtype(MIPS_GraphicsEngine *ge, uint32_t vtype_val);
void mips_ge_execute_command(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu, uint32_t cmd_word);
void mips_ge_run_list(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu, uint32_t start_pc, uint32_t stall_pc);

#endif // MIPS_GE_H
