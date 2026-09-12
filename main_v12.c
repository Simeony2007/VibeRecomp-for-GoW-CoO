#include "mips_cpu.h"
#include "mips_dispatcher.h"
#include "mips_interpreter.h"
#include "mips_hle.h"
#include "mips_video.h"
#include "mips_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
*  MIPS Allegrex Dynamic Executable Loader (ELF/PRX) - God of War Port
*  Version 13 - Bulletproof Heuristic/Direct Module Linker & Import Patching (PRX Linker)
*  Fixed SceLibraryStubHeader alignment and offset layout.
*/

// Global HLE Kernel Instance
MIPS_HLE_Kernel kernel;

// Global video system binding pointer for accelerated graphics callbacks
void *global_video_system = NULL;

// Weak declaration of the registration function auto-generated in gow_core_aot.c.
__attribute__ ((weak)) void aot_register_blocks(MIPS_Dispatcher *disp) {
    (void)disp;
    printf("[AOT] No Ahead-of-Time recompiled blocks found (gow_core_aot.c not compiled).\n");
    printf("[AOT] System will operate in 100%% Fallback Interpreter mode.\n");
}

// Dynamic Syscall routing to our real POSIX-based HLE OS Kernel with a safe dummy fallback
// Emulated Thread context tracking
static uint32_t emu_thread_entry = 0;
static uint32_t relocated_gp = 0;

typedef struct {
    char lib_name[32];
    uint32_t nid;
    uint32_t syscall_code;
} PatchedFallbackStub;

#define MAX_PATCHED_FALLBACKS 1024
static PatchedFallbackStub fallback_stubs[MAX_PATCHED_FALLBACKS];
static int fallback_stubs_count = 0;

static int32_t file_async_results[32] = {0};

// --- Stateful HLE Mocks for ATRAC3plus and SasCore Audio ---
typedef struct {
    bool active;
    uint32_t data_addr;
    uint32_t data_size;
    int remaining_frames;
    int total_frames;
} MockAtrac;

#define MAX_MOCK_ATRACS 4
static MockAtrac mock_atracs[MAX_MOCK_ATRACS] = {0};

typedef struct {
    bool initialized;
    uint32_t context_addr;
    int max_voices;
} MockSas;

static MockSas mock_sas = {0};





#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

// Helper to sanitize path (converting backslashes and protocols) preserving case
static void custom_sanitize_path(char *dest, const char *src) {
    const char *p = src;
    char *d = dest;
    if (strncmp(p, "umd0:/", 6) == 0) { strcpy(d, "./umd0/"); d += 7; p += 6; }
    else if (strncmp(p, "ms0:/", 5) == 0) { strcpy(d, "./ms0/"); d += 6; p += 5; }
    else if (strncmp(p, "disc0:/", 7) == 0) { strcpy(d, "./umd0/"); d += 7; p += 7; }
    else { strcpy(d, "./"); d += 2; }

    while (*p) {
        if (*p == '\\') {
            *d = '/';
        } else {
            *d = *p; // PRESERVE CASE here so case-insensitive resolver can match it!
        }
        d++;
        p++;
    }
    *d = '\0';
}


// sceIoLseek(int fd, int64_t offset, int whence)
static int64_t hle_sceIoLseek(MIPS_HLE_Kernel *kernel, int fd, int64_t offset, int whence) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;
    off_t new_pos = lseek(kernel->files[slot].linux_fd, offset, whence);
    return (int64_t)new_pos;
}

// sceIoLseek32(int fd, int offset, int whence)
static int hle_sceIoLseek32(MIPS_HLE_Kernel *kernel, int fd, int offset, int whence) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;
    off_t new_pos = lseek(kernel->files[slot].linux_fd, offset, whence);
    return (int)new_pos;
}

// Case-insensitive path resolver walking directory segment by segment
static void resolve_case_insensitive(char *resolved, const char *path) {
    strcpy(resolved, "");
    const char *p = path;
    if (p[0] == '.' && p[1] == '/') {
        strcat(resolved, "./");
        p += 2;
    } else if (p[0] == '/') {
        strcat(resolved, "/");
        p += 1;
    }
    
    char segment[256];
    while (*p) {
        int len = 0;
        while (*p && *p != '/') {
            segment[len++] = *p++;
        }
        segment[len] = '\0';
        
        if (len == 0) {
            if (*p == '/') p++;
            continue;
        }
        
        const char *parent = (resolved[0] == '\0') ? "." : resolved;
        DIR *dir = opendir(parent);
        bool found = false;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcasecmp(entry->d_name, segment) == 0) {
                    if (resolved[0] != '\0' && resolved[strlen(resolved)-1] != '/') {
                        strcat(resolved, "/");
                    }
                    strcat(resolved, entry->d_name);
                    found = true;
                    break;
                }
            }
            closedir(dir);
        }
        
        if (!found) {
            if (resolved[0] != '\0' && resolved[strlen(resolved)-1] != '/') {
                strcat(resolved, "/");
            }
            strcat(resolved, segment);
        }
        
        if (*p == '/') p++;
    }
}

void hle_syscall(MIPS_CPU *cpu) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;

    // --- Custom Mapped HLE Syscall Handlers ---
    if (syscall_id == 0x11112) { // sceIoRead
        uint32_t fd = cpu->gpr[4];
        uint32_t buf_ptr = cpu->gpr[5];
        uint32_t size = cpu->gpr[6];
        int slot = fd - 1;
        int32_t bytes_read = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *dst = mips_get_ptr(cpu, buf_ptr);
            if (dst != NULL) {
                bytes_read = read(kernel.files[slot].linux_fd, dst, size);
                printf("[HLE IO] sceIoRead(fd=%u, size=%u): read %d bytes successfully to 0x%08X\n", fd, size, bytes_read, buf_ptr);
            } else {
                printf("[HLE IO Error] sceIoRead: invalid destination address 0x%08X\n", buf_ptr);
            }
        } else {
            printf("[HLE IO Error] sceIoRead: invalid or inactive fd %u\n", fd);
        }
        cpu->gpr[2] = (uint32_t)bytes_read;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x11113) { // sceIoReadAsync
        uint32_t fd = cpu->gpr[4];
        uint32_t buf_ptr = cpu->gpr[5];
        uint32_t size = cpu->gpr[6];
        int slot = fd - 1;
        int32_t bytes_read = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *dst = mips_get_ptr(cpu, buf_ptr);
            if (dst != NULL) {
                bytes_read = read(kernel.files[slot].linux_fd, dst, size);
                printf("[HLE IO] sceIoReadAsync (sync-fallback): read %d bytes successfully to 0x%08X\n", bytes_read, buf_ptr);
                file_async_results[slot] = bytes_read;
            } else {
                printf("[HLE IO Error] sceIoReadAsync: invalid destination address 0x%08X\n", buf_ptr);
            }
        } else {
            printf("[HLE IO Error] sceIoReadAsync: invalid or inactive fd %u\n", fd);
        }
        cpu->gpr[2] = 0; // Return success starting the async operation
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x11114) { // sceIoClose
        uint32_t fd = cpu->gpr[4];
        int slot = fd - 1;
        int ret = -1;
        pthread_mutex_lock(&kernel.kernel_mutex);
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            ret = close(kernel.files[slot].linux_fd);
            kernel.files[slot].is_active = false;
            printf("[HLE IO] sceIoClose(fd=%u): closed successfully\n", fd);
        } else {
            printf("[HLE IO Error] sceIoClose: invalid or inactive fd %u\n", fd);
        }
        pthread_mutex_unlock(&kernel.kernel_mutex);
        cpu->gpr[2] = (uint32_t)ret;
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x22221) { // sceKernelCreateSema
        uint32_t name_ptr = cpu->gpr[4];
        const char *name = (const char *)mips_get_ptr(cpu, name_ptr);
        printf("[HLE ThreadMan] sceKernelCreateSema: '%s'\n", name ? name : "NULL");
        cpu->gpr[2] = 1; // Return sema ID 1
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x22222) { // sceKernelWaitSema
        uint32_t semaid = cpu->gpr[4];
        uint32_t count = cpu->gpr[5];
        printf("[HLE ThreadMan] sceKernelWaitSema: ID %u, count %u\n", semaid, count);
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x22223) { // sceKernelSignalSema
        uint32_t semaid = cpu->gpr[4];
        uint32_t count = cpu->gpr[5];
        printf("[HLE ThreadMan] sceKernelSignalSema: ID %u, count %u\n", semaid, count);
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x22224) { // sceKernelDelayThread
        uint32_t usec = cpu->gpr[4]; // $a0
        printf("[HLE ThreadMan] sceKernelDelayThread: delaying %u us\n", usec);
        if (usec > 0) {
            usleep(usec);
        }
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x33331) { // sceGeListEnqueue
        uint32_t list_addr = cpu->gpr[4]; // $a0
        uint32_t stall_addr = cpu->gpr[5]; // $a1
        uint32_t cb_id = cpu->gpr[6]; // $a2
        printf("[HLE GE] sceGeListEnqueue(addr=0x%08X, stall=0x%08X, cbid=%d)\n", list_addr, stall_addr, cb_id);
        cpu->gpr[2] = 0x35000001; // Return dummy list ID
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x33332) { // sceGeListSync
        uint32_t list_id = cpu->gpr[4]; // $a0
        uint32_t mode = cpu->gpr[5]; // $a1
        printf("[HLE GE] sceGeListSync(id=0x%08X, mode=%d) -> Completed\n", list_id, mode);
        cpu->gpr[2] = 0; // 0 = Completed
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x40001) { // sceAudioChReserve
        int chan = (int32_t)cpu->gpr[4]; // $a0
        int samples = (int32_t)cpu->gpr[5]; // $a1
        int format = (int32_t)cpu->gpr[6]; // $a2
        printf("[HLE Audio] sceAudioChReserve: chan %d, %d samples, format %d\n", chan, samples, format);
        cpu->gpr[2] = 1; // Return dummy channel ID 1
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x40002) { // sceAudioChRelease
        int chan = (int32_t)cpu->gpr[4]; // $a0
        printf("[HLE Audio] sceAudioChRelease: chan %d\n", chan);
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x40003) { // sceAudioOutputPanned
        int chan = (int32_t)cpu->gpr[4]; // $a0
        int left_vol = (int32_t)cpu->gpr[5]; // $a1
        int right_vol = (int32_t)cpu->gpr[6]; // $a2
        uint32_t sample_ptr = cpu->gpr[7]; // $a3
        printf("[HLE Audio] sceAudioOutputPanned: chan %d, vol L:%d R:%d, ptr 0x%08X\n", chan, left_vol, right_vol, sample_ptr);
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept our custom fallback syscall for safely returning success on unimplemented imports
    if (syscall_id >= 0x50000) {
        int idx = syscall_id - 0x50000;
        uint32_t ret_val = 0;
        if (idx >= 0 && idx < fallback_stubs_count) {
            uint32_t nid = fallback_stubs[idx].nid;
            if (nid == 0x342061E5 || nid == 0xF77D77CB) {
                // Return 24MB of simulated free userspace memory instead of 0!
                ret_val = 24 * 1024 * 1024; // 25165824 bytes (0x01800000)
                printf("[HLE Mock] %s (NID: 0x%08X) called. Returning simulated free memory: %u bytes (24MB)\n",
                       fallback_stubs[idx].lib_name, nid, ret_val);
            } else if (nid == 0x977DE386) { // sceKernelLoadModule
                uint32_t path_ptr = cpu->gpr[4]; // $a0
                const char *path = (const char *)mips_get_ptr(cpu, path_ptr);
                printf("[HLE ModuleMgr] sceKernelLoadModule: Loading '%s'\n", path ? path : "NULL");
                ret_val = 0x1111; // Mock module handle
                // Write success (1) to the Dax Engine module initialization flag
                mips_write8(cpu, 0x08800000 + 0x003297DE, 1);
            } else if (nid == 0x50F0C1EC) { // sceKernelStartModule
                uint32_t mod_id = cpu->gpr[4]; // $a0
                printf("[HLE ModuleMgr] sceKernelStartModule: Starting Module ID 0x%X\n", mod_id);
                ret_val = 0; // Success
                // Write success (1) to the Dax Engine module initialization flag
                mips_write8(cpu, 0x08800000 + 0x003297DE, 1);
            } else if (nid == 0x7A20E7AF) { // sceAtracSetDataAndGetID
                uint32_t buf = cpu->gpr[4]; // $a0
                uint32_t size = cpu->gpr[5]; // $a1
                int slot = -1;
                for (int i = 0; i < MAX_MOCK_ATRACS; i++) {
                    if (!mock_atracs[i].active) {
                        slot = i;
                        break;
                    }
                }
                if (slot == -1) slot = 0; // fallback to 0
                mock_atracs[slot].active = true;
                mock_atracs[slot].data_addr = buf;
                mock_atracs[slot].data_size = size;
                mock_atracs[slot].remaining_frames = 1000;
                mock_atracs[slot].total_frames = 1000;
                printf("[HLE Atrac] sceAtracSetDataAndGetID: buf=0x%08X, size=%u -> Assigned ID=%d\n", buf, size, slot);
                ret_val = slot;
            } else if (nid == 0x9AE849A7) { // sceAtracGetRemainingFrames
                int atracID = cpu->gpr[4]; // $a0
                uint32_t outRemainingFramesPtr = cpu->gpr[5]; // $a1
                if (atracID >= 0 && atracID < MAX_MOCK_ATRACS && mock_atracs[atracID].active) {
                    if (outRemainingFramesPtr != 0) {
                        mips_write32(cpu, outRemainingFramesPtr, mock_atracs[atracID].remaining_frames);
                    }
                    printf("[HLE Atrac] sceAtracGetRemainingFrames: ID=%d, remaining=%d\n", atracID, mock_atracs[atracID].remaining_frames);
                }
                ret_val = 0; // success
            } else if (nid == 0x61EB33F5) { // sceAtracDecodeData
                int atracID = cpu->gpr[4]; // $a0
                uint32_t outSamplesPtr = cpu->gpr[5]; // $a1
                uint32_t outNumSamplesPtr = cpu->gpr[6]; // $a2
                uint32_t outEndFlagPtr = cpu->gpr[7]; // $a3
                
                // Read 5th argument from stack (MIPS O32 standard offset is 16 bytes from $sp)
                uint32_t outRemainingFramesPtr = mips_read32(cpu, cpu->gpr[29] + 16);
                if (outRemainingFramesPtr < 0x08000000 || outRemainingFramesPtr >= 0x0C000000) {
                    outRemainingFramesPtr = cpu->gpr[8]; // fallback to $t0
                }
                
                if (atracID >= 0 && atracID < MAX_MOCK_ATRACS && mock_atracs[atracID].active) {
                    mock_atracs[atracID].remaining_frames--;
                    if (mock_atracs[atracID].remaining_frames <= 0) {
                        mock_atracs[atracID].remaining_frames = mock_atracs[atracID].total_frames;
                    }
                    
                    if (outNumSamplesPtr != 0) {
                        mips_write32(cpu, outNumSamplesPtr, 2048); // 1024 samples per channel stereo
                    }
                    if (outEndFlagPtr != 0) {
                        mips_write32(cpu, outEndFlagPtr, 0); // Not finished
                    }
                    if (outRemainingFramesPtr != 0 && outRemainingFramesPtr >= 0x08000000 && outRemainingFramesPtr < 0x0C000000) {
                        mips_write32(cpu, outRemainingFramesPtr, mock_atracs[atracID].remaining_frames);
                    }
                    printf("[HLE Atrac] sceAtracDecodeData: ID=%d, decoded 2048 samples, remaining=%d\n", atracID, mock_atracs[atracID].remaining_frames);
                }
                ret_val = 0; // success
            } else if (nid == 0xFAA4F89B) { // sceAtracReleaseId
                int atracID = cpu->gpr[4]; // $a0
                if (atracID >= 0 && atracID < MAX_MOCK_ATRACS) {
                    mock_atracs[atracID].active = false;
                    printf("[HLE Atrac] sceAtracReleaseId: Released ID=%d\n", atracID);
                }
                ret_val = 0;
            } else if (nid == 0xA2BBA8BE) { // sceAtracGetInternalSample
                int atracID = cpu->gpr[4];
                uint32_t outSamplesPtr = cpu->gpr[5];
                if (outSamplesPtr != 0) {
                    mips_write32(cpu, outSamplesPtr, 2048);
                }
                printf("[HLE Atrac] sceAtracGetInternalSample: ID=%d, returned 2048 samples\n", atracID);
                ret_val = 0;
            } else if (nid == 0x36FAABFB) { // sceAtracGetNextDecodeSample
                int atracID = cpu->gpr[4];
                uint32_t outSamplesPtr = cpu->gpr[5];
                if (outSamplesPtr != 0) {
                    mips_write32(cpu, outSamplesPtr, 2048);
                }
                printf("[HLE Atrac] sceAtracGetNextDecodeSample: ID=%d, returned 2048 samples\n", atracID);
                ret_val = 0;
            } else if (nid == 0xE88F759B) { // sceAtracGetSecondBufferStatus
                int atracID = cpu->gpr[4];
                uint32_t outStatePtr = cpu->gpr[5];
                if (outStatePtr != 0) {
                    mips_write32(cpu, outStatePtr, 1); // 1 = buffer full / ready
                }
                printf("[HLE Atrac] sceAtracGetSecondBufferStatus: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0x2DD3E298) { // sceAtracGetSecondBuffer
                int atracID = cpu->gpr[4];
                printf("[HLE Atrac] sceAtracGetSecondBuffer: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0x5622B7C1) { // sceAtracGetSoundSampleOfFormat
                printf("[HLE Atrac] sceAtracGetSoundSampleOfFormat\n");
                ret_val = 2048;
            } else if (nid == 0x5D268707) { // sceAtracGetBufferInfoForResb
                int atracID = cpu->gpr[4];
                uint32_t outInfoPtr = cpu->gpr[5];
                printf("[HLE Atrac] sceAtracGetBufferInfoForResb: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0x644E5607) { // sceAtracGetBitrate
                int atracID = cpu->gpr[4];
                uint32_t outBitratePtr = cpu->gpr[5];
                if (outBitratePtr != 0) {
                    mips_write32(cpu, outBitratePtr, 128); // 128kbps
                }
                printf("[HLE Atrac] sceAtracGetBitrate: ID=%d, returned 128kbps\n", atracID);
                ret_val = 0;
            } else if (nid == 0x6A8C3CD5) { // sceAtracGetSecondBufferInfo
                int atracID = cpu->gpr[4];
                printf("[HLE Atrac] sceAtracGetSecondBufferInfo: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0x7DB31251) { // sceAtracSetLoopNum
                int atracID = cpu->gpr[4];
                int loopNum = cpu->gpr[5];
                printf("[HLE Atrac] sceAtracSetLoopNum: ID=%d, loopNum=%d\n", atracID, loopNum);
                ret_val = 0;
            } else if (nid == 0x83E85EA0) { // sceAtracGetLoopStatus
                int atracID = cpu->gpr[4];
                printf("[HLE Atrac] sceAtracGetLoopStatus: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0x868120B5) { // sceAtracSetSecondBuffer
                int atracID = cpu->gpr[4];
                printf("[HLE Atrac] sceAtracSetSecondBuffer: ID=%d\n", atracID);
                ret_val = 0;
            } else if (nid == 0xD5A229C9) { // sceSasInit
                uint32_t sasCore = cpu->gpr[4];
                int maxVoices = cpu->gpr[5];
                mock_sas.initialized = true;
                mock_sas.context_addr = sasCore;
                mock_sas.max_voices = maxVoices;
                printf("[HLE Sas] sceSasInit: core=0x%08X, maxVoices=%d\n", sasCore, maxVoices);
                ret_val = 0;
            } else if (nid == 0xE175EF66) { // sceSasSetVoice
                printf("[HLE Sas] sceSasSetVoice\n");
                ret_val = 0;
            } else if (nid == 0xE855BF76) { // sceSasSetEffect
                printf("[HLE Sas] sceSasSetEffect\n");
                ret_val = 0;
            } else if (nid == 0xF983B186) { // sceSasGetEndFlag
                // Returns whether voices are finished.
                printf("[HLE Sas] sceSasGetEndFlag (returns 1 for finished)\n");
                ret_val = 1;
            } else {
                printf("[HLE Fallback] Unimplemented API called: %s -> NID: 0x%08X at PC: 0x%08X (returns 0)\n",
                       fallback_stubs[idx].lib_name, nid, cpu->pc);
            }
        } else {
            printf("[HLE Fallback Warning] Unknown unimplemented syscall NID code 0x%X triggered at PC: 0x%08X\n", syscall_id, cpu->pc);
        }
        cpu->gpr[2] = ret_val;
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x99999) {
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc = cpu->gpr[31]; // Return directly to $ra!
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept sceIoOpen (NID: 0x109F50BC) to perform custom case-insensitive file system path mapping on Linux/WSL
    if (syscall_id == 0x11111) {
        uint32_t file_ptr = cpu->gpr[4]; // $a0
        uint32_t flags = cpu->gpr[5];    // $a1
        uint32_t mode = cpu->gpr[6];     // $a2
        const char *file_path = (const char *)mips_get_ptr(cpu, file_ptr);
        
        pthread_mutex_lock(&kernel.kernel_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_FILES; i++) {
            if (!kernel.files[i].is_active) {
                slot = i;
                break;
            }
        }
        
        if (slot == -1) {
            pthread_mutex_unlock(&kernel.kernel_mutex);
            printf("[HLE IO Error] Out of file descriptors for sceIoOpen: %s\n", file_path);
            cpu->gpr[2] = -1; // Out of file descriptors
            cpu->pc = cpu->gpr[31]; // Return directly to $ra
            cpu->next_pc = cpu->pc + 4;
            return;
        }
        
        char host_path[512];
        custom_sanitize_path(host_path, file_path);
        
        char real_host_path[512];
        resolve_case_insensitive(real_host_path, host_path);
        
        int posix_flags = O_RDONLY;
        if (flags & 0x00000002) posix_flags = O_WRONLY | O_CREAT;
        if (flags & 0x00000003) posix_flags = O_RDWR | O_CREAT;
        
        int fd = open(real_host_path, posix_flags, 0666);
        if (fd < 0) {
            pthread_mutex_unlock(&kernel.kernel_mutex);
            printf("[HLE IO Error] Failed to open path: %s (resolved: %s)\n", file_path, real_host_path);
            cpu->gpr[2] = -2; // File not found
            cpu->pc = cpu->gpr[31]; // Return directly to $ra
            cpu->next_pc = cpu->pc + 4;
            return;
        }
        
        kernel.files[slot].linux_fd = fd;
        snprintf(kernel.files[slot].psp_path, sizeof(kernel.files[slot].psp_path), "%s", file_path);
        kernel.files[slot].is_active = true;
        kernel.files[slot].is_async = false;
        
        pthread_mutex_unlock(&kernel.kernel_mutex);
        printf("[HLE IO] Opened file '%s' successfully at slot %d (resolved path: %s).\n", file_path, slot, real_host_path);
        
        cpu->gpr[2] = slot + 1; // Return PSP file handle (1-indexed)
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept sceIoLseek (NID: 0x27EB27B8)
    if (syscall_id == 0x11116) {
        uint32_t fd = cpu->gpr[4];     // $a0
        uint32_t offset_lo = cpu->gpr[5]; // $a1
        uint32_t offset_hi = cpu->gpr[6]; // $a2 (64-bit split on MIPS 32-bit registers)
        uint32_t whence = cpu->gpr[7];    // $a3
        int64_t offset = ((int64_t)offset_hi << 32) | offset_lo;
        
        int64_t ret = hle_sceIoLseek(&kernel, fd, offset, whence);
        cpu->gpr[2] = (uint32_t)(ret & 0xFFFFFFFF); // Return lower 32-bit of int64_t in $v0
        cpu->gpr[3] = (uint32_t)(ret >> 32);         // Return upper 32-bit of int64_t in $v1
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept sceIoLseek32 (NID: 0x68963324)
    if (syscall_id == 0x11117) {
        uint32_t fd = cpu->gpr[4];     // $a0
        int32_t offset = (int32_t)cpu->gpr[5]; // $a1
        uint32_t whence = cpu->gpr[6];    // $a2
        
        int ret = hle_sceIoLseek32(&kernel, fd, offset, whence);
        cpu->gpr[2] = ret;
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept sceIoDevctl (NID: 0x54F5FB11) to mock UMD drive status checks
    if (syscall_id == 0x11115) {
        uint32_t name_ptr = cpu->gpr[4]; // $a0 (device name: "umd:" or "umd0:")
        uint32_t cmd = cpu->gpr[5];      // $a1 (ioctl command)
        uint32_t argAddr = cpu->gpr[6];  // $a2
        uint32_t argLen = cpu->gpr[7];   // $a3
        uint32_t outPtr = cpu->gpr[8];   // $t0
        uint32_t outLen = cpu->gpr[9];   // $t1
        
        const char *dev_name = (const char *)mips_get_ptr(cpu, name_ptr);
        if (!dev_name) dev_name = "umd:";
        
        printf("[HLE IO] sceIoDevctl: Device: '%s', Cmd: 0x%08X, outPtr: 0x%08X, outLen: %d\n", dev_name, cmd, outPtr, outLen);
        
        uint32_t ret_val = 0;
        
        if (cmd == 0x01E18030) {
            ret_val = 1; // Region matches
        } else if (cmd == 0x01F20001) {
            if (outPtr != 0 && outLen >= 8) {
                mips_write32(cpu, outPtr + 4, 0x10); // Always return game disc
            }
            ret_val = 0;
        } else if (cmd == 0x01F20002) {
            if (outPtr != 0 && outLen >= 4) {
                mips_write32(cpu, outPtr, 0x10); // Assume first sector
            }
            ret_val = 0;
        } else if (cmd == 0x01F20003) {
            if (outPtr != 0 && outLen >= 4) {
                mips_write32(cpu, outPtr, 0x00180000); // Dummy UMD sector count
            }
            ret_val = 0;
        }
        
        cpu->gpr[2] = ret_val;
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Advanced thread scheduler context-switching bypass (sceKernelCreateThread NID: 0xF6414A71 / 0xD6DA4BA1 / 0x446D8DE6)
    if (syscall_id == 0x22225) { 
        uint32_t name_ptr = cpu->gpr[4]; // $a0
        uint32_t entry_pc = cpu->gpr[5]; // $a1
        const char *name = (const char *)mips_get_ptr(cpu, name_ptr);
        if (!name) name = "GoWMainThread";
        
        // Se o PC de entrada for menor que 0x04000000 (endereco virtual relativo do PSP),
        // nos devemos aplicar dinamicamente o load_bias (0x08800000) de usermode!
        if (entry_pc < 0x04000000) {
            entry_pc += 0x08800000;
        }
        
        emu_thread_entry = entry_pc; // Register the real main thread entry point!
        printf("[HLE ThreadMan] sceKernelCreateThread: Name: '%s', Entry PC: 0x%08X. Assigned Thread ID: 1\n", name, entry_pc);
        
        cpu->gpr[2] = 1; // Return thid = 1
        cpu->pc = cpu->gpr[31]; // Return directly to $ra
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    // sceKernelStartThread (NID: 0xF475845D)
    if (syscall_id == 0x22226) { 
        uint32_t thid = cpu->gpr[4]; // $a0
        uint32_t arg_size = cpu->gpr[5]; // $a1
        uint32_t arg_ptr = cpu->gpr[6];  // $a2
        printf("[HLE ThreadMan] sceKernelStartThread: Starting Thread ID %d. Diverting CPU context directly to Thread Entry: 0x%08X!\n", thid, emu_thread_entry);
        
        cpu->gpr[2] = 0; // Return success in $v0
        
        // 1. Initial Stack Allocation
        uint32_t stack_top = 0x0BFFF000;
        uint32_t k0_addr = stack_top - 256;
        uint32_t sp = k0_addr; // Stack pointer starts below k0 context block
        
        // 2. Setup GOW spec matching arguments block on stack if present
        if (arg_size > 0 && arg_ptr != 0) {
            uint32_t aligned_size = (arg_size + 15) & ~15;
            sp -= aligned_size;
            
            void *src = mips_get_ptr(cpu, arg_ptr);
            void *dst = mips_get_ptr(cpu, sp);
            if (src && dst) {
                memcpy(dst, src, arg_size);
                printf("[HLE ThreadMan] Copied thread arguments (%u bytes) to stack address: 0x%08X\n", arg_size, sp);
            }
            cpu->gpr[4] = arg_size; // $a0
            cpu->gpr[5] = sp;       // $a1
        } else {
            cpu->gpr[4] = 0; // $a0
            cpu->gpr[5] = 0; // $a1
        }
        
        // 3. Eat extra 64 bytes of stack as seen on real hardware entry functions
        sp -= 64;
        cpu->gpr[29] = sp;             // $sp (Stack Pointer)
        cpu->gpr[26] = k0_addr;        // $k0 (Thread Context pointer)
        
        // 4. Write the required ThreadK0 block parameters to stack (above our current stack pointer)
        mips_write32(cpu, k0_addr + 0xc0, 1);          // Thread ID (1)
        mips_write32(cpu, k0_addr + 0xc8, stack_top);  // Initial Stack Top
        mips_write32(cpu, k0_addr + 0xf8, 0xffffffff); // Sentinel exit limit
        mips_write32(cpu, k0_addr + 0xfc, 0xffffffff); // Sentinel exit limit
        
        // 5. Set up the return address to our HALT trap (0x08800000)
        cpu->gpr[31] = 0x08800100; // $ra points to safe HALT trap
        cpu->gpr[28] = relocated_gp; // Set $gp (Global Pointer) for the starting thread!
        
        cpu->pc = emu_thread_entry; // Desvia o Program Counter para a thread principal de God of War!
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    // Intercept sceKernelExitThread (NID: 0xAA73C935 / 0x8011F9B0) to cleanly terminate emulation loop
    if (syscall_id == 0x22227) {
        printf("[HLE ThreadMan] sceKernelExitThread called. Terminating emulation loop cleanly.\n");
        cpu->exit_requested = true;
        return;
    }

    // Fallback wrapper around mips_hle_syscall to secure direct $ra returns
    uint32_t old_ra = cpu->gpr[31];
    mips_hle_syscall(cpu, &kernel);
    cpu->pc = old_ra; // Directly return to $ra, skipping jr $ra execution
    cpu->next_pc = cpu->pc + 4;
}


// Post-Mortem Instruction Trace Buffer for deep debugging
#define TRACE_SIZE 512
typedef struct {
    uint32_t pc;
    uint32_t inst;
    uint32_t regs[32];
} TraceEntry;

static TraceEntry trace_buffer[TRACE_SIZE];
static int trace_index = 0;
static bool trace_wrapped = false;

void interpreter_step(MIPS_CPU *cpu) {
    // Auto-intercept when CPU enters Exception Vector 0x00000040 to diagnose crash in real time
    if (cpu->pc == 0x00000040) {
        printf("
======================================================================
");
        printf("[HLE EXCEPTION INTERCEPTED] CPU jumped to exception vector 0x00000040!
");
        printf("Simulated cycles: %llu
", (unsigned long long)cpu->cycles);
        printf("
--- Core General Purpose Registers (GPR) ---
");
        for (int r = 0; r < 32; r += 4) {
            printf("  GPR[%2d]: 0x%08X  GPR[%2d]: 0x%08X  GPR[%2d]: 0x%08X  GPR[%2d]: 0x%08X
",
                   r, cpu->gpr[r], r+1, cpu->gpr[r+1], r+2, cpu->gpr[r+2], r+3, cpu->gpr[r+3]);
        }
        printf("  HI: 0x%08X  LO: 0x%08X  FCR31: 0x%08X
", cpu->hi, cpu->lo, cpu->fcr31);
        
        printf("
--- Floating Point Registers (FPR) ---
");
        for (int f = 0; f < 32; f += 4) {
            printf("  FPR[%2d]: %f (0x%08X)  FPR[%2d]: %f (0x%08X)
",
                   f, cpu->fpr[f], *(uint32_t*)&cpu->fpr[f],
                   f+1, cpu->fpr[f+1], *(uint32_t*)&cpu->fpr[f+1]);
            printf("  FPR[%2d]: %f (0x%08X)  FPR[%2d]: %f (0x%08X)
",
                   f+2, cpu->fpr[f+2], *(uint32_t*)&cpu->fpr[f+2],
                   f+3, cpu->fpr[f+3], *(uint32_t*)&cpu->fpr[f+3]);
        }
        
        printf("
--- VFPU Vector Registers (First 32) ---
");
        for (int v = 0; v < 32; v += 4) {
            printf("  VFPU[%2d]: %f (0x%08X)  VFPU[%2d]: %f (0x%08X)
",
                   v, cpu->vfpu[v], *(uint32_t*)&cpu->vfpu[v],
                   v+1, cpu->vfpu[v+1], *(uint32_t*)&cpu->vfpu[v+1]);
            printf("  VFPU[%2d]: %f (0x%08X)  VFPU[%2d]: %f (0x%08X)
",
                   v+2, cpu->vfpu[v+2], *(uint32_t*)&cpu->vfpu[v+2],
                   v+3, cpu->vfpu[v+3], *(uint32_t*)&cpu->vfpu[v+3]);
        }
        
        printf("
--- Post-Mortem Trace of last 80 instructions executed before Exception ---
");
        int count = trace_wrapped ? TRACE_SIZE : trace_index;
        int start = trace_wrapped ? trace_index : 0;
        if (count > 80) {
            start = (trace_index - 80 + TRACE_SIZE) % TRACE_SIZE;
            count = 80;
        }
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % TRACE_SIZE;
            printf("  [Trace %02d] PC: 0x%08X, Inst: 0x%08X | $v0: %d, $a0: %d, $sp: 0x%08X, $ra: 0x%08X
",
                   i, trace_buffer[idx].pc, trace_buffer[idx].inst,
                   (int32_t)trace_buffer[idx].regs[2], (int32_t)trace_buffer[idx].regs[4],
                   trace_buffer[idx].regs[29], trace_buffer[idx].regs[31]);
        }
        printf("======================================================================
");
        fflush(stdout);
        cpu->exit_requested = 1;
        return;
    }

    // Record current state in trace buffer
    trace_buffer[trace_index].pc = cpu->pc;
    trace_buffer[trace_index].inst = mips_read32(cpu, cpu->pc);
    for (int i = 0; i < 32; i++) {
        trace_buffer[trace_index].regs[i] = cpu->gpr[i];
    }
    trace_index++;
    if (trace_index >= TRACE_SIZE) {
        trace_index = 0;
        trace_wrapped = true;
    }

    // Periodic Heartbeat to print state every 1,000,000 (1M) cycles (extremely useful to check if it's running or hung!)
    if (cpu->cycles > 0 && cpu->cycles % 1000000 == 0) {
        printf("[HLE Heartbeat] Cycles Simulated: %llu | Current PC: 0x%08X | $v0: %d, $a0: %d, $sp: 0x%08X\n",
               (unsigned long long)cpu->cycles, cpu->pc, (int32_t)cpu->gpr[2], (int32_t)cpu->gpr[4], cpu->gpr[29]);
        fflush(stdout);
    }

    mips_interpreter_step(cpu);
}

// Structures representing MIPS ELF32 Headers
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

// Structures representing PSP Module Headers (SceModuleInfo) - Packed to avoid compiler alignment issues
typedef struct {
    uint16_t modAttribute;
    uint16_t modVersion;
    char     modName[27];
    uint8_t  terminal;
    uint32_t gp_value;
    uint32_t ent_top;
    uint32_t ent_btm;
    uint32_t stub_top;
    uint32_t stub_btm;
} __attribute__ ((packed)) SceModuleInfo;

// Structures representing PSP Library Import Stubs Header (Officially Aligned & Corrigido!)
typedef struct {
    uint32_t lib_name_ptr;    // Pointer to library name (const char *)
    uint16_t version;         // Library version
    uint16_t attribute;       // Import attributes/flags
    uint8_t  struct_size;     // Size of SceLibraryStubHeader in 32-bit words (usually 5)
    uint8_t  num_vars;        // Number of imported variables (usually 0)
    uint16_t num_funcs;       // Real number of imported stubs/functions (num_stubs real)
    uint32_t nid_table_ptr;   // Pointer to NID table
    uint32_t stub_table_ptr;  // Pointer to stub table (.sceStub.text)
} __attribute__ ((packed)) SceLibraryStubHeader;

// Robust helper to perform safe dynamic pointer relocations
static inline uint32_t relocate_addr(uint32_t addr, uint32_t load_bias) {
    if (addr == 0) return 0;
    // Clear cache, uncached, and kernel segment bits to inspect the relative address
    uint32_t relative_addr = addr & 0x0FFFFFFF;
    if (relative_addr >= 0x08000000 && relative_addr < 0x0C000000) {
        // Already relocated (either as 0x08xxxxxx, 0x48xxxxxx, 0x88xxxxxx, or 0xA8xxxxxx)
        // Normalize it to standard cached user RAM space (0x08xxxxxx) for consistency
        return 0x08000000 + (relative_addr & 0x03FFFFFF);
    }
    // If relative_addr is smaller than 0x04000000, it's unrelocated, so apply load_bias
    if (relative_addr < 0x04000000) {
        return relative_addr + load_bias;
    }
    return addr;
}

// Check if SceModuleInfo pointer is completely valid and clean
static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias) {
    (void)cpu; // Suppress unused-parameter warnings
    if (!mod) return 0;

    // Safety check on attribute (0x0000 = user, 0x1000 = kernel, 0x0006 = standard)
    if (mod->modAttribute > 0x1FFF) return 0;

    // Safety checks on pointers - Relocated to actual RAM limits (relocated pointers can legitimately be 0!)
    uint32_t gp = relocate_addr(mod->gp_value, load_bias);
    uint32_t ent_top = relocate_addr(mod->ent_top, load_bias);
    uint32_t ent_btm = relocate_addr(mod->ent_btm, load_bias);
    uint32_t stub_top = relocate_addr(mod->stub_top, load_bias);
    uint32_t stub_btm = relocate_addr(mod->stub_btm, load_bias);

    if (gp != 0 && (gp < 0x08800000 || gp >= 0x0C000000)) return 0;
    if (ent_top != 0 && (ent_top < 0x08800000 || ent_top >= 0x0C000000 || (ent_top & 3) != 0)) return 0;
    if (ent_btm != 0 && (ent_btm < 0x08800000 || ent_btm >= 0x0C000000 || (ent_btm & 3) != 0)) return 0;
    if (stub_top != 0 && (stub_top < 0x08800000 || stub_top >= 0x0C000000 || (stub_top & 3) != 0)) return 0;
    if (stub_btm != 0 && (stub_btm < 0x08800000 || stub_btm >= 0x0C000000 || (stub_btm & 3) != 0)) return 0;

    if (ent_top != 0 && ent_btm != 0 && ent_top > ent_btm) return 0;
    if (stub_top != 0 && stub_btm != 0 && stub_top > stub_btm) return 0;
    if (stub_top == 0 && ent_top == 0) return 0; // Must contain either stubs or exports

    // Check if name has printable chars (first char check)
    if (mod->modName[0] < 32 || mod->modName[0] >= 127) return 0;

    return 1;
}

// Heuristically scan the emulated RAM to locate SceModuleInfo structure (100% bulletproof fallback)
static inline uint32_t find_sce_module_info(MIPS_CPU *cpu, uint32_t load_bias) {
    // Scan RAM from 0x08800000 to 0x0BFFF000 (KU0 Cached User space)
    for (uint32_t addr = 0x08800000; addr < 0x0BFFF000; addr += 4) {
        SceModuleInfo *mod = (SceModuleInfo *)mips_get_ptr(cpu, addr);
        if (is_valid_module_info(cpu, mod, load_bias)) {
            printf("[HLE Linker] Heuristically discovered SceModuleInfo at virtual address 0x%08X!\n", addr);
            return addr;
        }
    }
    return 0;
}

// High performance uncompressed ELF and decrypted plain ~PSP loader
int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        printf("[Loader Error] Failed to open game executable: %s\n", filepath);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data) {
        printf("[Loader Error] Out of memory allocating file buffer.\n");
        fclose(f);
        return -2;
    }

    if (fread(data, 1, size, f) != size) {
        printf("[Loader Error] Failed to read full binary from file.\n");
        free(data);
        fclose(f);
        return -3;
    }
    fclose(f);

    uint8_t *elf_data = data;
    size_t elf_size = size;
    (void)elf_size; // Suppress unused-variable warnings

    uint32_t modinfo_offset = 0;

    // Detect ~PSP (PRX) container
    if (size >= 4 && memcmp(data, "~PSP", 4) == 0) {
        printf("[Loader] Detected ~PSP (PRX) executable container.\n");
        modinfo_offset = *(uint32_t*)&data[52]; // Read modinfo_offset directly from ~PSP header
        
        // PSP executables contain an embedded ELF inside the PRX block. Let's find it.
        uint8_t *elf_ptr = NULL;
        for (size_t i = 0; i < size - 4; i++) {
            if (data[i] == 0x7F && data[i+1] == 'E' && data[i+2] == 'L' && data[i+3] == 'F') {
                elf_ptr = &data[i];
                elf_size = size - i;
                break;
            }
        }

        if (elf_ptr != NULL) {
            printf("[Loader] Found embedded MIPS ELF32 at offset 0x%X inside PRX!\n", (unsigned int)(elf_ptr - data));
            elf_data = elf_ptr;
        } else {
            // Check compression type in header
            uint16_t comp_type = *(uint16_t*)&data[6];
            if ((comp_type & 0xF00) != 0x300) {
                printf("[Loader Error] PRX file is encrypted or compressed (Type 0x%03X).\n", comp_type & 0xF00);
                printf("[Loader Error] Please decrypt/decompress it first (e.g. using PPSSPP's decrypted ELF dump).\n");
                free(data);
                return -4;
            }

            // Fallback for flat uncompressed PRX mapping directly past header
            uint32_t boot_entry = *(uint32_t*)&data[48];
            printf("[Loader] PRX is uncompressed/decrypted but has no ELF signature. Segments: %d.\n", data[39]);
            printf("[Loader] Mapping flat program space directly to standard base address 0x08804000.\n");
            
            void *dest = mips_get_ptr(cpu, 0x08804000);
            if (!dest) {
                printf("[Loader Error] Target physical memory at 0x08804000 is unavailable.\n");
                free(data);
                return -5;
            }
            memcpy(dest, data + 0x150, size - 0x150); // Copy everything skipping PRX header
            *entry_point = boot_entry + 0x08804000;
            free(data);
            return 0;
        }
    }

    // Load standard ELF format with byte checks to avoid hex escape warning
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        printf("[Loader Error] Game binary does not have a valid ELF magic number.\n");
        free(data);
        return -6;
    }

    // Auto-relocation Base Address Calculation
    uint32_t load_bias = 0;
    if (ehdr->e_entry < 0x08000000) {
        load_bias = 0x08800000;
        printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying PSP user RAM load bias: 0x%08X\n", ehdr->e_entry, load_bias);
    }

    *entry_point = ehdr->e_entry + load_bias;
    printf("[Loader] Successfully parsed MIPS ELF Header. Entry Point: 0x%08X\n", *entry_point);

    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    int loaded_count = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD segment
            uint32_t target_vaddr = phdr->p_vaddr + load_bias;
            printf("  -> Loading PT_LOAD segment %d: VADDR 0x%08X (original: 0x%08X), File Size: %u bytes, Memory Size: %u bytes, Flags: 0x%X\n",
                   loaded_count, target_vaddr, phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz, phdr->p_flags);
            
            void *dest = mips_get_ptr(cpu, target_vaddr);
            if (!dest) {
                printf("[Loader Error] Memory mapping violation: Segment virtual range is out of bounds!\n");
                free(data);
                return -7;
            }
            // Copy initialized data
            memcpy(dest, elf_data + phdr->p_offset, phdr->p_filesz);
            // Fill BSS / unitialized data with zeros
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((uint8_t*)dest + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
            }
            loaded_count++;
        }
    }

    printf("[Loader] Successfully loaded %d executable segments into emulated memory spaces.\n", loaded_count);

    // --- SceModuleInfo OFFSET EXTRACTION FROM DECRYPTED ELF ---
    if (modinfo_offset == 0 && ehdr->e_phnum > 0) {
        Elf32_Phdr *first_phdr = (Elf32_Phdr*)(phdr_table);
        // Standard ELF p_paddr is often 0 or equals p_vaddr. In PRX, first program header's p_paddr 
        // is uniquely set to SceModuleInfo file offset (different from p_vaddr).
        if (first_phdr->p_paddr != 0 && first_phdr->p_paddr != first_phdr->p_vaddr) {
            modinfo_offset = first_phdr->p_paddr & 0x7FFFFFFF; // Clear the MSB kernel-mode flag if present
            printf("[Loader] ELF format detected. Retrieved SceModuleInfo offset from first phdr p_paddr: 0x%08X\n", modinfo_offset);
        } else {
            printf("[Loader] Standard ELF detected. Skipping direct p_paddr modinfo offset to avoid false positives.\n");
        }
    }

    // --- HLE LINKER & IMPORT STUB PATCHING (PRX RESOLVER) ---
    uint32_t resolved_modinfo_addr = 0;

    // 1. First path: check if the direct pointer is valid (passing load_bias for relocations)
    if (modinfo_offset != 0) {
        // Try treating as virtual reloc relative address first (standard for decrypted ELFs)
        uint32_t test_addr = relocate_addr(modinfo_offset, load_bias);
        SceModuleInfo *mod = (SceModuleInfo *)mips_get_ptr(cpu, test_addr);
        if (mod && is_valid_module_info(cpu, mod, load_bias)) {
            resolved_modinfo_addr = test_addr;
            printf("[HLE Linker] Direct modinfo virtual offset is VALID. Using SceModuleInfo at virtual address 0x%08X.\n", resolved_modinfo_addr);
        }

        // If that fails, try treating as a file offset (standard for raw PRX files)
        if (resolved_modinfo_addr == 0) {
            for (int i = 0; i < ehdr->e_phnum; i++) {
                Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
                if (phdr->p_type == 1) { // PT_LOAD segment
                    if (modinfo_offset >= phdr->p_offset && modinfo_offset < phdr->p_offset + phdr->p_filesz) {
                        uint32_t relative_offset = modinfo_offset - phdr->p_offset;
                        uint32_t file_addr = phdr->p_vaddr + relative_offset + load_bias;
                        SceModuleInfo *fmod = (SceModuleInfo *)mips_get_ptr(cpu, file_addr);
                        if (fmod && is_valid_module_info(cpu, fmod, load_bias)) {
                            resolved_modinfo_addr = file_addr;
                            printf("[HLE Linker] Direct modinfo file offset is VALID. Using SceModuleInfo at virtual address 0x%08X.\n", resolved_modinfo_addr);
                            break;
                        }
                    }
                }
            }
        }
    }

    // 2. Fallback: Scan memory heuristically if direct pointer is missing or corrupted
    if (resolved_modinfo_addr == 0) {
        printf("[HLE Linker Warning] Direct modinfo offset is invalid or missing. Initiating heuristic memory scanner...\n");
        resolved_modinfo_addr = find_sce_module_info(cpu, load_bias);
    }

    if (resolved_modinfo_addr != 0) {
        SceModuleInfo *mod_info = (SceModuleInfo *)mips_get_ptr(cpu, resolved_modinfo_addr);
        if (mod_info != NULL) {
            relocated_gp = relocate_addr(mod_info->gp_value, load_bias);
            printf("[HLE Linker] Module Name: '%s', Version: %d.%d, Attributes: 0x%04X, GP: 0x%08X (relocated: 0x%08X)\n",
                   mod_info->modName, mod_info->modVersion & 0xFF, mod_info->modVersion >> 8, mod_info->modAttribute, mod_info->gp_value, relocated_gp);
            
            uint32_t stub_top_addr = relocate_addr(mod_info->stub_top, load_bias);
            uint32_t stub_btm_addr = relocate_addr(mod_info->stub_btm, load_bias);
            printf("[HLE Linker] Stub table virtual range: 0x%08X - 0x%08X\n", stub_top_addr, stub_btm_addr);
            
            uint32_t curr_stub_addr = stub_top_addr;
            int patch_count = 0;
            int total_stubs = 0;
            
            while (curr_stub_addr < stub_btm_addr) {
                SceLibraryStubHeader *stub_hdr = (SceLibraryStubHeader *)mips_get_ptr(cpu, curr_stub_addr);
                // Sanity check to avoid reading unmapped memory
                if (stub_hdr == NULL || stub_hdr->lib_name_ptr == 0) {
                    break;
                }
                
                uint32_t lib_name_vaddr = relocate_addr(stub_hdr->lib_name_ptr, load_bias);
                const char *lib_name = (const char *)mips_get_ptr(cpu, lib_name_vaddr);
                if (!lib_name) lib_name = "UnknownLibrary";
                
                uint32_t nid_table_vaddr = relocate_addr(stub_hdr->nid_table_ptr, load_bias);
                uint32_t *nid_table = (uint32_t *)mips_get_ptr(cpu, nid_table_vaddr);
                
                uint32_t stub_table_vaddr = relocate_addr(stub_hdr->stub_table_ptr, load_bias);
                
                // Safety Cap to prevent corrupt headers from writing all over RAM
                uint32_t num_stubs_to_patch = stub_hdr->num_funcs; // FIXED: Real number of imported stubs/funcs
                if (num_stubs_to_patch > 1024) {
                    num_stubs_to_patch = 1024;
                }
                
                if (nid_table != NULL && stub_table_vaddr != 0) {
                    for (uint32_t j = 0; j < num_stubs_to_patch; j++) {
                        uint32_t nid = nid_table[j];
                        uint32_t target_stub_vaddr = stub_table_vaddr + j * 8; // Each stub is exactly 8 bytes (jr $ra; nop)
                        
                        uint32_t syscall_code = 0;
                        const char *func_name = "unknown";
                        
                        // Map the real PSPSDK 32-bit crypt NID hashes into HLE mock Syscalls (GOW spec match)
                        if (strcmp(lib_name, "IoFileMgrForUser") == 0) {
                            if (nid == 0x109F504F || nid == 0x109F50BC) { syscall_code = 0x11111; func_name = "sceIoOpen"; }
                            else if (nid == 0x6A638D83) { syscall_code = 0x11112; func_name = "sceIoRead"; }
                            else if (nid == 0x27EB27B8) { syscall_code = 0x11116; func_name = "sceIoLseek"; }
                            else if (nid == 0x68963324) { syscall_code = 0x11117; func_name = "sceIoLseek32"; }
                            else if (nid == 0xA0B5A7C2) { syscall_code = 0x11113; func_name = "sceIoReadAsync"; }
                            else if (nid == 0x810C4BC3) { syscall_code = 0x11114; func_name = "sceIoClose"; }
                            else if (nid == 0x54F5FB11) { syscall_code = 0x11115; func_name = "sceIoDevctl"; }
                        } else if (strcmp(lib_name, "ThreadManForUser") == 0) {
                            if (nid == 0xD6D016D7 || nid == 0x3F53A0F3) { syscall_code = 0x22221; func_name = "sceKernelCreateSema"; }
                            else if (nid == 0x4E3A1105) { syscall_code = 0x22222; func_name = "sceKernelWaitSema"; }
                            else if (nid == 0xCEADEB85) { syscall_code = 0x22223; func_name = "sceKernelSignalSema"; }
                            else if (nid == 0x2C1184E6) { syscall_code = 0x22224; func_name = "sceKernelDelayThread"; }
                            else if (nid == 0xF6414A71 || nid == 0xD6DA4BA1 || nid == 0x446D8DE6) { syscall_code = 0x22225; func_name = "sceKernelCreateThread"; }
                            else if (nid == 0xF475845D) { syscall_code = 0x22226; func_name = "sceKernelStartThread"; }
                            else if (nid == 0xAA73C935 || nid == 0x8011F9B0) { syscall_code = 0x22227; func_name = "sceKernelExitThread"; }
                        } else if (strcmp(lib_name, "sceGe_Driver") == 0 || strcmp(lib_name, "sceGe") == 0) {
                            if (nid == 0xAB49E7EC || nid == 0xE47E40E4) { syscall_code = 0x33331; func_name = "sceGeListEnqueue"; }
                            else if (nid == 0x034C113E) { syscall_code = 0x33332; func_name = "sceGeListSync"; }
                        } else if (strcmp(lib_name, "sceAudio") == 0) {
                            if (nid == 0x5EC81C55) { syscall_code = 0x40001; func_name = "sceAudioChReserve"; }
                            else if (nid == 0x13F592BC) { syscall_code = 0x40002; func_name = "sceAudioChRelease"; }
                            else if (nid == 0x14074D7E || nid == 0x08DF58A5) { syscall_code = 0x40003; func_name = "sceAudioOutputPanned"; }
                        }
                        
                        if (syscall_code != 0) {
                            // Overwrite the original stub jr $ra with our custom syscall
                            mips_write32(cpu, target_stub_vaddr, (syscall_code << 6) | 0x0000000C);
                            // Overwrite delay slot with NOP (HLE handler performs direct $ra return virtualization)
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            
                            printf("  -> patched import: %s -> %s (NID: 0x%08X) -> custom syscall: 0x%X\n", lib_name, func_name, nid, syscall_code);
                            patch_count++;
                        } else {
                            // Fallback for unimplemented stubs to dynamic unique syscalls starting from 0x50000
                            uint32_t fallback_code = 0x50000 + fallback_stubs_count;
                            if (fallback_stubs_count < MAX_PATCHED_FALLBACKS) {
                                strncpy(fallback_stubs[fallback_stubs_count].lib_name, lib_name, 31);
                                fallback_stubs[fallback_stubs_count].nid = nid;
                                fallback_stubs[fallback_stubs_count].syscall_code = fallback_code;
                                fallback_stubs_count++;
                            }
                            mips_write32(cpu, target_stub_vaddr, (fallback_code << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            printf("  -> patched import (fallback): %s -> NID: 0x%08X -> fallback success (custom code 0x%X)\n", lib_name, nid, fallback_code);
                            patch_count++;
                        }
                        total_stubs++;
                    }
                }
                
                // Advance stub headers sequentially. FIXED: step reads 1 byte from struct_size (offset 8)
                uint32_t step = stub_hdr->struct_size;
                if (step < 5 || step > 32) step = 5;
                curr_stub_addr += step * 4;
            }
            printf("[HLE Linker] Resolved and patched %d / %d imported stubs successfully!\n", patch_count, total_stubs);
        } else {
            printf("[HLE Linker Error] SceModuleInfo structure is invalid or corrupt.\n");
        }
    } else {
        printf("[HLE Linker Error] SceModuleInfo structure could not be discovered or resolved! Skip linker patching.\n");
    }

    free(data);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("======================================================================\n");
    printf("     MIPS Allegrex AOT Emulator & GLES 3.2 Core - RK3326/R36S\n");
    printf("======================================================================\n");

    // 1. Allocate and Initialize CPU State
    MIPS_CPU cpu;
    int init_ret = mips_cpu_init(&cpu);
    if (init_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate contiguous memory blocks for CPU (Code: %d).\n", init_ret);
        return EXIT_FAILURE;
    }
    printf("[Init] Contiguous 64MB RAM, 2MB VRAM, and 16KB Scratchpad allocated successfully.\n");

    // 2. Initialize HLE OS Subsystems (including GE Graphics Engine)
    int hle_ret = mips_hle_init(&kernel);
    if (hle_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize HLE subsystem (Code: %d).\n", hle_ret);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }

    // 3. Initialize OpenGL ES 3.20 Graphics Subsystem
    MIPS_VideoSystem video;
    int video_ret = mips_video_init(&video);
    if (video_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize Video GLES subsystem (Code: %d).\n", video_ret);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    global_video_system = &video;
    kernel.video_system = &video;

    // 4. Initialize SDL2 Multi-channel Audio Subsystem
    MIPS_AudioSystem audio;
    int audio_ret = mips_audio_init(&audio);
    if (audio_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize Audio subsystem (Code: %d).\n", audio_ret);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    kernel.audio_system = &audio;

    // 5. Execute Isolated Diagnostics GLES 3.20 Draw Test
    printf("[Diagnostics] Running Isolated Direct GLES 3.20 Render Test first...\n");
    mips_video_draw_test_triangle(&video);
    printf("[Video Diagnostics] Standard hardcoded test triangle drawn successfully!\n");
    printf("[Diagnostics] Direct triangle should be visible on screen. Keeping open for 2s...\n");
    SDL_Delay(2000);

    // 6. Allocate and Initialize Dispatcher Lookup
    MIPS_Dispatcher dispatcher;
    int disp_ret = mips_dispatcher_init(&dispatcher);
    if (disp_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate dispatcher lookup table (Code: %d).\n", disp_ret);
        mips_audio_free(&audio);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    printf("[Init] Direct-mapped O(1) flat 128MB lookup table aligned to RAM.\n");

    // 7. Bind Fallback Interpreter
    dispatcher.fallback_interpreter = interpreter_step;
    printf("[Init] Fallback interpreter bound to dispatcher successfully.\n");

    // 8. Load Decrypted Game Executable
    uint32_t entry_point = 0x08804000;

    // We try fallbacks to decrypted files, and critically, the unencrypted BOOT.BIN of extracted UMDs!
    const char *fallback_paths[] = {
        "EBOOT.BIN",
        "BOOT.BIN",
        "umd0/PSP_GAME/SYSDIR/BOOT.BIN", // Unencrypted fallback from original game files
        "umd0/PSP_GAME/SYSDIR/EBOOT.BIN",
        "EBOOT.ELF"
    };
    int num_fallbacks = sizeof(fallback_paths) / sizeof(fallback_paths[0]);
    int load_success = 0;

    for (int i = 0; i < num_fallbacks; i++) {
        printf("[Boot] Attempting to load executable option %d: '%s'...\n", i + 1, fallback_paths[i]);
        if (load_eboot(&cpu, fallback_paths[i], &entry_point) == 0) {
            printf("[Boot SUCCESS] Mapped program memory space using '%s'.\n", fallback_paths[i]);
            load_success = 1;
            break;
        }
    }

    if (!load_success) {
        fprintf(stderr, "[Boot ERROR] No valid, decrypted executable found.\n");
        fprintf(stderr, "[Boot ERROR] Please place a decrypted EBOOT.BIN, EBOOT.ELF, or the original BOOT.BIN in this folder.\n");
        mips_dispatcher_free(&dispatcher);
        mips_audio_free(&audio);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }

    // 9. Auto-register Recompiled AOT Blocks (Dynamic registration)
    // Para depuracao profunda e 100% compativel com o Interpretador corrigido, podemos
    // forcá-lo a rodar no modo de Interpretador Puro compilando com -DFORCE_INTERPRETER!
    #ifdef FORCE_INTERPRETER
    printf("[AOT] FORCE_INTERPRETER active! System running in 100%% Fallback Interpreter mode.\n");
    #else
    printf("[AOT] Binding pre-compiled translation blocks from gow_core_aot.c...\n");
    aot_register_blocks(&dispatcher);
    #endif

    // 10. Start emulation loop
    // Set up standard initial hardware register contexts before the CPU boots
    cpu.gpr[29] = 0x0BFFF000; // $sp (Stack Pointer) safely allocated at the very top of user RAM
    cpu.gpr[31] = 0x08800100; // $ra (Return Address) points to our safe HALT trap
    cpu.gpr[28] = relocated_gp; // Set initial $gp (Global Pointer)

    // Insert HALT instruction (0x70000000) at 0x08800000 to cleanly catch exit if the module main function returns
    mips_write32(&cpu, 0x08800100, 0x70000000); // Write HALT trap at 0x08800100

    cpu.pc = entry_point;
    cpu.next_pc = entry_point;

    // God of War Anti-Tamper / Assert Bypass Patches
    // PC: 0x0880758C (Inst: 0x14800003 -> bne $a0, $zero, 3)
    // We overwrite it with an unconditional branch (0x10000003 -> beq $zero, $zero, 3)
    // to bypass the abort / BREAK call at 0x088056AC!
    if (mips_read32(&cpu, 0x0880758C) == 0x14800003) {
        mips_write32(&cpu, 0x0880758C, 0x10000003);
        printf("[HLE Patch] Applied God of War Anti-Tamper/Assert Bypass at 0x0880758C successfully!\n");
    }

    printf("[Emulator] Boot Sector successfully loaded. CPU Entry point initialized at PC: 0x%08X\n", cpu.pc);
    printf("[Emulator] Stack Pointer ($sp) initialized to: 0x%08X\n", cpu.gpr[29]);
    printf("[Emulator] Starting primary execution thread loop...\n\n");

    mips_dispatcher_execute(&dispatcher, &cpu);

    printf("\n======================================================================\n");
    printf("[Emulator Halt] Loop completed. Execution stats:\n");
    printf("  - Registers state: $v0 = %d, $a0 = %d\n", (int32_t)cpu.gpr[2], (int32_t)cpu.gpr[4]);
    printf("  - Elapsed Cycles simulated: %llu\n", (unsigned long long)cpu.cycles);
    printf("  - Exit requested flag: %d\n", (int)cpu.exit_requested);
    printf("  - Fault virtual address (BadVAddr): 0x%08X\n", cpu.bad_vaddr);
    
    printf("\n[Post-Mortem Trace] Last 120 instructions executed before Halt:\n");
    int start = trace_wrapped ? trace_index : 0;
    int count = trace_wrapped ? TRACE_SIZE : trace_index;
    if (count > 120) {
        start = (trace_index - 120 + TRACE_SIZE) % TRACE_SIZE;
        count = 120;
    }
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % TRACE_SIZE;
        printf("  [Trace %02d] PC: 0x%08X, Inst: 0x%08X | $v0: %d, $a0: %d, $sp: 0x%08X, $ra: 0x%08X\n",
               i, trace_buffer[idx].pc, trace_buffer[idx].inst,
               (int32_t)trace_buffer[idx].regs[2], (int32_t)trace_buffer[idx].regs[4],
               trace_buffer[idx].regs[29], trace_buffer[idx].regs[31]);
    }
    printf("======================================================================\n");

    // 11. Cleanup and release blocks
    mips_dispatcher_free(&dispatcher);
    mips_audio_free(&audio);
    mips_video_free(&video);
    mips_hle_free(&kernel);
    mips_cpu_free(&cpu);
    printf("[Cleanup] Contiguous virtual spaces released cleanly. Done.\n");

    return EXIT_SUCCESS;
}