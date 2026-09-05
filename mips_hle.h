#ifndef MIPS_HLE_H
#define MIPS_HLE_H

#include "mips_cpu.h"
#include "mips_ge.h"
#include <pthread.h>

#define MAX_THREADS          128
#define MAX_FILES            64
#define MAX_SEMAPHORES       64
#define MAX_EVENT_FLAGS      64

/*
 * PSP HLE Thread Representation mapped to Linux pthreads
 */
typedef struct {
    uint32_t psp_tid;        // PSP Thread ID
    char name[32];           // Thread name
    uint32_t entry_pc;       // MIPS target entry PC
    uint32_t arg_addr;       // Argument pointer address passed in $a0
    uint32_t init_priority;  // Thread priority
    uint32_t status;         // Running, suspended, etc.
    pthread_t linux_thread;  // Native POSIX Thread handle
    MIPS_CPU *cpu;           // Dedicated CPU state
    bool is_active;          // Slot status
} HLE_Thread;

/*
 * PSP Virtual File Descriptor
 */
typedef struct {
    int linux_fd;            // Standard Linux file descriptor
    char psp_path[256];      // PSP mapped path
    bool is_async;           // Async read flag
    uint32_t async_buffer;   // Destination PSP virtual RAM buffer
    uint32_t async_size;     // Size of the async request
    uint32_t async_result;   // Result bytes read/written
    pthread_t async_thread;  // Background read thread
    bool is_active;
} HLE_File;

/*
 * PSP Semaphore Simulation
 */
typedef struct {
    uint32_t psp_sid;
    char name[32];
    int count;
    int max_count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool is_active;
} HLE_Semaphore;

/*
 * PSP Event Flag Simulation
 */
typedef struct {
    uint32_t psp_evid;
    char name[32];
    uint32_t pattern;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool is_active;
} HLE_EventFlag;

/*
 * HLE Kernel State
 */
typedef struct {
    HLE_Thread threads[MAX_THREADS];
    HLE_File files[MAX_FILES];
    HLE_Semaphore semaphores[MAX_SEMAPHORES];
    HLE_EventFlag events[MAX_EVENT_FLAGS];
    
    // Graphics Engine core mapping
    MIPS_GraphicsEngine ge;
    void *video_system; // Mapped dynamically to MIPS_VideoSystem
    void *audio_system; // Mapped dynamically to MIPS_AudioSystem (for main.c assigning)

    pthread_mutex_t kernel_mutex;
} MIPS_HLE_Kernel;

// Initialize HLE system OS structures
int mips_hle_init(MIPS_HLE_Kernel *kernel);

// Clean up HLE system resources
void mips_hle_free(MIPS_HLE_Kernel *kernel);

// Intercepts MIPS syscall instructions and routes them via NID or ID hashes
void mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel);

#endif // MIPS_HLE_H
