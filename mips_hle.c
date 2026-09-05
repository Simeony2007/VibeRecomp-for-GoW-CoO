#include "mips_hle.h"
#include "mips_video.h"
#include "mips_audio.h"
#include "mips_ge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>

/*
 * Case-insensitive case folding for Linux filesystem support (since PSP is case-insensitive [Filesystem])
 */
static void sanitize_path(char *dest, const char *src) {
    const char *p = src;
    char *d = dest;
    
    // Check and remove mount protocols
    if (strncmp(p, "umd0:/", 6) == 0) {
        strcpy(d, "./umd0/");
        d += 7;
        p += 6;
    } else if (strncmp(p, "ms0:/", 5) == 0) {
        strcpy(d, "./ms0/");
        d += 6;
        p += 5;
    } else if (strncmp(p, "disc0:/", 7) == 0) {
        strcpy(d, "./umd0/");
        d += 7;
        p += 7;
    } else {
        strcpy(d, "./");
        d += 2;
    }

    while (*p) {
        if (*p == '\\') {
            *d = '/'; // Map Windows-like backslashes to POSIX
        } else {
            *d = tolower((unsigned char)*p);
        }
        d++;
        p++;
    }
    *d = '\0';
}

int mips_hle_init(MIPS_HLE_Kernel *kernel) {
    if (kernel == NULL) return -1;
    memset(kernel, 0, sizeof(MIPS_HLE_Kernel));
    pthread_mutex_init(&kernel->kernel_mutex, NULL);
    
    // Initialize the Graphics Engine state
    mips_ge_init(&kernel->ge);
    kernel->video_system = NULL;
    kernel->audio_system = NULL;
    
    // Create local mock directories for storage integrity
    mkdir("./umd0", 0777);
    mkdir("./ms0", 0777);
    
    printf("[HLE Kernel] Sub-system Virtual File System and thread controllers initialized.\n");
    return 0;
}

void mips_hle_free(MIPS_HLE_Kernel *kernel) {
    if (kernel == NULL) return;
    
    pthread_mutex_lock(&kernel->kernel_mutex);
    // Cleanup active files, threads, and sync objects
    for (int i = 0; i < MAX_FILES; i++) {
        if (kernel->files[i].is_active) {
            close(kernel->files[i].linux_fd);
        }
    }
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (kernel->semaphores[i].is_active) {
            pthread_mutex_destroy(&kernel->semaphores[i].mutex);
            pthread_cond_destroy(&kernel->semaphores[i].cond);
        }
    }
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (kernel->events[i].is_active) {
            pthread_mutex_destroy(&kernel->events[i].mutex);
            pthread_cond_destroy(&kernel->events[i].cond);
        }
    }
    pthread_mutex_unlock(&kernel->kernel_mutex);
    pthread_mutex_destroy(&kernel->kernel_mutex);
}

/*
 * Background Async Thread for sceIoReadAsync execution [Async I/O Real]
 */
static void *async_io_read_worker(void *arg) {
    HLE_File *file = (HLE_File *)arg;
    
    // Execute block reading from Linux filesystem
    uint8_t *temp_buf = (uint8_t *)malloc(file->async_size);
    ssize_t read_bytes = read(file->linux_fd, temp_buf, file->async_size);
    
    // Synchronize to the PSP Virtual Ram safely with memory barriers
    if (read_bytes > 0) {
        __sync_synchronize(); 
        file->async_result = (uint32_t)read_bytes;
    } else {
        file->async_result = -1;
    }
    
    free(temp_buf);
    
    // Signal completion
    file->is_async = false;
    printf("[HLE IO] Async read worker completed: read %d bytes.\n", (int)read_bytes);
    return NULL;
}

/*
 * HLE API Implementations
 */

// sceIoOpen(const char* file, int flags, int mode)
static int hle_sceIoOpen(MIPS_HLE_Kernel *kernel, const char *file_path, int flags, int mode) {
    (void)mode; // Suppress unused parameter warning
    pthread_mutex_lock(&kernel->kernel_mutex);
    
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!kernel->files[i].is_active) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1; // Out of file descriptors
    }
    
    char host_path[512];
    sanitize_path(host_path, file_path);
    
    // Map PSP flags to standard POSIX open flags [Filesystem]
    int posix_flags = O_RDONLY;
    if (flags & 0x00000002) posix_flags = O_WRONLY | O_CREAT;
    if (flags & 0x00000003) posix_flags = O_RDWR | O_CREAT;
    
    int fd = open(host_path, posix_flags, 0666);
    if (fd < 0) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        printf("[HLE IO Error] Failed to open path: %s (mapped: %s)\n", file_path, host_path);
        return -2; // File not found
    }
    
    kernel->files[slot].linux_fd = fd;
    snprintf(kernel->files[slot].psp_path, sizeof(kernel->files[slot].psp_path), "%s", file_path);
    kernel->files[slot].is_active = true;
    kernel->files[slot].is_async = false;
    
    pthread_mutex_unlock(&kernel->kernel_mutex);
    printf("[HLE IO] Opened file '%s' successfully at slot %d.\n", file_path, slot);
    return slot + 1; // Return PSP file handle (1-indexed)
}

// sceIoRead(int fd, void *data, int size)
static int hle_sceIoRead(MIPS_HLE_Kernel *kernel, int fd, MIPS_CPU *cpu, uint32_t ram_addr, int size) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;
    
    void *ptr = mips_get_ptr(cpu, ram_addr);
    if (ptr == NULL) return -2; // Invalid RAM target
    
    ssize_t read_bytes = read(kernel->files[slot].linux_fd, ptr, size);
    return (int)read_bytes;
}

// sceIoReadAsync(int fd, void *data, int size) [Async I/O Real]
static int hle_sceIoReadAsync(MIPS_HLE_Kernel *kernel, int fd, MIPS_CPU *cpu, uint32_t ram_addr, int size) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;
    
    void *ptr = mips_get_ptr(cpu, ram_addr);
    if (ptr == NULL) return -2;
    
    HLE_File *file = &kernel->files[slot];
    file->is_async = true;
    file->async_buffer = ram_addr;
    file->async_size = size;
    file->async_result = 0;
    
    pthread_create(&file->async_thread, NULL, async_io_read_worker, file);
    sched_yield(); 
    return 0; // Async read requested successfully
}

// sceKernelCreateSema(const char* name, int attr, int init_val, int max_val, void* option)
static int hle_sceKernelCreateSema(MIPS_HLE_Kernel *kernel, const char *name, int init_val, int max_val) {
    pthread_mutex_lock(&kernel->kernel_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (!kernel->semaphores[i].is_active) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1;
    }
    
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    snprintf(sema->name, sizeof(sema->name), "%s", name);
    sema->count = init_val;
    sema->max_count = max_val;
    pthread_mutex_init(&sema->mutex, NULL);
    pthread_cond_init(&sema->cond, NULL);
    sema->is_active = true;
    sema->psp_sid = slot + 1;
    
    pthread_mutex_unlock(&kernel->kernel_mutex);
    printf("[HLE Sema] Semaphore '%s' created (Init: %d, Max: %d) with ID %d.\n", name, init_val, max_val, sema->psp_sid);
    return sema->psp_sid;
}

// sceKernelWaitSema(int semaid, int signal, void* timeout)
static int hle_sceKernelWaitSema(MIPS_HLE_Kernel *kernel, int semaid, int signal) {
    int slot = semaid - 1;
    if (slot < 0 || slot >= MAX_SEMAPHORES || !kernel->semaphores[slot].is_active) return -1;
    
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    while (sema->count < signal) {
        pthread_cond_wait(&sema->cond, &sema->mutex);
    }
    sema->count -= signal;
    pthread_mutex_unlock(&sema->mutex);
    return 0;
}

// sceKernelSignalSema(int semaid, int signal)
static int hle_sceKernelSignalSema(MIPS_HLE_Kernel *kernel, int semaid, int signal) {
    int slot = semaid - 1;
    if (slot < 0 || slot >= MAX_SEMAPHORES || !kernel->semaphores[slot].is_active) return -1;
    
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    sema->count += signal;
    if (sema->count > sema->max_count) sema->count = sema->max_count;
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);
    return 0;
}

/*
 * Syscall Intercepting Routing Manager
 * Maps PSP NID hashes directly into HLE routines in POSIX
 */
void mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;
    
    // Arguments: $a0, $a1, $a2, $a3 [Allegrex Calling Convention]
    uint32_t arg0 = cpu->gpr[4];  // $a0
    uint32_t arg1 = cpu->gpr[5];  // $a1
    uint32_t arg2 = cpu->gpr[6];  // $a2
    uint32_t arg3 = cpu->gpr[7];  // $a3
    
    uint32_t return_val = 0;
    
    switch (syscall_id) {
        // --- IoFileMgrForUser ---
        case 0x11111: // Mock: sceIoOpen
            return_val = hle_sceIoOpen(kernel, (const char *)mips_get_ptr(cpu, arg0), (int)arg1, (int)arg2);
            break;
            
        case 0x11112: // Mock: sceIoRead
            return_val = hle_sceIoRead(kernel, (int)arg0, cpu, arg1, (int)arg2);
            break;
            
        case 0x11113: // Mock: sceIoReadAsync
            return_val = hle_sceIoReadAsync(kernel, (int)arg0, cpu, arg1, (int)arg2);
            break;
            
        case 0x11114: // Mock: sceIoClose
            pthread_mutex_lock(&kernel->kernel_mutex);
            int file_slot = (int)arg0 - 1;
            if (file_slot >= 0 && file_slot < MAX_FILES && kernel->files[file_slot].is_active) {
                close(kernel->files[file_slot].linux_fd);
                kernel->files[file_slot].is_active = false;
                return_val = 0;
            } else {
                return_val = -1;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        // --- ThreadManForUser ---
        case 0x22221: // Mock: sceKernelCreateSema
            return_val = hle_sceKernelCreateSema(kernel, (const char *)mips_get_ptr(cpu, arg0), (int)arg2, (int)arg3);
            break;
            
        case 0x22222: // Mock: sceKernelWaitSema
            return_val = hle_sceKernelWaitSema(kernel, (int)arg0, (int)arg1);
            break;
            
        case 0x22223: // Mock: sceKernelSignalSema
            return_val = hle_sceKernelSignalSema(kernel, (int)arg0, (int)arg1);
            break;
            
        case 0x22224: // Mock: sceKernelDelayThread
            usleep(arg0);
            return_val = 0;
            break;

        // --- GraphicsEngine (sceGe) ---
        case 0x33331: // Mock: sceGeListEnqueue
            printf("[HLE GE] Enqueued Display List (Start: 0x%08X, Stall: 0x%08X)\n", arg0, arg1);
            kernel->ge.video_system = kernel->video_system;
            if (kernel->video_system != NULL) {
                kernel->ge.draw_callback = (GEDrawCallback)mips_video_draw_prim;
            }
            mips_ge_run_list(&kernel->ge, cpu, arg0, arg1);
            return_val = 1; // List ID
            break;

        case 0x33332: // Mock: sceGeListSync
            return_val = 0; // Success
            break;

        // --- Audio OS Library (sceAudio) ---
        case 0x40001: // Mock: sceAudioChReserve
            if (kernel->audio_system != NULL) {
                return_val = mips_audio_reserve_channel((MIPS_AudioSystem *)kernel->audio_system, (int)arg0, (int)arg1, (int)arg2);
            } else {
                return_val = -1;
            }
            break;

        case 0x40002: // Mock: sceAudioChRelease
            if (kernel->audio_system != NULL) {
                return_val = mips_audio_release_channel((MIPS_AudioSystem *)kernel->audio_system, (int)arg0);
            } else {
                return_val = -1;
            }
            break;

        case 0x40003: // Mock: sceAudioOutputPanned (Blockingly wait to avoid latency desync)
            if (kernel->audio_system != NULL) {
                const int16_t *pcm_data = (const int16_t *)mips_get_ptr(cpu, arg3);
                if (pcm_data != NULL) {
                    MIPS_AudioSystem *sys = (MIPS_AudioSystem *)kernel->audio_system;
                    int format = sys->channels[(int)arg0].format; // 0=Mono, 1=Stereo
                    int sample_count = sys->channels[(int)arg0].sample_count;
                    int element_len = sample_count * (format == 1 ? 2 : 1);
                    
                    // Sychronous Blocking Lock Strategy to keep Audio and Video Thread running in 1:1 parity
                    int pushed = 0;
                    while (pushed < element_len && !cpu->exit_requested) {
                        int ret = mips_audio_output_panned(sys, (int)arg0, (int)arg1, (int)arg2, pcm_data + pushed, element_len - pushed);
                        if (ret > 0) {
                            pushed += ret;
                        } else {
                            // If audio buffer is saturated, block for 2 milliseconds to let Linux ALSA play out frames
                            usleep(2000);
                        }
                    }
                    return_val = 0; // Success
                } else {
                    return_val = -2; // Pointer violation
                }
            } else {
                return_val = -1;
            }
            break;
            
        default:
            printf("[HLE Warning] Unknown Syscall NID Hash 0x%05X triggered at PC: 0x%08X\n", syscall_id, cpu->pc);
            return_val = -1;
            break;
    }
    
    // Store returned result in $v0 [Allegrex Calling Convention]
    cpu->gpr[2] = return_val;
    
    // Increment PC past syscall
    cpu->pc += 4;
    cpu->next_pc = cpu->pc;
}

// Global default weak implementation of link handler
__attribute__((weak)) void hle_syscall(MIPS_CPU *cpu) {
    extern MIPS_HLE_Kernel kernel;
    mips_hle_syscall(cpu, &kernel);
}
