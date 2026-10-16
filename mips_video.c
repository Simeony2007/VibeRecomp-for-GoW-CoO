#include "mips_video.h"
#include "mips_video_pixel.h"
#include "mips_video_texture.h"
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * OpenGL ES 3.20 (GLSL ES 3.20) Shader Sources [10.1][opengles32-quick-reference-card.pdf]
 */
static const char *vertex_shader_source = 
    "#version 320 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec4 inPosition;\n"
    "layout(location = 1) in vec4 inColor;\n"
    "layout(location = 2) in vec2 inTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec2 fragTexCoord;\n"
    "uniform mat4 uMVP;\n"
    "void main() {\n"
    "    gl_Position = uMVP * inPosition;\n"
    "    fragColor = inColor;\n"
    "    fragTexCoord = inTexCoord;\n"
    "}\n";

static const char *fragment_shader_source = 
    "#version 320 es\n"
    "precision mediump float;\n"
    "in vec4 fragColor;\n"
    "in vec2 fragTexCoord;\n"
    "uniform sampler2D uFramebuffer;\n"
    "uniform sampler2D uTexture;\n"
    "uniform int uUseFramebuffer;\n"
    "uniform int uUseTexture;\n"
    "uniform int uTextureFunction;\n"
    "uniform vec4 uTextureEnvironment;\n"
    "uniform int uTextureHasAlpha;\n"
    "out vec4 outColor;\n"
    "void main() {\n"
    "    if (uUseFramebuffer != 0) {\n"
    "        outColor = texture(uFramebuffer, fragTexCoord);\n"
    "    } else if (uUseTexture != 0) {\n"
    "        vec4 texel = texture(uTexture, vec2(fragTexCoord.x, 1.0 - fragTexCoord.y));\n"
    "        if (uTextureHasAlpha == 0) texel.a = 1.0;\n"
    "        if (uTextureFunction == 1) {\n"
    "            outColor = vec4(mix(fragColor.rgb, texel.rgb, texel.a), fragColor.a);\n"
    "        } else if (uTextureFunction == 2) {\n"
    "            outColor = vec4(mix(fragColor.rgb, uTextureEnvironment.rgb, texel.rgb), fragColor.a * texel.a);\n"
    "        } else if (uTextureFunction == 3) {\n"
    "            outColor = vec4(texel.rgb, uTextureHasAlpha != 0 ? texel.a : fragColor.a);\n"
    "        } else if (uTextureFunction >= 4) {\n"
    "            outColor = vec4(fragColor.rgb + texel.rgb, fragColor.a * texel.a);\n"
    "        } else {\n"
    "            outColor = fragColor * texel;\n"
    "        }\n"
    "    } else {\n"
    "        outColor = fragColor;\n"
    "    }\n"
    "}\n";

/*
 * Pack/unroll structure for the GLES vertex attributes
 */
typedef struct {
    float x, y, z;
    float w;
    float r, g, b, a;
    float u, v;
} GLES_Vertex;

static GLES_Vertex interpolate_vertex(const GLES_Vertex *a,
                                     const GLES_Vertex *b) {
    return (GLES_Vertex) {
        (a->x + b->x) * 0.5f, (a->y + b->y) * 0.5f,
        (a->z + b->z) * 0.5f, (a->w + b->w) * 0.5f,
        (a->r + b->r) * 0.5f, (a->g + b->g) * 0.5f,
        (a->b + b->b) * 0.5f, (a->a + b->a) * 0.5f,
        (a->u + b->u) * 0.5f, (a->v + b->v) * 0.5f
    };
}

// Helper to log OpenGL physical state warnings
static void check_gl_error(const char *op) {
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[Video GLES Warning] %s triggered GL Error: 0x%x\n", op, error);
    }
}

static void log_texture_error(const char *format, ...) {
    static unsigned int error_count = 0;
    error_count++;
    if (error_count > 8 && (error_count % 1024) != 0) return;

    va_list args;
    va_start(args, format);
    fputs("[GE Renderer] ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
}

// Compile a shader stage with error logging
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        fprintf(stderr, "[GLES Shader Error] Could not allocate shader: 0x%x\n", glGetError());
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint log_len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char *log = (char *)calloc((size_t)(log_len > 0 ? log_len : 0) + 1, 1);
        if (log != NULL && log_len > 0) {
            glGetShaderInfoLog(shader, log_len, NULL, log);
        }
        fprintf(stderr, "[GLES Shader Error] Compile failed: %s\n",
                log != NULL ? log : "(log allocation failed)");
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
    const char *debug_ge_only = getenv("MIPS_VIDEO_DEBUG_GE_ONLY");
    vid->debug_ge_only = debug_ge_only != NULL &&
                         strcmp(debug_ge_only, "1") == 0;
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

    if (SDL_GL_MakeCurrent(vid->window, vid->gl_context) != 0) {
        fprintf(stderr, "[SDL Error] Could not activate GLES context: %s\n", SDL_GetError());
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        return -4;
    }
    
    // V-Sync control
    SDL_GL_SetSwapInterval(1);
    
    // Compile OpenGL ES 3.2 shader programs
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        return -5;
    }

    vid->program = glCreateProgram();
    if (vid->program == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        fprintf(stderr, "[GLES Program Error] Could not allocate shader program.\n");
        return -5;
    }
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
        char *log = (char *)calloc((size_t)log_len + 1, 1);
        if (log != NULL) glGetProgramInfoLog(vid->program, log_len, NULL, log);
        fprintf(stderr, "[GLES Program Error] Linking failed: %s\n",
                log != NULL ? log : "(log allocation failed)");
        free(log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(vid->program);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        return -5;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Retrieve uniform locations
    vid->u_mvp_location = glGetUniformLocation(vid->program, "uMVP");
    vid->u_tint_location = glGetUniformLocation(vid->program, "uTint");
    vid->u_framebuffer_location = glGetUniformLocation(vid->program, "uFramebuffer");
    vid->u_use_framebuffer_location = glGetUniformLocation(vid->program, "uUseFramebuffer");
    vid->u_texture_location = glGetUniformLocation(vid->program, "uTexture");
    vid->u_use_texture_location = glGetUniformLocation(vid->program, "uUseTexture");
    vid->u_texture_function_location =
        glGetUniformLocation(vid->program, "uTextureFunction");
    vid->u_texture_has_alpha_location =
        glGetUniformLocation(vid->program, "uTextureHasAlpha");
    vid->u_texture_environment_location =
        glGetUniformLocation(vid->program, "uTextureEnvironment");
    
    // Generate vertex buffer structures [6][10.4][opengles32-quick-reference-card.pdf]
    glGenVertexArrays(1, &vid->vao);
    glGenBuffers(1, &vid->vbo);
    glGenTextures(1, &vid->framebuffer_texture);
    glGenTextures(1, &vid->ge_texture);
    glGenTextures(1, &vid->render_texture);
    glGenFramebuffers(1, &vid->render_framebuffer);
    glGenRenderbuffers(1, &vid->render_depthbuffer);
    if (vid->vao == 0 || vid->vbo == 0 || vid->framebuffer_texture == 0 ||
        vid->ge_texture == 0 || vid->render_texture == 0 ||
        vid->render_framebuffer == 0 || vid->render_depthbuffer == 0) {
        fprintf(stderr, "[GLES Error] Could not allocate video buffers or framebuffer texture.\n");
        if (vid->render_depthbuffer != 0) {
            glDeleteRenderbuffers(1, &vid->render_depthbuffer);
        }
        if (vid->render_framebuffer != 0) {
            glDeleteFramebuffers(1, &vid->render_framebuffer);
        }
        if (vid->render_texture != 0) glDeleteTextures(1, &vid->render_texture);
        if (vid->ge_texture != 0) glDeleteTextures(1, &vid->ge_texture);
        if (vid->framebuffer_texture != 0) glDeleteTextures(1, &vid->framebuffer_texture);
        if (vid->vbo != 0) glDeleteBuffers(1, &vid->vbo);
        if (vid->vao != 0) glDeleteVertexArrays(1, &vid->vao);
        glDeleteProgram(vid->program);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        return -6;
    }

    glBindTexture(GL_TEXTURE_2D, vid->render_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 960, 544, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, vid->render_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, vid->render_texture, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, vid->render_depthbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 960, 544);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, vid->render_depthbuffer);
    GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "[GLES Error] Persistent render target is incomplete "
                "(status=0x%04X).\n",
                framebuffer_status);
        glDeleteRenderbuffers(1, &vid->render_depthbuffer);
        glDeleteFramebuffers(1, &vid->render_framebuffer);
        glDeleteTextures(1, &vid->render_texture);
        glDeleteTextures(1, &vid->ge_texture);
        glDeleteTextures(1, &vid->framebuffer_texture);
        glDeleteBuffers(1, &vid->vbo);
        glDeleteVertexArrays(1, &vid->vao);
        glDeleteProgram(vid->program);
        SDL_GL_DeleteContext(vid->gl_context);
        SDL_DestroyWindow(vid->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        memset(vid, 0, sizeof(*vid));
        return -6;
    }
    
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    
    // Vertex positions (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)0);
    
    // Vertex colors (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)(4 * sizeof(float)));
    
    // Vertex UV coordinates (location = 2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLES_Vertex), (void*)(8 * sizeof(float)));
    
    glBindVertexArray(0);
    
    // Viewport and physical state initialization
    glBindFramebuffer(GL_FRAMEBUFFER, vid->render_framebuffer);
    glViewport(0, 0, 960, 544);
    
    // Test direct errors
    check_gl_error("Init");

    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    vid->initialized = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    SDL_GL_SwapWindow(vid->window);
    glBindFramebuffer(GL_FRAMEBUFFER, vid->render_framebuffer);
    printf("[Video GLES] OpenGL ES 3.20 context initialized.\n");
    return 0;
}

void mips_video_free(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    
    glDeleteBuffers(1, &vid->vbo);
    glDeleteVertexArrays(1, &vid->vao);
    glDeleteTextures(1, &vid->framebuffer_texture);
    glDeleteTextures(1, &vid->ge_texture);
    glDeleteTextures(1, &vid->render_texture);
    glDeleteFramebuffers(1, &vid->render_framebuffer);
    glDeleteRenderbuffers(1, &vid->render_depthbuffer);
    glDeleteProgram(vid->program);
    free(vid->display_snapshot);
    vid->display_snapshot = NULL;
    vid->display_snapshot_size = 0;
    vid->display_snapshot_valid = false;
    
    SDL_GL_DeleteContext(vid->gl_context);
    SDL_DestroyWindow(vid->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    vid->initialized = false;
    printf("[Video GLES] Renderer shutdown complete.\n");
}

static void dump_display_framebuffer(MIPS_CPU *cpu, uint32_t topaddr,
                                     uint32_t stride, uint32_t format,
                                     uint32_t width, uint32_t height) {
    static int initialized;
    static char prefix[512];
    static uint32_t dump_count;
    enum { MAX_DISPLAY_DUMPS = 20 };

    if (!initialized) {
        initialized = 1;
        const char *setting = getenv("MIPS_DISPLAY_DUMP_PREFIX");
        if (setting != NULL && setting[0] != '\0') {
            int length = snprintf(prefix, sizeof(prefix), "%s", setting);
            if (length < 0 || (size_t)length >= sizeof(prefix)) {
                prefix[0] = '\0';
                fprintf(stderr,
                        "[Display Dump Error] MIPS_DISPLAY_DUMP_PREFIX is "
                        "too long.\n");
            }
        }
    }
    if (prefix[0] == '\0' || dump_count >= MAX_DISPLAY_DUMPS) return;
    uint32_t dump_number = ++dump_count;
    if (cpu == NULL || width == 0 || height == 0 || stride < width ||
        format > 3) {
        fprintf(stderr,
                "[Display Dump Error] Invalid framebuffer metadata "
                "(addr=%08X stride=%u format=%u size=%ux%u).\n",
                topaddr, stride, format, width, height);
        return;
    }
    size_t bytes_per_pixel = format == 3 ? 4 : 2;
    if ((size_t)stride > SIZE_MAX / bytes_per_pixel) {
        fprintf(stderr, "[Display Dump Error] Framebuffer row size overflow.\n");
        return;
    }
    size_t row_bytes = (size_t)stride * bytes_per_pixel;
    if ((size_t)height > SIZE_MAX / row_bytes) {
        fprintf(stderr, "[Display Dump Error] Framebuffer size overflow.\n");
        return;
    }
    size_t size = row_bytes * height;
    const uint8_t *source =
        (const uint8_t *)mips_get_ptr_range(cpu, topaddr, size);
    if (source == NULL) {
        fprintf(stderr,
                "[Display Dump Error] Guest framebuffer range is invalid "
                "(addr=%08X size=%zu).\n",
                topaddr, size);
        return;
    }

    char path[640];
    int length = snprintf(path, sizeof(path),
                          "%s_display%02u_%08X_f%u.bin",
                          prefix, dump_number, topaddr, format);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "[Display Dump Error] Output path is too long.\n");
        return;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "[Display Dump Error] Cannot open '%s'.\n", path);
        return;
    }
    size_t written = fwrite(source, 1, size, file);
    int close_result = fclose(file);
    if (written != size || close_result != 0) {
        fprintf(stderr, "[Display Dump Error] Failed writing '%s'.\n", path);
        return;
    }
    fprintf(stderr,
            "[Display Dump] #%u addr=%08X stride=%u format=%u size=%ux%u "
            "bytes=%zu file=%s\n",
            dump_number, topaddr, stride, format, width, height, size, path);
}

static void capture_present_backbuffer(MIPS_VideoSystem *vid) {
    static int initialized;
    static char prefix[512];
    static uint32_t capture_count;
    enum { MAX_PRESENT_CAPTURES = 20 };

    if (!initialized) {
        initialized = 1;
        const char *setting = getenv("MIPS_VIDEO_CAPTURE_PREFIX");
        if (setting != NULL && setting[0] != '\0') {
            int length = snprintf(prefix, sizeof(prefix), "%s", setting);
            if (length < 0 || (size_t)length >= sizeof(prefix)) {
                prefix[0] = '\0';
                fprintf(stderr,
                        "[Video Capture Error] MIPS_VIDEO_CAPTURE_PREFIX "
                        "is too long.\n");
            }
        }
    }
    if (prefix[0] == '\0' || capture_count >= MAX_PRESENT_CAPTURES) return;

    int width = 0;
    int height = 0;
    SDL_GL_GetDrawableSize(vid->window, &width, &height);
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4 / (size_t)height) {
        fprintf(stderr,
                "[Video Capture Error] Invalid drawable dimensions %dx%d.\n",
                width, height);
        return;
    }
    size_t row_bytes = (size_t)width * 4;
    size_t image_size = row_bytes * (size_t)height;
    uint8_t *pixels = (uint8_t *)malloc(image_size);
    uint8_t *row = (uint8_t *)malloc(row_bytes);
    if (pixels == NULL || row == NULL) {
        free(pixels);
        free(row);
        fprintf(stderr,
                "[Video Capture Error] Could not allocate %zu-byte "
                "backbuffer capture.\n",
                image_size);
        return;
    }

    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum read_error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    if (read_error != GL_NO_ERROR) {
        free(pixels);
        free(row);
        fprintf(stderr,
                "[Video Capture Error] glReadPixels failed with 0x%04X.\n",
                read_error);
        return;
    }

    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = pixels + (size_t)y * row_bytes;
        uint8_t *bottom = pixels + (size_t)(height - 1 - y) * row_bytes;
        memcpy(row, top, row_bytes);
        memcpy(top, bottom, row_bytes);
        memcpy(bottom, row, row_bytes);
    }

    uint32_t capture_number = capture_count + 1;
    char path[640];
    int length = snprintf(path, sizeof(path), "%s_present%02u.rgba",
                          prefix, capture_number);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        free(pixels);
        free(row);
        fprintf(stderr, "[Video Capture Error] Output path is too long.\n");
        return;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(pixels);
        free(row);
        fprintf(stderr, "[Video Capture Error] Cannot open '%s'.\n", path);
        return;
    }
    size_t written = fwrite(pixels, 1, image_size, file);
    int close_result = fclose(file);
    free(pixels);
    free(row);
    if (written != image_size || close_result != 0) {
        fprintf(stderr, "[Video Capture Error] Failed writing '%s'.\n", path);
        return;
    }
    capture_count = capture_number;
    fprintf(stderr,
            "[Video Capture] #%u size=%dx%d fbp=%08X fbw=%u format=%u "
            "draws=%u total_draws=%u file=%s\n",
            capture_number, width, height, vid->display_topaddr,
            vid->display_stride, vid->display_format,
            vid->draw_count_since_present, vid->total_draw_count, path);
}

void mips_video_set_framebuffer(MIPS_VideoSystem *vid, MIPS_CPU *cpu,
                                uint32_t topaddr, uint32_t stride,
                                uint32_t format, uint32_t width,
                                uint32_t height) {
    if (vid == NULL || !vid->initialized) return;
    dump_display_framebuffer(cpu, topaddr, stride, format, width, height);
    vid->display_cpu = cpu;
    vid->display_topaddr = topaddr;
    vid->display_stride = stride;
    vid->display_format = format;
    vid->display_width = width;
    vid->display_height = height;
    vid->framebuffer_dirty = true;
}

void mips_video_begin_frame(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    if (vid->frame_started) return;

    glBindFramebuffer(GL_FRAMEBUFFER, vid->render_framebuffer);
    vid->frame_started = true;
    glViewport(0, 0, 960, 544);
    glClear(GL_DEPTH_BUFFER_BIT);
    /* Preserve GE-only pixels across VBlanks until scanout changes. */
    if (!vid->framebuffer_dirty) return;

    if (vid->debug_ge_only) {
        glClearColor(0.30f, 0.30f, 0.30f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (vid->display_cpu == NULL || vid->display_width == 0 ||
        vid->display_height == 0 || vid->display_stride < vid->display_width) {
        return;
    }
    if (vid->display_format > 3 || vid->display_width > 512 ||
        vid->display_height > 512) {
        fprintf(stderr,
                "[HLE Display Error] Unsupported framebuffer format/dimensions "
                "(format=%u width=%u height=%u stride=%u).\n",
                vid->display_format, vid->display_width, vid->display_height,
                vid->display_stride);
        return;
    }

    uint32_t bytes_per_pixel = vid->display_format == 3 ? 4 : 2;
    size_t row_bytes = (size_t)vid->display_stride * bytes_per_pixel;
    if (row_bytes > SIZE_MAX / vid->display_height) {
        fprintf(stderr, "[HLE Display Error] Framebuffer size overflows host address space.\n");
        return;
    }
    size_t source_size = row_bytes * vid->display_height;
    const uint8_t *source = (const uint8_t *)mips_get_ptr_range(
        vid->display_cpu, vid->display_topaddr, source_size);
    if (source == NULL) {
        fprintf(stderr,
                "[HLE Display Error] Framebuffer range is outside emulated memory "
                "(addr=0x%08X size=%zu).\n",
                vid->display_topaddr, source_size);
        return;
    }

    bool framebuffer_changed = !vid->display_snapshot_valid ||
        vid->display_snapshot_size != source_size ||
        vid->display_snapshot_stride != vid->display_stride ||
        vid->display_snapshot_format != vid->display_format ||
        vid->display_snapshot_width != vid->display_width ||
        vid->display_snapshot_height != vid->display_height ||
        memcmp(vid->display_snapshot, source, source_size) != 0;
    if (!framebuffer_changed) return;

    uint8_t *snapshot = (uint8_t *)malloc(source_size);
    if (snapshot == NULL) {
        fprintf(stderr,
                "[HLE Display Error] Could not allocate %zu-byte "
                "framebuffer snapshot.\n",
                source_size);
        return;
    }
    memcpy(snapshot, source, source_size);

    size_t pixel_count = (size_t)vid->display_width * vid->display_height;
    if (pixel_count > SIZE_MAX / 4) {
        free(snapshot);
        fprintf(stderr, "[HLE Display Error] Framebuffer dimensions overflow host size.\n");
        return;
    }
    uint8_t *rgba = (uint8_t *)malloc(pixel_count * 4);
    if (rgba == NULL) {
        free(snapshot);
        fprintf(stderr, "[HLE Display Error] Could not allocate framebuffer upload buffer.\n");
        return;
    }
    for (uint32_t y = 0; y < vid->display_height; y++) {
        const uint8_t *row = source + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < vid->display_width; x++) {
            if (!mips_video_decode_display_pixel(
                    row + (size_t)x * bytes_per_pixel, vid->display_format,
                    rgba + ((size_t)y * vid->display_width + x) * 4)) {
                free(rgba);
                free(snapshot);
                fprintf(stderr, "[HLE Display Error] Could not decode PSP framebuffer pixel format.\n");
                return;
            }
        }
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vid->framebuffer_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)vid->display_width,
                 (GLsizei)vid->display_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    const GLES_Vertex quad[4] = {
        {-1.0f, -1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 0, 1},
        { 1.0f, -1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 1, 1},
        {-1.0f,  1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 0, 0},
        { 1.0f,  1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 1, 0},
    };
    const GLfloat identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
    };
    glUseProgram(vid->program);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUniform1i(vid->u_use_framebuffer_location, 1);
    glUniform1i(vid->u_framebuffer_location, 0);
    glUniform1i(vid->u_use_texture_location, 0);
    glUniformMatrix4fv(vid->u_mvp_location, 1, GL_FALSE, identity);
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    free(vid->display_snapshot);
    vid->display_snapshot = snapshot;
    vid->display_snapshot_size = source_size;
    vid->display_snapshot_stride = vid->display_stride;
    vid->display_snapshot_format = vid->display_format;
    vid->display_snapshot_width = vid->display_width;
    vid->display_snapshot_height = vid->display_height;
    vid->display_snapshot_valid = true;
}

static float ge_float24(uint32_t value) {
    uint32_t bits = value << 8;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static uint32_t ge_encode_float24(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits >> 8;
}

static int ge_dump_write(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "[GE Dump Error] Cannot open '%s' for writing.\n",
                path);
        return 0;
    }
    size_t written = fwrite(data, 1, size, file);
    int close_result = fclose(file);
    if (written != size || close_result != 0) {
        fprintf(stderr,
                "[GE Dump Error] Incomplete write to '%s' (%zu/%zu bytes).\n",
                path, written, size);
        return 0;
    }
    return 1;
}

static uint32_t ge_dump_hash(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void ge_dump_texture_data(const char *prefix, uint32_t draw_number,
                                 const MIPS_GraphicsEngine *ge,
                                 const uint8_t *source, size_t source_size,
                                 uint32_t width, uint32_t height,
                                 uint32_t format, uint32_t clut_format,
                                 uint32_t clut_entry_bytes,
                                 const uint8_t *indices,
                                 const uint8_t *rgba) {
    char path[512];
    char header[64];
    int ok = 1;
    uint32_t nonzero_indices = 0;
    uint32_t nonblack_rgba = 0;
    uint32_t unique_indices = 0;
    bool seen_indices[256] = {false};
    size_t pixel_count = (size_t)width * height;

    for (size_t i = 0; i < pixel_count; i++) {
        if (indices != NULL) {
            uint8_t index = indices[i];
            if (index != 0) nonzero_indices++;
            if (!seen_indices[index]) {
                seen_indices[index] = true;
                unique_indices++;
            }
        }
        const uint8_t *pixel = rgba + i * 4;
        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 0) {
            nonblack_rgba++;
        }
    }

    snprintf(path, sizeof(path), "%s_draw%u_texture.bin", prefix, draw_number);
    ok &= ge_dump_write(path, source, source_size);

    if (indices != NULL) {
        int header_size = snprintf(header, sizeof(header), "P5\n%u %u\n255\n",
                                   width, height);
        snprintf(path, sizeof(path), "%s_draw%u_indices.pgm", prefix,
                 draw_number);
        ok &= header_size > 0 &&
              ge_dump_write(path, header, (size_t)header_size);
        if (ok) {
            FILE *file = fopen(path, "ab");
            if (file == NULL) {
                fprintf(stderr,
                        "[GE Dump Error] Cannot append pixel data to '%s'.\n",
                        path);
                ok = 0;
            } else {
                size_t written = fwrite(indices, 1, pixel_count, file);
                int close_result = fclose(file);
                if (written != pixel_count || close_result != 0) {
                    fprintf(stderr,
                            "[GE Dump Error] Incomplete index image '%s'.\n",
                            path);
                    ok = 0;
                }
            }
        }
    }

    int header_size = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                               width, height);
    snprintf(path, sizeof(path), "%s_draw%u_resolved.ppm", prefix,
             draw_number);
    if (header_size <= 0 || !ge_dump_write(path, header, (size_t)header_size)) {
        ok = 0;
    } else {
        FILE *file = fopen(path, "ab");
        if (file == NULL) {
            fprintf(stderr,
                    "[GE Dump Error] Cannot append RGB data to '%s'.\n", path);
            ok = 0;
        } else {
            uint8_t rgb[3];
            for (size_t i = 0; i < pixel_count; i++) {
                rgb[0] = rgba[i * 4];
                rgb[1] = rgba[i * 4 + 1];
                rgb[2] = rgba[i * 4 + 2];
                if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) {
                    fprintf(stderr,
                            "[GE Dump Error] Incomplete RGB image '%s'.\n",
                            path);
                    ok = 0;
                    break;
                }
            }
            if (fclose(file) != 0) ok = 0;
        }
    }

    snprintf(path, sizeof(path), "%s_draw%u_resolved.rgba", prefix,
             draw_number);
    ok &= ge_dump_write(path, rgba, pixel_count * 4);

    if (format >= 4 && ge->clut_loaded) {
        uint32_t entries = ge->clut_loaded_bytes / clut_entry_bytes;
        uint32_t cols = entries < 16 ? entries : 16;
        uint32_t rows = cols != 0 ? (entries + cols - 1) / cols : 0;
        if (cols != 0 && rows != 0) {
            size_t palette_pixels = (size_t)cols * rows;
            uint8_t *palette_rgb = (uint8_t *)calloc(palette_pixels, 3);
            if (palette_rgb == NULL) {
                fprintf(stderr, "[GE Dump Error] Palette allocation failed.\n");
                ok = 0;
            } else {
                for (uint32_t i = 0; i < entries; i++) {
                    uint8_t decoded[4];
                    if (!mips_video_decode_display_pixel(
                            ge->clut + (size_t)i * clut_entry_bytes,
                            clut_format, decoded)) {
                        fprintf(stderr,
                                "[GE Dump Error] Failed decoding CLUT entry %u.\n",
                                i);
                        ok = 0;
                        break;
                    }
                    palette_rgb[(size_t)i * 3] = decoded[0];
                    palette_rgb[(size_t)i * 3 + 1] = decoded[1];
                    palette_rgb[(size_t)i * 3 + 2] = decoded[2];
                }
                snprintf(path, sizeof(path), "%s_draw%u_palette.ppm", prefix,
                         draw_number);
                header_size = snprintf(header, sizeof(header),
                                       "P6\n%u %u\n255\n", cols, rows);
                if (header_size <= 0 ||
                    !ge_dump_write(path, header, (size_t)header_size)) {
                    ok = 0;
                } else {
                    FILE *file = fopen(path, "ab");
                    if (file == NULL) {
                        fprintf(stderr,
                                "[GE Dump Error] Cannot append palette to '%s'.\n",
                                path);
                        ok = 0;
                    } else {
                        size_t bytes = palette_pixels * 3;
                        size_t written = fwrite(palette_rgb, 1, bytes, file);
                        int close_result = fclose(file);
                        if (written != bytes || close_result != 0) {
                            fprintf(stderr,
                                    "[GE Dump Error] Incomplete palette '%s'.\n",
                                    path);
                            ok = 0;
                        }
                    }
                }
                snprintf(path, sizeof(path), "%s_draw%u_palette.bin", prefix,
                         draw_number);
                ok &= ge_dump_write(path, ge->clut, ge->clut_loaded_bytes);
                free(palette_rgb);
            }
        }
    }

    fprintf(stderr,
            "[GE Dump] draw=%u texture=%ux%u format=%u source_bytes=%zu "
            "source_fnv1a=%08X indices_unique=%u indices_nonzero=%u "
            "indices_fnv1a=%08X palette_fnv1a=%08X "
            "rgba_nonzero=%u rgba_fnv1a=%08X result=%s\n",
            draw_number, width, height, format, source_size,
            ge_dump_hash(source, source_size), unique_indices, nonzero_indices,
            indices != NULL ? ge_dump_hash(indices, pixel_count) : 0,
            ge->clut_loaded
                ? ge_dump_hash(ge->clut, ge->clut_loaded_bytes) : 0,
            nonblack_rgba, ge_dump_hash(rgba, pixel_count * 4),
            ok ? "ok" : "failed");
}

static void ge_dump_draw_state(const char *prefix, uint32_t draw_number,
                               MIPS_VideoSystem *vid, MIPS_CPU *cpu,
                               const MIPS_GraphicsEngine *ge,
                               int prim_type, int vertex_count,
                               const GE_VertexLayout *layout,
                               const float raw_positions[4][3],
                               const float clip_positions[4][4],
                               const GLES_Vertex *vertices) {
    char path[512];
    snprintf(path, sizeof(path), "%s_draw%u_state.txt", prefix, draw_number);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "[GE Dump Error] Cannot open '%s' for writing.\n",
                path);
        return;
    }

    uint32_t clut_address =
        (ge->regs[CMD_CLUTADDR] & 0x00FFFFF0u) |
        ((ge->regs[CMD_CLUTADDRUPPER] << 8) & 0x0F000000u);
    fprintf(file,
            "draw=%u pc=%08X primitive=%d vertex_count=%d\n"
            "vtype=%06X through=%u stride=%d offsets(weight=%d tex=%d "
            "color=%d normal=%d position=%d)\n"
            "vaddr=%08X iaddr=%08X base=%08X offset=%08X origin=%08X\n"
            "texture_enable=%06X tformat=%u tmode=%06X tbp0=%06X "
            "tbw0=%06X tsize0=%06X clut_format=%06X clut_addr=%08X "
            "clut_upper=%06X cload=%06X clut_loaded=%u clut_bytes=%u\n"
            "texfunc=%06X texenv=%06X texscale=(%06X,%06X) "
            "texoffset=(%06X,%06X)\n"
            "fbp=%06X fbw=%06X zbp=%06X zbw=%06X psm=%06X clear=%06X\n"
            "display=(topaddr=%08X stride=%u format=%u size=%ux%u)\n"
            "enable=(blend=%06X alpha=%06X stencil=%06X color=%06X "
            "logicop=%06X) "
            "tests=(colorref=%06X colormask=%06X alpha=%06X "
            "stencil=%06X stencilop=%06X blendmode=%06X "
            "fixed=(%06X,%06X)) "
            "scissor=(%06X,%06X) region=(%06X,%06X)\n",
            draw_number, ge->pc >= 4 ? ge->pc - 4 : 0, prim_type & 7,
            vertex_count, ge->regs[0x12],
            ge->vtype.transform_bypass ? 1u : 0u, layout->stride,
            layout->weight_offset, layout->tex_offset, layout->color_offset,
            layout->normal_offset, layout->position_offset, ge->vaddr,
            ge->iaddr, ge->base_addr, ge->offset_addr, ge->regs[CMD_ORIGIN],
            ge->regs[CMD_TEXENABLE], ge->regs[CMD_TPSM] & 0xFu,
            ge->regs[CMD_TMODE], ge->regs[CMD_TBP0], ge->regs[CMD_TBW0],
            ge->regs[CMD_TSIZE0], ge->regs[CMD_CLUTFORMAT], clut_address,
            ge->regs[CMD_CLUTADDRUPPER], ge->regs[CMD_LOADCLUT],
            ge->clut_loaded ? 1u : 0u, ge->clut_loaded_bytes,
            ge->regs[CMD_TEXFUNC], ge->regs[0xCA], ge->regs[CMD_TEXSCALEU],
            ge->regs[CMD_TEXSCALEV], ge->regs[CMD_TEXOFFSETU],
            ge->regs[CMD_TEXOFFSETV], ge->regs[CMD_FBP], ge->regs[CMD_FBW],
            ge->regs[CMD_ZBP], ge->regs[CMD_ZBW], ge->regs[CMD_PSM],
            ge->regs[CMD_CLEAR], vid->display_topaddr,
            vid->display_stride, vid->display_format, vid->display_width,
            vid->display_height,
            ge->regs[0x21], ge->regs[0x22], ge->regs[0x24],
            ge->regs[0x27], ge->regs[0x28],
            ge->regs[0xD9], ge->regs[0xDA], ge->regs[0xDB],
            ge->regs[0xDC], ge->regs[0xDD], ge->regs[0xDF],
            ge->regs[0xE0], ge->regs[0xE1],
            ge->regs[CMD_SCISSOR1], ge->regs[CMD_SCISSOR2],
            ge->regs[CMD_REGION1], ge->regs[CMD_REGION2]);
    fprintf(file,
            "viewport_raw=(%06X,%06X,%06X;%06X,%06X,%06X) "
            "viewport_float=(%.9g,%.9g,%.9g;%.9g,%.9g,%.9g)\n"
            "raster_offset_raw=(%06X,%06X) raster_offset_pixels=(%.9g,%.9g)\n",
            ge->regs[CMD_VIEWPORTXSCALE], ge->regs[CMD_VIEWPORTYSCALE],
            ge->regs[CMD_VIEWPORTZSCALE], ge->regs[CMD_VIEWPORTXCENTER],
            ge->regs[CMD_VIEWPORTYCENTER], ge->regs[CMD_VIEWPORTZCENTER],
            ge_float24(ge->regs[CMD_VIEWPORTXSCALE]),
            ge_float24(ge->regs[CMD_VIEWPORTYSCALE]),
            ge_float24(ge->regs[CMD_VIEWPORTZSCALE]),
            ge_float24(ge->regs[CMD_VIEWPORTXCENTER]),
            ge_float24(ge->regs[CMD_VIEWPORTYCENTER]),
            ge_float24(ge->regs[CMD_VIEWPORTZCENTER]),
            ge->regs[CMD_OFFSETX], ge->regs[CMD_OFFSETY],
            (float)(ge->regs[CMD_OFFSETX] & 0xFFFFu) / 16.0f,
            (float)(ge->regs[CMD_OFFSETY] & 0xFFFFu) / 16.0f);

    fprintf(file, "world=");
    for (int i = 0; i < 12; i++) {
        fprintf(file, "%s%.9g", i == 0 ? "[" : ",", ge->world_matrix[i]);
    }
    fprintf(file, "]\nview=");
    for (int i = 0; i < 12; i++) {
        fprintf(file, "%s%.9g", i == 0 ? "[" : ",", ge->view_matrix[i]);
    }
    fprintf(file, "]\nprojection=");
    for (int i = 0; i < 16; i++) {
        fprintf(file, "%s%.9g", i == 0 ? "[" : ",", ge->proj_matrix[i]);
    }
    fprintf(file, "]\n");

    int captured_vertices = vertex_count < 4 ? vertex_count : 4;
    for (int v = 0; v < captured_vertices; v++) {
        uint64_t address = (uint64_t)ge->vaddr +
                           (uint64_t)(unsigned int)v *
                           (uint64_t)(unsigned int)layout->stride;
        const uint8_t *bytes = address <= UINT32_MAX
            ? (const uint8_t *)mips_get_ptr_range(
                  cpu, (uint32_t)address, (size_t)layout->stride)
            : NULL;
        fprintf(file, "vertex[%d] address=%08X raw_position=(%.9g,%.9g,%.9g) ",
                v, (uint32_t)address, raw_positions[v][0],
                raw_positions[v][1], raw_positions[v][2]);
        if (bytes == NULL) {
            fprintf(file, "raw_bytes=<invalid>\n");
            continue;
        }
        fprintf(file, "raw_bytes=");
        int bytes_to_print = layout->stride < 128 ? layout->stride : 128;
        for (int b = 0; b < bytes_to_print; b++) {
            fprintf(file, "%s%02X", b == 0 ? "" : " ", bytes[b]);
        }
        float ndc_x = vertices[v].x / vertices[v].w;
        float ndc_y = vertices[v].y / vertices[v].w;
        fprintf(file,
                "\n  clip=(%.9g,%.9g,%.9g,%.9g) "
                "gles_clip=(%.9g,%.9g,%.9g,%.9g) "
                "ndc=(%.9g,%.9g,%.9g) "
                "screen_480x272=(%.9g,%.9g) uv=(%.9g,%.9g) "
                "color=(%.9g,%.9g,%.9g,%.9g)\n",
                clip_positions[v][0], clip_positions[v][1],
                clip_positions[v][2], clip_positions[v][3],
                vertices[v].x, vertices[v].y, vertices[v].z, vertices[v].w,
                ndc_x, ndc_y, vertices[v].z / vertices[v].w,
                (ndc_x + 1.0f) * 240.0f, (1.0f - ndc_y) * 136.0f,
                vertices[v].u, vertices[v].v, vertices[v].r, vertices[v].g,
                vertices[v].b, vertices[v].a);
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "[GE Dump Error] Failed closing '%s'.\n", path);
        return;
    }
    fprintf(stderr, "[GE Dump] state=%s\n", path);

    snprintf(path, sizeof(path), "%s_draw%u_state.json", prefix, draw_number);
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "[GE Dump Error] Cannot open '%s' for writing.\n",
                path);
        return;
    }
    fprintf(file,
            "{\n"
            "  \"draw\": %u,\n"
            "  \"pc\": %u,\n"
            "  \"primitive\": %d,\n"
            "  \"vtype\": %u,\n"
            "  \"through\": %s,\n"
            "  \"viewport_raw\": [%u,%u,%u,%u,%u,%u],\n"
            "  \"offset_raw\": [%u,%u],\n"
            "  \"world_float24\": [",
            draw_number, ge->pc >= 4 ? ge->pc - 4 : 0, prim_type & 7,
            ge->regs[0x12], ge->vtype.transform_bypass ? "true" : "false",
            ge->regs[CMD_VIEWPORTXSCALE], ge->regs[CMD_VIEWPORTYSCALE],
            ge->regs[CMD_VIEWPORTZSCALE], ge->regs[CMD_VIEWPORTXCENTER],
            ge->regs[CMD_VIEWPORTYCENTER], ge->regs[CMD_VIEWPORTZCENTER],
            ge->regs[CMD_OFFSETX], ge->regs[CMD_OFFSETY]);
    for (int i = 0; i < 12; i++) {
        fprintf(file, "%s%u", i == 0 ? "" : ",",
                ge_encode_float24(ge->world_matrix[i]));
    }
    fprintf(file, "],\n  \"view_float24\": [");
    for (int i = 0; i < 12; i++) {
        fprintf(file, "%s%u", i == 0 ? "" : ",",
                ge_encode_float24(ge->view_matrix[i]));
    }
    fprintf(file, "],\n  \"projection_float24\": [");
    for (int i = 0; i < 16; i++) {
        fprintf(file, "%s%u", i == 0 ? "" : ",",
                ge_encode_float24(ge->proj_matrix[i]));
    }
    fprintf(file, "],\n  \"vertices\": [\n");
    for (int v = 0; v < captured_vertices; v++) {
        uint32_t address = ge->vaddr + (uint32_t)v *
                           (uint32_t)layout->stride;
        float ndc_x = vertices[v].x / vertices[v].w;
        float ndc_y = vertices[v].y / vertices[v].w;
        fprintf(file,
                "    {\"address\": %u, "
                "\"raw_position\": [%.9g,%.9g,%.9g], "
                "\"clip\": [%.9g,%.9g,%.9g,%.9g], "
                "\"gles_clip\": [%.9g,%.9g,%.9g,%.9g], "
                "\"ndc\": [%.9g,%.9g,%.9g], "
                "\"pixel\": [%.9g,%.9g]}%s\n",
                address, raw_positions[v][0], raw_positions[v][1],
                raw_positions[v][2], clip_positions[v][0],
                clip_positions[v][1], clip_positions[v][2],
                clip_positions[v][3], vertices[v].x, vertices[v].y,
                vertices[v].z, vertices[v].w, ndc_x, ndc_y,
                vertices[v].z / vertices[v].w,
                (ndc_x + 1.0f) * 240.0f, (1.0f - ndc_y) * 136.0f,
                v + 1 == captured_vertices ? "" : ",");
    }
    fprintf(file, "  ]\n}\n");
    if (fclose(file) != 0) {
        fprintf(stderr, "[GE Dump Error] Failed closing '%s'.\n", path);
        return;
    }
    fprintf(stderr, "[GE Dump] coordinates=%s\n", path);
}

static int upload_ge_texture(MIPS_VideoSystem *vid, MIPS_CPU *cpu,
                             MIPS_GraphicsEngine *ge, uint32_t draw_number,
                             const char *dump_prefix) {
    uint32_t format = ge->regs[0xC3] & 0xF;
    uint32_t width = 1U << (ge->regs[0xB8] & 0xF);
    uint32_t height = 1U << ((ge->regs[0xB8] >> 8) & 0xF);
    uint32_t tbw_reg = ge->regs[0xA8];
    uint32_t buffer_width = tbw_reg & 0xFFFF;
    uint32_t address = (ge->regs[0xA0] & 0x00FFFFF0) |
                       ((tbw_reg << 8) & 0x0F000000);
    uint32_t bytes_per_pixel = format == 3 || format == 7 ? 4 :
                              format == 2 || format == 6 ? 2 : 1;
    uint32_t storage_width = format == 4
        ? (buffer_width + 1u) / 2u : buffer_width;
    int swizzled = (ge->regs[0xC2] & 1) != 0;

    if (format > 7 || width == 0 || height == 0 || width > 512 ||
        height > 512 || buffer_width < width || buffer_width > 512) {
        log_texture_error(
            "Textured 2D primitive skipped: unsupported texture "
            "format/dimensions (format=%u size=%ux%u tbw=%u).\n",
            format, width, height, buffer_width);
        return 0;
    }
    size_t source_size;
    if (swizzled) {
        size_t row_blocks =
            ((size_t)storage_width * bytes_per_pixel + 15) / 16;
        size_t block_rows = ((size_t)height + 7) / 8;
        if (row_blocks > SIZE_MAX / 128 ||
            block_rows > SIZE_MAX / (row_blocks * 128)) {
            log_texture_error("Texture storage size overflows.\n");
            return 0;
        }
        source_size = block_rows * row_blocks * 128;
    } else {
        if ((size_t)storage_width > SIZE_MAX / bytes_per_pixel ||
            (size_t)storage_width * bytes_per_pixel > SIZE_MAX / height) {
            log_texture_error("Texture storage size overflows.\n");
            return 0;
        }
        source_size = (size_t)storage_width * height * bytes_per_pixel;
    }
    const uint8_t *source =
        (const uint8_t *)mips_get_ptr_range(cpu, address, source_size);
    if (source == NULL) {
        log_texture_error(
            "Textured 2D primitive skipped: texture memory is out of range "
            "(addr=0x%08X size=%zu).\n",
            address, source_size);
        return 0;
    }
    uint32_t clut_format = ge->regs[CMD_CLUTFORMAT] & 3u;
    uint32_t clut_shift = (ge->regs[CMD_CLUTFORMAT] >> 2) & 0x1Fu;
    uint32_t clut_mask = (ge->regs[CMD_CLUTFORMAT] >> 8) & 0xFFu;
    uint32_t clut_start =
        ((ge->regs[CMD_CLUTFORMAT] >> 16) & 0x1Fu) << 4;
    uint32_t clut_entry_bytes = clut_format == 3 ? 4u : 2u;
    if (format >= 4 &&
        (!ge->clut_loaded || ge->clut_loaded_bytes < clut_entry_bytes)) {
        log_texture_error(
            "Indexed texture has no loaded CLUT (format=%u clut_loaded=%u "
            "bytes=%u).\n",
            format, ge->clut_loaded ? 1u : 0u, ge->clut_loaded_bytes);
        return 0;
    }
    if ((size_t)width * height > SIZE_MAX / 4) {
        log_texture_error("Texture conversion size overflows.\n");
        return 0;
    }
    uint8_t *rgba = (uint8_t *)malloc((size_t)width * height * 4);
    uint8_t *indices = format == 5 && dump_prefix != NULL
        ? (uint8_t *)malloc((size_t)width * height) : NULL;
    if (rgba == NULL || (format == 5 && dump_prefix != NULL && indices == NULL)) {
        free(indices);
        free(rgba);
        log_texture_error("Could not allocate texture conversion buffer.\n");
        return 0;
    }
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t source_offset;
            uint32_t source_x = format == 4 ? x / 2u : x;
            if (!mips_video_texture_byte_offset(
                                                source_x, y, storage_width,
                                                bytes_per_pixel, swizzled,
                                                &source_offset) ||
                source_offset > source_size ||
                bytes_per_pixel > source_size - source_offset ||
                (format < 4 &&
                 !mips_video_decode_display_pixel(
                     source + source_offset, format,
                     rgba + ((size_t)y * width + x) * 4))) {
                free(indices);
                free(rgba);
                log_texture_error("Texture decode failed at (%u,%u).\n",
                                  x, y);
                return 0;
            }
            if (format >= 4) {
                uint32_t index;
                if (format == 4) {
                    index = (source[source_offset] >> ((x & 1u) * 4u)) & 0xFu;
                } else if (format == 5) {
                    index = source[source_offset];
                } else if (format == 6) {
                    uint16_t value;
                    memcpy(&value, source + source_offset, sizeof(value));
                    index = value;
                } else {
                    memcpy(&index, source + source_offset, sizeof(index));
                }
                if (indices != NULL) indices[(size_t)y * width + x] =
                    (uint8_t)index;

                uint32_t palette_index = ((index >> clut_shift) & clut_mask) |
                                         clut_start;
                uint32_t palette_entries = ge->clut_loaded_bytes /
                                           clut_entry_bytes;
                if (palette_index >= palette_entries) {
                    free(indices);
                    free(rgba);
                    log_texture_error(
                        "Indexed texture CLUT index %u is outside loaded "
                        "palette (%u entries).\n",
                        palette_index, palette_entries);
                    return 0;
                }
                if (!mips_video_decode_display_pixel(
                        ge->clut + (size_t)palette_index * clut_entry_bytes,
                        clut_format, rgba + ((size_t)y * width + x) * 4)) {
                    free(indices);
                    free(rgba);
                    log_texture_error("CLUT decode failed at index %u.\n",
                                      palette_index);
                    return 0;
                }
            }
        }
    }
    if (dump_prefix != NULL) {
        ge_dump_texture_data(dump_prefix, draw_number, ge, source, source_size,
                             width, height, format, clut_format,
                             clut_entry_bytes, indices, rgba);
    }
    free(indices);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vid->ge_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (ge->regs[0xC7] & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (ge->regs[0xC7] & 0x100) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width,
                 (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return 1;
}

static const char *ge_dump_prefix_for_draw(uint32_t draw_number) {
    static int initialized;
    static uint32_t target_draw;
    static char prefix[384];
    if (!initialized) {
        initialized = 1;
        const char *draw_setting = getenv("MIPS_GE_DUMP_DRAW");
        if (draw_setting != NULL && draw_setting[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(draw_setting, &end, 10);
            if (end != draw_setting && *end == '\0' && parsed > 0 &&
                parsed <= UINT32_MAX) {
                target_draw = (uint32_t)parsed;
            } else {
                fprintf(stderr,
                        "[GE Dump Error] Invalid MIPS_GE_DUMP_DRAW='%s'.\n",
                        draw_setting);
            }
        }
        const char *prefix_setting = getenv("MIPS_GE_DUMP_PREFIX");
        if (prefix_setting != NULL && prefix_setting[0] != '\0') {
            int length = snprintf(prefix, sizeof(prefix), "%s", prefix_setting);
            if (length < 0 || (size_t)length >= sizeof(prefix)) {
                prefix[0] = '\0';
                fprintf(stderr,
                        "[GE Dump Error] MIPS_GE_DUMP_PREFIX is too long.\n");
            }
        } else {
            snprintf(prefix, sizeof(prefix), "GE-dump");
        }
    }
    return target_draw == draw_number && prefix[0] != '\0' ? prefix : NULL;
}

static void transform_point_4x3(float out[3], const float in[3],
                                const float matrix[12]) {
    out[0] = in[0] * matrix[0] + in[1] * matrix[3] +
             in[2] * matrix[6] + matrix[9];
    out[1] = in[0] * matrix[1] + in[1] * matrix[4] +
             in[2] * matrix[7] + matrix[10];
    out[2] = in[0] * matrix[2] + in[1] * matrix[5] +
             in[2] * matrix[8] + matrix[11];
}

static void transform_point_4x4(float out[4], const float in[3],
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

void mips_video_draw_prim(MIPS_VideoSystem *vid, MIPS_CPU *cpu, MIPS_GraphicsEngine *ge, int prim_type, int vertex_count) {
    if (vid == NULL || !vid->initialized || ge == NULL || vertex_count <= 0) return;
    static uint32_t video_draw_number;
    if (video_draw_number != UINT32_MAX) video_draw_number++;
    uint32_t current_draw_number = video_draw_number;
    const char *dump_prefix = ge_dump_prefix_for_draw(current_draw_number);
    int trace_texture_vertices = 0;

    static int trace_3d_enabled = -1;
    static unsigned int trace_3d_draw_count = 0;
    if (trace_3d_enabled < 0) {
        const char *trace_setting = getenv("MIPS_VIDEO_TRACE_3D");
        trace_3d_enabled = trace_setting != NULL &&
                           strcmp(trace_setting, "1") == 0;
    }
    int trace_this_3d_draw = trace_3d_enabled &&
                             !ge->vtype.transform_bypass &&
                             trace_3d_draw_count < 4;
    if (trace_this_3d_draw) {
        trace_3d_draw_count++;
        fprintf(stderr,
                "[GE 3D Trace] draw=%u vtype=0x%06X vertices=%d "
                "viewport=(%.5g,%.5g,%.5g; %.5g,%.5g,%.5g) "
                "raster_offset=(%.5g,%.5g)\n",
                trace_3d_draw_count, ge->regs[0x12], vertex_count,
                ge_float24(ge->regs[CMD_VIEWPORTXSCALE]),
                ge_float24(ge->regs[CMD_VIEWPORTYSCALE]),
                ge_float24(ge->regs[CMD_VIEWPORTZSCALE]),
                ge_float24(ge->regs[CMD_VIEWPORTXCENTER]),
                ge_float24(ge->regs[CMD_VIEWPORTYCENTER]),
                ge_float24(ge->regs[CMD_VIEWPORTZCENTER]),
                (float)(ge->regs[CMD_OFFSETX] & 0xFFFF) / 16.0f,
                (float)(ge->regs[CMD_OFFSETY] & 0xFFFF) / 16.0f);
        fprintf(stderr, "[GE 3D Trace] world=");
        for (int i = 0; i < 12; i++) {
            fprintf(stderr, "%s%.5g", i == 0 ? "[" : ",",
                    ge->world_matrix[i]);
        }
        fprintf(stderr, "] view=");
        for (int i = 0; i < 12; i++) {
            fprintf(stderr, "%s%.5g", i == 0 ? "[" : ",",
                    ge->view_matrix[i]);
        }
        fprintf(stderr, "] proj=");
        for (int i = 0; i < 16; i++) {
            fprintf(stderr, "%s%.5g", i == 0 ? "[" : ",",
                    ge->proj_matrix[i]);
        }
        fprintf(stderr, "]\n");
    }
    
    GE_VertexLayout layout;
    mips_ge_get_vertex_layout(ge, &layout);
    int stride = layout.stride;
    
    if (stride <= 0) return;
    if (ge->vtype.morph_count != 1 || ge->vtype.index_fmt != 0) {
        static unsigned int unsupported_layout_count = 0;
        unsupported_layout_count++;
        if (unsupported_layout_count <= 16 ||
            (unsupported_layout_count % 1024) == 0) {
            fprintf(stderr,
                    "[GE Renderer] Skipping unsupported vertex layout "
                    "(morph=%d index_fmt=%d); advancing %d vertices.\n",
                    ge->vtype.morph_count, ge->vtype.index_fmt, vertex_count);
        }
        ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
        return;
    }
    if (ge->vtype.pos_fmt == 0) {
        fprintf(stderr, "[GE Renderer] Skipping primitive with no position data.\n");
        ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
        return;
    }
    int texture_requested = (ge->regs[0x1E] & 1) != 0;
    int texture_enabled = texture_requested && ge->vtype.tex_fmt != 0;
    if (vid->debug_ge_only) texture_enabled = 0;
    if (texture_requested && ge->vtype.tex_fmt == 0) {
        log_texture_error(
            "Texture enable ignored for primitive without UV attributes "
            "(vtype=0x%06X); drawing vertex color only.\n",
            ge->regs[0x12]);
    }
    if (texture_enabled &&
        !upload_ge_texture(vid, cpu, ge, current_draw_number, dump_prefix)) {
        log_texture_error(
            "Texture state unavailable for this primitive; drawing vertex "
            "color only (vtype=0x%06X).\n",
            ge->regs[0x12]);
        texture_enabled = 0;
    }

    static int texture_state_trace_enabled = -1;
    static unsigned int texture_state_trace_count;
    if (texture_state_trace_enabled < 0) {
        const char *setting = getenv("MIPS_GE_TRACE_TEXTURE_STATE");
        texture_state_trace_enabled = setting != NULL &&
                                      strcmp(setting, "1") == 0;
    }
    if (texture_state_trace_enabled && texture_requested &&
        texture_state_trace_count < 64) {
        texture_state_trace_count++;
        trace_texture_vertices = texture_enabled;
        uint32_t tsize = ge->regs[CMD_TSIZE0];
        uint32_t tbw = ge->regs[CMD_TBW0];
        uint32_t clut_address =
            (ge->regs[CMD_CLUTADDR] & 0x00FFFFF0u) |
            ((ge->regs[CMD_CLUTADDRUPPER] << 8) & 0x0F000000u);
        fprintf(stderr,
                "[GE TEX STATE] draw_pc=%08X size_pc=%08X TSIZE0=%06X "
                "TMODE=%06X TFORMAT=%u TBP0=%06X TBW0=%06X "
                "CLUTFORMAT=%06X CLUTADDR=%08X CLOAD=%06X "
                "clut_loaded=%u clut_bytes=%u\n",
                ge->pc >= 4 ? ge->pc - 4 : 0,
                ge->texture_size_pc[0], tsize, ge->regs[CMD_TMODE],
                ge->regs[CMD_TPSM] & 0xFu,
                ge->regs[CMD_TBP0], tbw, ge->regs[CMD_CLUTFORMAT],
                clut_address, ge->regs[CMD_LOADCLUT],
                ge->clut_loaded ? 1u : 0u, ge->clut_loaded_bytes);
    }
    
    // Allocate pack buffer for unrolling/translating vertices
    GLES_Vertex *vertices = (GLES_Vertex *)malloc(sizeof(GLES_Vertex) * vertex_count);
    if (vertices == NULL) return;
    
    // Unpack MIPS memory into normalized GLES Vertices
    float min_ndc_x = 0.0f, max_ndc_x = 0.0f;
    float min_ndc_y = 0.0f, max_ndc_y = 0.0f;
    float raw_positions[4][3] = {{0}};
    float clip_positions[4][4] = {{0}};
    for (int v = 0; v < vertex_count; v++) {
        uint64_t curr_vaddr64 = (uint64_t)ge->vaddr + (uint64_t)v * stride;
        uint8_t *v_ptr = curr_vaddr64 <= UINT32_MAX
            ? (uint8_t *)mips_get_ptr_range(cpu, (uint32_t)curr_vaddr64,
                                            (size_t)stride)
            : NULL;
        
        if (v_ptr == NULL) {
            free(vertices);
            static unsigned int invalid_vertex_count = 0;
            invalid_vertex_count++;
            if (invalid_vertex_count <= 8 ||
                (invalid_vertex_count % 1024) == 0) {
                fprintf(stderr,
                        "[GE Renderer] Primitive skipped: vertex %d is outside "
                        "emulated memory (base=0x%08X stride=%d).\n",
                        v, ge->vaddr, stride);
            }
            ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
            return;
        }
        
        int offset = layout.tex_offset;
        
        // 2. Unpack Texture UV coordinates
        float u = 0.0f, v_coord = 0.0f;
        uint16_t su = 0, sv = 0; // Initialize to secure compiler
        if (ge->vtype.tex_fmt != 0) {
            if (ge->vtype.transform_bypass && ge->vtype.tex_fmt == 3) {
                memcpy(&u, v_ptr + offset, 4);
                memcpy(&v_coord, v_ptr + offset + 4, 4);
            } else if (ge->vtype.transform_bypass &&
                       ge->vtype.tex_fmt == 2) {
                memcpy(&su, v_ptr + offset, 2);
                memcpy(&sv, v_ptr + offset + 2, 2);
                u = (float)su;
                v_coord = (float)sv;
            } else if (ge->vtype.transform_bypass) {
                u = (float)v_ptr[offset] * (1.0f / 128.0f);
                v_coord = (float)v_ptr[offset + 1] * (1.0f / 128.0f);
            } else if (ge->vtype.tex_fmt == 3) { // Float
                memcpy(&u, v_ptr + offset, 4);
                memcpy(&v_coord, v_ptr + offset + 4, 4);
            } else if (ge->vtype.tex_fmt == 2) { // 16-bit short
                memcpy(&su, v_ptr + offset, 2);
                memcpy(&sv, v_ptr + offset + 2, 2);
                u = (float)su * (1.0f / 32768.0f) *
                    ge_float24(ge->regs[0x48]);
                v_coord = (float)sv * (1.0f / 32768.0f) *
                          ge_float24(ge->regs[0x49]);
            } else { // 8-bit byte
                u = (float)v_ptr[offset] * (1.0f / 128.0f) *
                    ge_float24(ge->regs[0x48]);
                v_coord = (float)v_ptr[offset + 1] * (1.0f / 128.0f) *
                          ge_float24(ge->regs[0x49]);
            }
            if (!ge->vtype.transform_bypass &&
                ge->vtype.tex_fmt == 3) {
                u *= ge_float24(ge->regs[0x48]);
                v_coord *= ge_float24(ge->regs[0x49]);
            }
            if (!ge->vtype.transform_bypass) {
                u += ge_float24(ge->regs[0x4A]);
                v_coord += ge_float24(ge->regs[0x4B]);
            }
        }
        vertices[v].u = u;
        vertices[v].v = v_coord;
        
        // 3. Unpack Vertex colors
        offset = layout.color_offset;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        if (ge->vtype.col_fmt != 0) {
            uint8_t rgba[4];
            if (mips_video_decode_display_pixel(v_ptr + offset,
                                                (uint32_t)ge->vtype.col_fmt - 4,
                                                rgba)) {
                r = (float)rgba[0] / 255.0f;
                g = (float)rgba[1] / 255.0f;
                b = (float)rgba[2] / 255.0f;
                a = (float)rgba[3] / 255.0f;
            }
        }
        vertices[v].r = r;
        vertices[v].g = g;
        vertices[v].b = b;
        vertices[v].a = a;

        // 4. Unpack Positions
        offset = layout.position_offset;
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
                z = (float)(uint16_t)sz;
            } else { // 8-bit Byte
                int8_t bx = (int8_t)v_ptr[offset];
                int8_t by = (int8_t)v_ptr[offset + 1];
                int8_t bz = (int8_t)v_ptr[offset + 2];
                x = (float)bx;
                y = (float)by;
                z = (float)bz;
            }
        }
        if (v < 4) {
            raw_positions[v][0] = x;
            raw_positions[v][1] = y;
            raw_positions[v][2] = z;
        }
        
        // Transform mapping check
        if (ge->vtype.transform_bypass) {
            // Coordinate space is already 2D screen coordinate. Normalize to GLES NDC [-1, 1]
            vertices[v].x = (x / 240.0f) - 1.0f;
            vertices[v].y = 1.0f - (y / 136.0f);
            vertices[v].z = 0.0f;
            vertices[v].w = 1.0f;
            if (v < 4) {
                clip_positions[v][0] = vertices[v].x;
                clip_positions[v][1] = vertices[v].y;
                clip_positions[v][2] = vertices[v].z;
                clip_positions[v][3] = vertices[v].w;
            }
        } else {
            const float position[3] = {x, y, z};
            float world_position[3];
            float view_position[3];
            float clip_position[4];
            transform_point_4x3(world_position, position, ge->world_matrix);
            transform_point_4x3(view_position, world_position, ge->view_matrix);
            transform_point_4x4(clip_position, view_position, ge->proj_matrix);
            if (v < 4) {
                memcpy(clip_positions[v], clip_position,
                       sizeof(clip_positions[v]));
            }

            if (!isfinite(clip_position[0]) || !isfinite(clip_position[1]) ||
                !isfinite(clip_position[2]) || !isfinite(clip_position[3]) ||
                fabsf(clip_position[3]) < 1.0e-7f) {
                free(vertices);
                static unsigned int invalid_transform_count = 0;
                invalid_transform_count++;
                if (invalid_transform_count <= 8 ||
                    (invalid_transform_count % 1024) == 0) {
                    fprintf(stderr,
                            "[GE Renderer] Skipping primitive with invalid "
                            "clip-space position (vtype=0x%06X vertex=%d).\n",
                            ge->regs[0x12], v);
                }
                ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
                return;
            }
            float viewport_x_scale = ge_float24(ge->regs[CMD_VIEWPORTXSCALE]);
            float viewport_y_scale = ge_float24(ge->regs[CMD_VIEWPORTYSCALE]);
            float viewport_z_scale = ge_float24(ge->regs[CMD_VIEWPORTZSCALE]);
            float viewport_x_center = ge_float24(ge->regs[CMD_VIEWPORTXCENTER]);
            float viewport_y_center = ge_float24(ge->regs[CMD_VIEWPORTYCENTER]);
            float viewport_z_center = ge_float24(ge->regs[CMD_VIEWPORTZCENTER]);

            vertices[v].x = (viewport_x_scale / 240.0f) * clip_position[0] +
                ((viewport_x_center -
                  (float)(ge->regs[CMD_OFFSETX] & 0xFFFF) / 16.0f) /
                 240.0f - 1.0f) * clip_position[3];
            vertices[v].y = (-viewport_y_scale / 136.0f) * clip_position[1] +
                (1.0f - (viewport_y_center -
                         (float)(ge->regs[CMD_OFFSETY] & 0xFFFF) / 16.0f) /
                        136.0f) * clip_position[3];
            vertices[v].z = (2.0f * viewport_z_scale / 65535.0f) *
                clip_position[2] +
                (2.0f * viewport_z_center / 65535.0f - 1.0f) *
                clip_position[3];
            vertices[v].w = clip_position[3];
            if (trace_this_3d_draw && v < 4) {
                fprintf(stderr,
                        "[GE 3D Trace] vertex=%d raw=(%.5g,%.5g,%.5g) "
                        "clip=(%.5g,%.5g,%.5g,%.5g) "
                        "viewport=(%.5g,%.5g,%.5g,%.5g) "
                        "ndc=(%.5g,%.5g,%.5g)\n",
                        v, x, y, z, clip_position[0], clip_position[1],
                        clip_position[2], clip_position[3], vertices[v].x,
                        vertices[v].y, vertices[v].z, vertices[v].w,
                        vertices[v].x / vertices[v].w,
                        vertices[v].y / vertices[v].w,
                        vertices[v].z / vertices[v].w);
            }
        }
        float ndc_x = vertices[v].x / vertices[v].w;
        float ndc_y = vertices[v].y / vertices[v].w;
        if (v == 0) {
            min_ndc_x = max_ndc_x = ndc_x;
            min_ndc_y = max_ndc_y = ndc_y;
        } else {
            if (ndc_x < min_ndc_x) min_ndc_x = ndc_x;
            if (ndc_x > max_ndc_x) max_ndc_x = ndc_x;
            if (ndc_y < min_ndc_y) min_ndc_y = ndc_y;
            if (ndc_y > max_ndc_y) max_ndc_y = ndc_y;
        }
    }
    if (dump_prefix != NULL) {
        ge_dump_draw_state(dump_prefix, current_draw_number, vid, cpu, ge,
                           prim_type, vertex_count, &layout, raw_positions,
                           clip_positions, vertices);
    }
    if (trace_texture_vertices) {
        float min_u = vertices[0].u;
        float max_u = vertices[0].u;
        float min_v = vertices[0].v;
        float max_v = vertices[0].v;
        for (int v = 1; v < vertex_count; v++) {
            if (vertices[v].u < min_u) min_u = vertices[v].u;
            if (vertices[v].u > max_u) max_u = vertices[v].u;
            if (vertices[v].v < min_v) min_v = vertices[v].v;
            if (vertices[v].v > max_v) max_v = vertices[v].v;
        }
        fprintf(stderr,
                "[GE TEX UV] prim_pc=%08X vaddr=%08X vertices=%d "
                "vtype=%06X size=%ux%u scale=(%.7g,%.7g) "
                "offset=(%.7g,%.7g) range_u=[%.7g,%.7g] "
                "range_v=[%.7g,%.7g]\n",
                ge->pc >= 4 ? ge->pc - 4 : 0, ge->vaddr, vertex_count,
                ge->regs[CMD_VTYPE], 1U << (ge->regs[CMD_TSIZE0] & 0xF),
                1U << ((ge->regs[CMD_TSIZE0] >> 8) & 0xF),
                ge_float24(ge->regs[CMD_TEXSCALEU]),
                ge_float24(ge->regs[CMD_TEXSCALEV]),
                ge_float24(ge->regs[CMD_TEXOFFSETU]),
                ge_float24(ge->regs[CMD_TEXOFFSETV]),
                min_u, max_u, min_v, max_v);
        if (vertex_count <= 8) {
            for (int v = 0; v < vertex_count; v++) {
                fprintf(stderr,
                        "[GE TEX UV] vertex=%d uv=(%.7g,%.7g) "
                        "position_ndc=(%.7g,%.7g)\n",
                        v, vertices[v].u, vertices[v].v,
                        vertices[v].x / vertices[v].w,
                        vertices[v].y / vertices[v].w);
            }
        }
    }
    if (vid->debug_ge_only) {
        for (int v = 0; v < vertex_count; v++) {
            vertices[v].r = 1.0f;
            vertices[v].g = 1.0f;
            vertices[v].b = 1.0f;
            vertices[v].a = 1.0f;
        }
    }
    
    GLES_Vertex *draw_vertices = vertices;
    int draw_vertex_count = vertex_count;
    int draw_prim_type = prim_type & 7;
    if (draw_prim_type == 6) {
        if ((vertex_count & 1) != 0 || vertex_count > INT_MAX / 3 ||
            (size_t)vertex_count > SIZE_MAX / (3 * sizeof(*vertices))) {
            fprintf(stderr,
                    "[GE Renderer] Skipping malformed sprite primitive "
                    "(vertices=%d).\n",
                    vertex_count);
            free(vertices);
            ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
            return;
        }
        draw_vertex_count = vertex_count * 3;
        draw_vertices = (GLES_Vertex *)malloc(
            (size_t)draw_vertex_count * sizeof(*draw_vertices));
        if (draw_vertices == NULL) {
            free(vertices);
            fprintf(stderr, "[GE Renderer] Could not allocate sprite expansion buffer.\n");
            ge->vaddr += (uint32_t)vertex_count * (uint32_t)stride;
            return;
        }
        for (int pair = 0; pair < vertex_count / 2; pair++) {
            const GLES_Vertex *top_left = &vertices[pair * 2];
            const GLES_Vertex *bottom_right = &vertices[pair * 2 + 1];
            GLES_Vertex top_right = *top_left;
            GLES_Vertex bottom_left = *top_left;
            GLES_Vertex side_color = interpolate_vertex(top_left, bottom_right);
            top_right.x = bottom_right->x;
            top_right.u = bottom_right->u;
            top_right.z = side_color.z;
            top_right.r = side_color.r;
            top_right.g = side_color.g;
            top_right.b = side_color.b;
            top_right.a = side_color.a;
            bottom_left.y = bottom_right->y;
            bottom_left.v = bottom_right->v;
            bottom_left.z = side_color.z;
            bottom_left.r = side_color.r;
            bottom_left.g = side_color.g;
            bottom_left.b = side_color.b;
            bottom_left.a = side_color.a;
            GLES_Vertex corners[6] = {
                *top_left, top_right, bottom_left,
                bottom_left, top_right, *bottom_right
            };
            memcpy(draw_vertices + pair * 6, corners, sizeof(corners));
        }
        free(vertices);
    }

    mips_video_begin_frame(vid);

    // Activate GLES Shaders [7.3]
    glUseProgram(vid->program);
    glUniform1i(vid->u_use_framebuffer_location, 0);
    glUniform1i(vid->u_use_texture_location, texture_enabled ? 1 : 0);
    if (texture_enabled) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, vid->ge_texture);
        glUniform1i(vid->u_texture_location, 1);
        uint32_t texture_function = ge->regs[0xC9];
        uint32_t environment = ge->regs[0xCA];
        glUniform1i(vid->u_texture_function_location,
                    (GLint)(texture_function & 7));
        glUniform1i(vid->u_texture_has_alpha_location,
                    (texture_function & 0x100) ? 1 : 0);
        const GLfloat environment_color[4] = {
            (GLfloat)(environment & 0xFF) / 255.0f,
            (GLfloat)((environment >> 8) & 0xFF) / 255.0f,
            (GLfloat)((environment >> 16) & 0xFF) / 255.0f,
            1.0f
        };
        glUniform4fv(vid->u_texture_environment_location, 1,
                     environment_color);
    }
    glActiveTexture(GL_TEXTURE0);
    if (!vid->debug_ge_only && (ge->regs[0x21] & 1) != 0) {
        static const GLenum blend_src_factors[6] = {
            GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,
            GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA
        };
        static const GLenum blend_dst_factors[6] = {
            GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
            GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA
        };
        uint32_t blend = ge->regs[0xDF];
        uint32_t src_factor = blend & 0xF;
        uint32_t dst_factor = (blend >> 4) & 0xF;
        uint32_t equation = (blend >> 8) & 7;
        if (equation > 4) {
            static unsigned int unsupported_blend_equation_count = 0;
            unsupported_blend_equation_count++;
            if (unsupported_blend_equation_count <= 8 ||
                (unsupported_blend_equation_count % 1024) == 0) {
                fprintf(stderr,
                        "[GE Renderer] Blend equation %u is not supported; "
                        "using additive blending.\n",
                        equation);
            }
        }
        GLenum gl_equation = equation == 1 ? GL_FUNC_SUBTRACT :
                             equation == 2 ? GL_FUNC_REVERSE_SUBTRACT :
                             equation == 3 ? GL_MIN :
                             equation == 4 ? GL_MAX : GL_FUNC_ADD;
        glEnable(GL_BLEND);
        glBlendEquation(gl_equation);
        if (src_factor < 6 && dst_factor < 6) {
            glBlendFunc(blend_src_factors[src_factor],
                        blend_dst_factors[dst_factor]);
        } else {
            static unsigned int unsupported_blend_count = 0;
            unsupported_blend_count++;
            if (unsupported_blend_count <= 8 ||
                (unsupported_blend_count % 1024) == 0) {
                fprintf(stderr,
                        "[GE Renderer] Blend factors %u/%u are not yet "
                        "supported; using source-alpha blending.\n",
                        src_factor, dst_factor);
            }
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    } else {
        glDisable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
    }
    if (!ge->vtype.transform_bypass &&
        (ge->regs[CMD_ZTESTENABLE] & 1) != 0) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDisable(GL_CULL_FACE);
    static unsigned int primitive_log_count = 0;
    primitive_log_count++;
    if (primitive_log_count <= 16 || (primitive_log_count % 4096) == 0) {
        fprintf(stderr,
                "[GE Renderer] GE draw #%u prim=%d vertices=%d vtype=0x%06X "
                "addr=0x%08X texture=%d fmt=%u size=%ux%u swizzled=%d "
                "blend=%d uvfmt=%d texenable=%d firstuv=(%.3f,%.3f) "
                "firstclip=(%.3f,%.3f,%.3f,%.3f).\n",
                primitive_log_count, prim_type & 7, vertex_count,
                ge->regs[0x12], ge->vaddr, texture_enabled,
                ge->regs[0xC3] & 0xF,
                1U << (ge->regs[0xB8] & 0xF),
                1U << ((ge->regs[0xB8] >> 8) & 0xF),
                (ge->regs[0xC2] & 1) != 0,
                (ge->regs[0x21] & 1) != 0,
                ge->vtype.tex_fmt, texture_requested,
                draw_vertices[0].u, draw_vertices[0].v,
                draw_vertices[0].x, draw_vertices[0].y,
                draw_vertices[0].z, draw_vertices[0].w);
        fprintf(stderr,
                "[GE Renderer] GLES bounds NDC x=[%.3f,%.3f] "
                "y=[%.3f,%.3f] first_rgba=(%.1f,%.1f,%.1f,%.1f)%s.\n",
                min_ndc_x, max_ndc_x, min_ndc_y, max_ndc_y,
                draw_vertices[0].r, draw_vertices[0].g,
                draw_vertices[0].b, draw_vertices[0].a,
                vid->debug_ge_only ? " debug_white" : "");
    }
    
    // Bind vertex arrays [10.4]
    glBindVertexArray(vid->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vid->vbo);
    
    // Dynamic Buffer upload [6.2][opengles32-quick-reference-card.pdf]
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLES_Vertex) * draw_vertex_count,
                 draw_vertices, GL_DYNAMIC_DRAW);
    
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    glUniformMatrix4fv(vid->u_mvp_location, 1, GL_FALSE, mvp);
    
    // Map primitive types [10.5][opengles32-quick-reference-card.pdf]
    GLenum mode = GL_TRIANGLES;
    switch (draw_prim_type) {
        case 0: mode = GL_POINTS; break;
        case 1: mode = GL_LINES; break;
        case 2: mode = GL_LINE_STRIP; break;
        case 3: mode = GL_TRIANGLES; break;
        case 4: mode = GL_TRIANGLE_STRIP; break;
        case 5: mode = GL_TRIANGLE_FAN; break;
        case 6: mode = GL_TRIANGLES; break;
    }
    
    // Hardware accelerated Draw Call [10.5]
    glDrawArrays(mode, 0, draw_vertex_count);
    check_gl_error("GE primitive draw");
    if (vid->draw_count_since_present != UINT32_MAX) {
        vid->draw_count_since_present++;
    }
    if (vid->total_draw_count != UINT32_MAX) {
        vid->total_draw_count++;
    }
    
    // Clean state
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    
    free(draw_vertices);
    
    // Advance Vertex list pointer
    ge->vaddr += vertex_count * stride;
}

bool mips_video_present(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) vid->close_requested = true;
    }
    if (vid->close_requested) return true;
    if (!vid->frame_started && !vid->framebuffer_dirty) return false;
    mips_video_begin_frame(vid);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, vid->render_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 960, 544, 0, 0, 960, 544,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    capture_present_backbuffer(vid);
    SDL_GL_SwapWindow(vid->window);
    vid->draw_count_since_present = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, vid->render_framebuffer);
    glViewport(0, 0, 960, 544);
    vid->frame_started = false;
    vid->framebuffer_dirty = false;
    return false;
}

void mips_video_draw_test_triangle(MIPS_VideoSystem *vid) {
    if (vid == NULL || !vid->initialized) return;
    
    // Clear screen to graphite
    vid->frame_started = true;
    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Hardcoded simple test vertices (Degradê Vermelho-Verde-Azul)
    GLES_Vertex test_vertices[3] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, // Vermelho
        { 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f}, // Verde
        { 0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f}  // Azul
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
