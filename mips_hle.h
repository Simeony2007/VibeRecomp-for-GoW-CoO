#ifndef MIPS_HLE_H
#define MIPS_HLE_H

#include "mips_cpu.h"
#include "mips_ge.h"
#include <dirent.h>
#include <pthread.h>

#define MAX_THREADS          128
#define MAX_FILES            64
#define MAX_DIRECTORIES      64
#define MAX_SEMAPHORES       64
#define MAX_EVENT_FLAGS      64

#define MIPS_HLE_SAS_INIT       0x40010
#define MIPS_HLE_SAS_CORE       0x40011
#define MIPS_HLE_SAS_END_FLAG   0x40012
#define MIPS_HLE_SAS_GET_OUTPUT_MODE 0x40013
#define MIPS_HLE_SAS_REV_TYPE   0x40014
#define MIPS_HLE_SAS_REV_VON    0x40015
#define MIPS_HLE_SAS_REV_EVOL   0x40016
#define MIPS_HLE_SAS_SET_VOICE  0x40017
#define MIPS_HLE_SAS_KEY_ON     0x40018
#define MIPS_HLE_AUDIOCODEC_CHECK_NEED_MEM 0x40019
#define MIPS_HLE_AUDIOCODEC_GET_EDRAM      0x4001A
#define MIPS_HLE_SAS_REV_PARAM             0x4001B

typedef struct {
    bool configured;
    bool playing;
    bool on;
    bool paused;
    bool loop;
    uint32_t sample_addr;
    uint32_t sample_size;
    uint64_t samples_remaining;
} MIPS_SasVoice;

typedef struct {
    bool initialized;
    uint32_t context_addr;
    uint32_t grain_size;
    uint32_t output_mode;
    int32_t reverb_type;
    uint32_t reverb_delay;
    uint32_t reverb_feedback;
    uint32_t reverb_left_volume;
    uint32_t reverb_right_volume;
    bool reverb_dry_enabled;
    bool reverb_wet_enabled;
    MIPS_SasVoice voices[32];
} MIPS_SasState;

uint32_t mips_sas_set_voice(MIPS_SasState *sas, uint32_t voice,
                            uint32_t sample_addr, int32_t sample_size,
                            uint32_t loop);
uint32_t mips_sas_key_on(MIPS_SasState *sas, uint32_t voice);
void mips_sas_advance(MIPS_SasState *sas);
uint32_t mips_sas_get_end_flag(const MIPS_SasState *sas);

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
    int raw_image_fd;
    char psp_path[256];      // PSP mapped path
    bool is_async;           // Async read flag
    bool is_raw_sector;
    uint64_t raw_offset;
    uint64_t raw_length;
    uint64_t raw_position;
    uint32_t async_buffer;   // Destination PSP virtual RAM buffer
    uint32_t async_size;     // Size of the async request
    int64_t async_result;    // Async result, including sign-extended PSP errors
    MIPS_CPU *async_cpu;      // CPU owning the destination RAM
    pthread_t async_thread;  // Background read thread
    pthread_mutex_t async_mutex;
    pthread_cond_t async_cond;
    bool async_done;
    bool async_thread_valid;
    bool is_active;
    int async_priority;
    bool async_open;
} HLE_File;

typedef struct {
    DIR *linux_dir;
    char host_path[512];
    char psp_path[256];
    bool is_active;
} HLE_Directory;

/*
 * PSP Semaphore Simulation
 */
typedef struct {
    uint32_t psp_sid;
    char name[32];
    uint32_t attributes;
    int initial_count;
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
    uint32_t attributes;
    uint32_t initial_pattern;
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
    HLE_Directory directories[MAX_DIRECTORIES];
    HLE_Semaphore semaphores[MAX_SEMAPHORES];
    HLE_EventFlag events[MAX_EVENT_FLAGS];
    
    // Graphics Engine core mapping
    MIPS_GraphicsEngine ge;
    void *video_system; // Mapped dynamically to MIPS_VideoSystem
    void *audio_system; // Mapped dynamically to MIPS_AudioSystem (for main.c assigning)
    MIPS_SasState sas;
    int umd_image_fd;
    uint64_t umd_image_size;

    pthread_mutex_t kernel_mutex;
} MIPS_HLE_Kernel;

// Initialize HLE system OS structures
int mips_hle_init(MIPS_HLE_Kernel *kernel);
int mips_hle_resolve_case_insensitive(char *resolved, size_t resolved_size,
                                      const char *path);

// Clean up HLE system resources
void mips_hle_free(MIPS_HLE_Kernel *kernel);

// Intercepts MIPS syscall instructions and routes them via NID or ID hashes
void mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel);

#endif // MIPS_HLE_H
