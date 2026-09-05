#include "mips_video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * OpenGL ES 3.20 (GLSL ES 3.20) Shader Sources [10.1][opengles32-quick-reference-card.pdf]
 */
static const char *vertex_shader_source = 
    "#version 320 es\n"
    "precision mediump float;\n"
    "layout(location = 0) in vec3 inPosition;\n"
    "layout(location = 1) in vec4 inColor;\n"
    "layout(location = 2) in vec2 inTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec2 fragTexCoord;\n"
    "uniform mat4 uMVP;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(inPosition, 1.0);\n"
    "    fragColor = inColor;\n"
    "    fragTexCoord = inTexCoord;\n"
    "}\n";

static const char *fragment_shader_source = 
    "#version 320 es\n"
    "precision mediump float;\n"
    "in vec4 fragColor;\n"
    "in vec2 fragTexCoord;\n"
    "out vec4 outColor;\n"
    "void main() {\n"
    "    outColor = fragColor;\n"
    "}\n";

/*
 * Pack/unroll structure for the GLES vertex attributes
 */
typedef struct {
    float x, y, z;
    float r, g, b, a;
    float u, v;
} GLES_Vertex;

// Helper to log OpenGL physical state warnings
static void check_gl_error(const char *op) {
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[Video GLES Warning] %s triggered GL Error: 0x%x\n", op, error);
    }
}

// Compile a shader stage with error logging
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint log_len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char *log = (char *)malloc(log_len);
        glGetShaderInfoLog(shader, log_len, NULL, log);
        fprintf(stderr, "[GLES Shader Error] Compile failed: %s\n", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int mips_video_init(MIPS_VideoSystem *vid) {
    if (vid == NULL) return -1;
    memset(vid, 0, sizeof(MIPS_VideoSystem));
    
    // Initialize SDL2 Video Subsystem
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[SDL Error] Failed to initialize video: %s\n", SDL_GetError());
        return -2;
    }
    
    // Request OpenGL ES 3.2 Context Profile [2.2][opengles32-quick-reference-card.pdf]
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    
    // Create high-definition upscaled window (960x544 is exactly 2x PSP native screen resolution)
    vid->window = SDL_CreateWindow(
        "PSS-AOT God of War Hardware-Accelerated GLES 3.2 Output",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 544,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );
    
    if (vid->window == NULL) {
        fprintf(stderr, "[SDL Error] Window creation failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -3;
    }
    
    // Create GLES Context
    vid->gl_context = SDL_GL_CreateContext(vid->window);
    if (vid->gl_context == NULL) {
        fprintf(stderr, "[SDL Error] GLES Context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -4;
    }
    
    // V-Sync control
    SDL_GL_SetSwapInterval(1);
    
    // Compile OpenGL ES 3.2 shader programs
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    
    vid->program = glCreateProgram();
    glAttachShader(vid->program, vs);
    glAttachShader(vid->program, fs);
    
    // Explicit attribute bindings prior to linking for strict ES driver compatibility
    glBindAttribLocation(vid->program, 0, "inPosition");
    glBindAttribLocation(vid->program, 1, "inColor");
    glBindAttribLocation(vid->program, 2, "inTexCoord");
    
    glLinkProgram(vid->program);
    
    GLint linked;
    glGetProgramiv(vid->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint log_len;
        glGetProgramiv(vid->program, GL_INFO_LOG_LENGTH, &log_len);
        char *log = (char *)malloc(log_len);
        glGetProgramInfoLog(vid->program, log_len, NULL, log);
        fprintf(stderr, "[GLES Program Error] Linking failed: %s\n", log);
        free(log);
        glDeleteProgram(vid->program);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        return -5;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Retrieve uniform locations
    vid->u_mvp_location = glGetUniformLocation(vid->program, "uMVP");
    vid->u_tint_location = glGetUniformLocation(vid->program, "uTint");
    
    // Generate vertex buffer structures [6][10.4][opengles32-quick-reference-card.pdf]
    glGenVertexArrays(1, &vid->vao);
    glGenBuffers(1, &vid->vbo);
    
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    
    // Vertex positions (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)0);
    
    // Vertex colors (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)(3 * sizeof(float)));
    
    // Vertex UV coordinates (location = 2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)(7 * sizeof(float)));
    
    glBindVertexArray(0);
    
    // Viewport and physical state initialization
    glViewport(0, 0, 960, 544);
    
    // Test direct errors
    check_gl_error("Init");
    
    vid->initialized = true;
    printf("[Video GLES] OpenGL ES 3.20 contexts successfully initialized on RK3326 Mali GPU.\n");
    return 0;
}

void mips_video_free(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    
    glDeleteBuffers(1, &vid->vbo);
    glDeleteVertexArrays(1, &vid->vao);
    glDeleteProgram(vid->program);
    
    SDL_GL_DeleteContext(vid->gl_context);
    SDL_DestroyWindow(vid->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    vid->initialized = false;
    printf("[Video GLES] Renderer shutdown complete.\n");
}

void mips_video_draw_prim(MIPS_VideoSystem *vid, MIPS_CPU *cpu, MIPS_GraphicsEngine *ge, int prim_type, int vertex_count) {
    if (vid == NULL || !vid->initialized || ge == NULL || vertex_count <= 0) return;
    
    // Decode stride from vtype
    int stride = 0;
    int weight_size = 0;
    int tex_size = 0;
    int col_size = 0;
    int pos_size = 0;
    
    if (ge->vtype.weight_fmt != 0) {
        weight_size = (ge->vtype.weight_fmt == 3) ? 4 : (ge->vtype.weight_fmt == 2) ? 2 : 1;
        stride += weight_size * ge->vtype.weight_count;
    }
    if (ge->vtype.tex_fmt != 0) {
        tex_size = (ge->vtype.tex_fmt == 3) ? 4 : (ge->vtype.tex_fmt == 2) ? 2 : 1;
        stride += tex_size * 2;
    }
    if (ge->vtype.col_fmt != 0) {
        col_size = (ge->vtype.col_fmt == 7) ? 4 : 2;
        stride += col_size;
    }
    if (ge->vtype.pos_fmt != 0) {
        pos_size = (ge->vtype.pos_fmt == 3) ? 4 : (ge->vtype.pos_fmt == 2) ? 2 : 1;
        stride += pos_size * 3;
    }
    
    if (stride == 0) return;
    
    // Allocate pack buffer for unrolling/translating vertices
    GLES_Vertex *vertices = (GLES_Vertex *)malloc(sizeof(GLES_Vertex) * vertex_count);
    if (vertices == NULL) return;
    
    // Unpack MIPS memory into normalized GLES Vertices
    for (int v = 0; v < vertex_count; v++) {
        uint32_t curr_vaddr = ge->vaddr + (v * stride);
        uint8_t *v_ptr = (uint8_t *)mips_get_ptr(cpu, curr_vaddr);
        
        if (v_ptr == NULL) {
            free(vertices);
            return;
        }
        
        int offset = 0;
        
        // 1. Unpack Weights (If skinning is active)
        if (ge->vtype.weight_fmt != 0) {
            offset += weight_size * ge->vtype.weight_count;
        }
        
        // 2. Unpack Texture UV coordinates
        float u = 0.0f, v_coord = 0.0f;
        uint16_t su = 0, sv = 0; // Initialize to secure compiler
        if (ge->vtype.tex_fmt != 0) {
            if (ge->vtype.tex_fmt == 3) { // Float
                memcpy(&u, v_ptr + offset, 4);
                memcpy(&v_coord, v_ptr + offset + 4, 4);
            } else if (ge->vtype.tex_fmt == 2) { // 16-bit short
                memcpy(&su, v_ptr + offset, 2);
                memcpy(&sv, v_ptr + offset + 2, 2);
                u = (float)su / 32767.0f;
                v_coord = (float)sv / 32767.0f;
            } else { // 8-bit byte
                u = (float)v_ptr[offset] / 255.0f;
                v_coord = (float)v_ptr[offset + 1] / 255.0f;
            }
            offset += tex_size * 2;
        }
        vertices[v].u = u;
        vertices[v].v = v_coord;
        
        // 3. Unpack Vertex colors
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        if (ge->vtype.col_fmt != 0) {
            if (ge->vtype.col_fmt == 7) { // RGBA8888 (32-bit)
                r = (float)v_ptr[offset] / 255.0f;
                g = (float)v_ptr[offset + 1] / 255.0f;
                b = (float)v_ptr[offset + 2] / 255.0f;
                a = (float)v_ptr[offset + 3] / 255.0f;
            } else if (ge->vtype.col_fmt == 4) { // BGR-565 (16-bit)
                uint16_t c;
                memcpy(&c, v_ptr + offset, 2);
                r = (float)((c >> 11) & 0x1F) / 31.0f;
                g = (float)((c >> 5) & 0x3F) / 63.0f;
                b = (float)(c & 0x1F) / 31.0f;
                a = 1.0f;
            } else if (ge->vtype.col_fmt == 5) { // ABGR-1555 (16-bit)
                uint16_t c;
                memcpy(&c, v_ptr + offset, 2);
                r = (float)((c >> 10) & 0x1F) / 31.0f;
                g = (float)((c >> 5) & 0x3F) / 31.0f;
                b = (float)(c & 0x1F) / 31.0f;
                a = (c & 0x8000) ? 1.0f : 0.0f;
            }
            offset += col_size;
        }
        vertices[v].r = r;
        vertices[v].g = g;
        vertices[v].b = b;
        vertices[v].a = a;
        
        // 4. Unpack Positions
        float x = 0.0f, y = 0.0f, z = 0.0f;
        int16_t sx = 0, sy = 0, sz = 0; // Initialize to secure compiler
        if (ge->vtype.pos_fmt != 0) {
            if (ge->vtype.pos_fmt == 3) { // Float
                memcpy(&x, v_ptr + offset, 4);
                memcpy(&y, v_ptr + offset + 4, 4);
                memcpy(&z, v_ptr + offset + 8, 4);
            } else if (ge->vtype.pos_fmt == 2) { // 16-bit Short (fixed point layout)
                memcpy(&sx, v_ptr + offset, 2);
                memcpy(&sy, v_ptr + offset + 2, 2);
                memcpy(&sz, v_ptr + offset + 4, 2);
                x = (float)sx;
                y = (float)sy;
                z = (float)sz;
            } else { // 8-bit Byte
                int8_t bx = (int8_t)v_ptr[offset];
                int8_t by = (int8_t)v_ptr[offset + 1];
                int8_t bz = (int8_t)v_ptr[offset + 2];
                x = (float)bx;
                y = (float)by;
                z = (float)bz;
            }
        }
        
        // Transform mapping check
        if (ge->vtype.transform_bypass) {
            // Coordinate space is already 2D screen coordinate. Normalize to GLES NDC [-1, 1]
            vertices[v].x = (x / 240.0f) - 1.0f;
            vertices[v].y = 1.0f - (y / 136.0f);
            vertices[v].z = z;
        } else {
            vertices[v].x = x;
            vertices[v].y = y;
            vertices[v].z = z;
        }
    }
    
    // Activate GLES Shaders [7.3]
    glUseProgram(vid->program);
    
    // Bind vertex arrays [10.4]
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    
    // Dynamic Buffer upload [6.2][opengles32-quick-reference-card.pdf]
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLES_Vertex) * vertex_count, vertices, GL_DYNAMIC_DRAW);
    
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    glUniformMatrix4fv(vid->u_mvp_location, 1, GL_FALSE, mvp);
    
    // Map primitive types [10.5][opengles32-quick-reference-card.pdf]
    GLenum mode = GL_TRIANGLES;
    switch (prim_type & 7) {
        case 0: mode = GL_POINTS; break;
        case 1: mode = GL_LINES; break;
        case 2: mode = GL_LINE_STRIP; break;
        case 3: mode = GL_TRIANGLES; break;
        case 4: mode = GL_TRIANGLE_STRIP; break;
        case 5: mode = GL_TRIANGLE_FAN; break;
        case 6: // Sprites (Render as Triangle Strips)
            mode = GL_TRIANGLE_STRIP;
            break;
    }
    
    // Hardware accelerated Draw Call [10.5]
    glDrawArrays(mode, 0, vertex_count);
    
    // Clean state
    glBindVertexArray(0);
    glUseProgram(0);
    
    free(vertices);
    
    // Advance Vertex list pointer
    ge->vaddr += vertex_count * stride;
}

void mips_video_present(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    SDL_GL_SwapWindow(vid->window);
    
    // Flush command pipeline [2.3.3][opengles32-quick-reference-card.pdf]
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void mips_video_draw_test_triangle(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    
    // Clear screen to graphite
    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Hardcoded simple test vertices (Degradê Vermelho-Verde-Azul)
    GLES_Vertex test_vertices[3] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, // Vermelho
        { 0.5f, -0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f}, // Verde
        { 0.0f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f}  // Azul
    };
    
    glUseProgram(vid->program);
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(test_vertices), test_vertices, GL_DYNAMIC_DRAW);
    
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    glUniformMatrix4fv(vid->u_mvp_location, 1, GL_FALSE, mvp);
    
    glDrawArrays(GL_TRIANGLES, 0, 3);
    
    glBindVertexArray(0);
    glUseProgram(0);
    
    SDL_GL_SwapWindow(vid->window);
}
