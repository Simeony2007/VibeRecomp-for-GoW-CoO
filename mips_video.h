#ifndef MIPS_VIDEO_H
#define MIPS_VIDEO_H

#include "mips_cpu.h"
#include "mips_ge.h"
#include <SDL2/SDL.h>
#include <GLES3/gl32.h>
#include <stdbool.h>

/*
 * MIPS Video Rendering System
 * Integrates SDL2 window management and OpenGL ES 3.2 context
 * for hardware-accelerated PSP Graphics Core emulation.
 */
typedef struct {
    SDL_Window *window;
    SDL_GLContext gl_context;
    
    // Shader program
    GLuint program;
    GLuint vao;
    GLuint vbo;
    
    // Uniform locations
    GLint u_mvp_location;
    GLint u_tint_location;
    
    // Virtual PSP Display buffering
    uint32_t fbp; // Frame buffer pointer
    uint32_t fbw; // Frame buffer width (stride)
    uint32_t psm; // Pixel Storage Mode
    
    bool initialized;
} MIPS_VideoSystem;

// Initialize the SDL2 + OpenGL ES 3.2 video subsystem
int mips_video_init(MIPS_VideoSystem *vid);

// Free video resources
void mips_video_free(MIPS_VideoSystem *vid);

// Process a primitive kick (CMD_PRIM) using GLES 3.2 pipeline
void mips_video_draw_prim(MIPS_VideoSystem *vid, MIPS_CPU *cpu, MIPS_GraphicsEngine *ge, int prim_type, int vertex_count);

// Swap buffers (present emulated frames)
void mips_video_present(MIPS_VideoSystem *vid);

// Isolated direct render diagnostic check to bypass emulated lists
void mips_video_draw_test_triangle(MIPS_VideoSystem *vid);

#endif // MIPS_VIDEO_H
