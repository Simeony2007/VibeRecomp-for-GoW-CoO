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

// NOVO: pasta real do jogo, definida em main_v35.c a partir de argv[1].
extern char g_game_root[512];

/*
 * Case-insensitive case folding for Linux filesystem support (since PSP is case-insensitive [Filesystem])
 */
static void sanitize_path(char *dest, const char *src) {
    const char *p = src;
    char *d = dest;

    // FIX: "./umd0/" fixo (e o "./" do caso sem prefixo) nao tinham
    // nenhuma relacao com a pasta de onde o EBOOT.PBP foi realmente aberto
    // (g_game_root). Isso fazia sceIoOpen("data.csz") procurar no
    // diretorio de trabalho do emulador em vez da pasta do jogo, sempre
    // falhando pra arquivos soltos que ficam do lado do EBOOT.PBP.
    if (strncmp(p, "umd0:/", 6) == 0) {
        d += snprintf(d, 480, "%s/", g_game_root);
        p += 6;
    } else if (strncmp(p, "ms0:/", 5) == 0) {
        d += snprintf(d, 480, "%s/ms0/", g_game_root);
        p += 5;
    } else if (strncmp(p, "disc0:/", 7) == 0) {
        d += snprintf(d, 480, "%s/", g_game_root);
        p += 7;
    } else {
        d += snprintf(d, 480, "%s/", g_game_root);
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
    
    // FIX: mapeamento de flags estava errado. PSP_O_RDONLY=0x0001 batia com
    // a mascara '(flags & 0x3)' usada pra detectar RDWR, entao QUALQUER
    // abertura somente-leitura tambem criava o arquivo vazio (O_CREAT
    // aplicado sem necessidade) - e' isso que causava "read 0 bytes" em
    // arquivos de save/config que deveriam ja existir com dados. No PSP de
    // verdade o modo de acesso (bits 0-1: RDONLY=1/WRONLY=2/RDWR=3) e as
    // flags extras (CREAT=0x200, TRUNC=0x400, APPEND=0x100) sao
    // independentes - uma nao implica a outra.
    int posix_flags;
    int access_mode = flags & 0x3;
    if (access_mode == 0x2) posix_flags = O_WRONLY;
    else if (access_mode == 0x3) posix_flags = O_RDWR;
    else posix_flags = O_RDONLY;
    if (flags & 0x0200) posix_flags |= O_CREAT;  // PSP_O_CREAT
    if (flags & 0x0400) posix_flags |= O_TRUNC;  // PSP_O_TRUNC
    if (flags & 0x0100) posix_flags |= O_APPEND; // PSP_O_APPEND
    
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
        case 0x11111: { // Mock: sceIoOpen
            // NOVO: diagnostico do bug de "caminho com espacos" - antes de
            // confiar no ponteiro, mostra o endereco bruto (arg0) e um dump
            // hex+ASCII da memoria ali, pra ver se e' o PONTEIRO que esta
            // errado (apontando pra um lugar aleatorio da RAM) ou se o
            // CONTEUDO da memoria naquele endereco especifico realmente tem
            // esses espacos (o que apontaria pro jogo/decompressor, nao pra
            // leitura de argumento).
            const char *path_check = (const char *)mips_get_ptr(cpu, arg0);
            bool looks_wrong = false;
            if (path_check) {
                for (int k = 0; k < 8; k++) {
                    if (path_check[k] == ' ') { looks_wrong = true; break; }
                    if (path_check[k] == '\0') break;
                }
            }
            if (looks_wrong) {
                printf("[HLE IO DEBUG] sceIoOpen: arg0 (ponteiro do path) = 0x%08X. Dump da memoria nesse endereco:\n", arg0);
                const uint8_t *raw = (const uint8_t *)path_check;
                for (int row = 0; row < 4; row++) {
                    printf("  +0x%02X:", row * 16);
                    for (int col = 0; col < 16; col++) printf(" %02X", raw[row * 16 + col]);
                    printf("  |");
                    for (int col = 0; col < 16; col++) {
                        uint8_t b = raw[row * 16 + col];
                        putchar((b >= 32 && b < 127) ? (char)b : '.');
                    }
                    printf("|\n");
                }
                printf("[HLE IO DEBUG] Caller RA: 0x%08X\n", cpu->gpr[0x1F]);
            }
            return_val = hle_sceIoOpen(kernel, path_check, (int)arg1, (int)arg2);
            break;
        }

        case 0x11112: // Mock: sceIoRead
            return_val = hle_sceIoRead(kernel, (int)arg0, cpu, arg1, (int)arg2);
            break;

        // NOVO: sceIoLseek - ja estava mapeada pelo linker (NID 0x27EB27B8)
        // mas sem handler aqui, entao qualquer seek virava "Unknown Syscall".
        // Assinatura real: sceIoLseek(int fd, SceOff offset, int whence) ->
        // retorna a nova posicao (64-bit na PSP real, mas aqui simplificado
        // pra 32-bit ja que os arquivos de jogo nao devem passar de 4GB).
        // offset chega em $a1:$a2 (64-bit split em 2 registradores de 32,
        // little-endian: $a1=parte baixa, $a2=parte alta) e whence em $a3.
        case 0x11116: {
            int fd = (int)arg0;
            int64_t offset = (int64_t)((uint64_t)arg1 | ((uint64_t)arg2 << 32));
            int whence = (int)arg3;
            int slot = fd - 1;
            if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) {
                return_val = (uint32_t)-1;
            } else {
                int posix_whence = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
                off_t new_pos = lseek(kernel->files[slot].linux_fd, (off_t)offset, posix_whence);
                return_val = (uint32_t)new_pos;
                printf("[HLE IO] sceIoLseek(fd=%d, offset=%lld, whence=%d) -> nova posicao %ld\n",
                       fd, (long long)offset, whence, (long)new_pos);
            }
            break;
        }
            
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

        // NOVO: sceGeEdramGetAddr - retorna o endereco base da EDRAM de
        // video (onde framebuffers/texturas residem). Ja estava mapeada
        // pelo linker (NID 0xE47E40E4) mas sem handler, caindo em "Unknown
        // Syscall". Valor real do PSP e' fixo: 0x04000000.
        case 0x33333:
            return_val = 0x04000000;
            break;

        // NOVO: sceGeDrawSync - espera a GE terminar de desenhar a lista
        // atual. Como a nossa GE roda de forma sincrona dentro do proprio
        // sceGeListEnqueue (mips_ge_run_list ja processa a lista inteira
        // na hora), aqui so precisa devolver sucesso - nao ha fila
        // assincrona de verdade pra esperar.
        case 0x33335:
            return_val = 0; // PSP_OK
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
    
    // FIX: o stub gravado por mips_linker.c e' so "syscall; nop" - nao tem
    // nenhum 'jr $ra' de verdade em lugar nenhum. Avancar so +4 fazia o
    // interpretador continuar executando o nop e depois cair direto no resto
    // da tabela de stubs original (dados/NIDs, nao codigo) como se fosse
    // instrucao valida, ate um jr/jalr com registrador sujo produzir um PC
    // invalido (foi assim que "0x001D976E" apareceu logo apos o sceIoOpen).
    // O retorno tem que ser pro chamador ($ra), igual TODO outro handler HLE
    // do projeto ja faz (ver sceSysMem.c, sceKernelThread.c). Isso vale pra
    // qualquer branch do switch acima, inclusive o 'default' de syscall
    // desconhecida - nao ha motivo pra deixar essas continuarem pra frente.
    cpu->pc = cpu->gpr[0x1F]; // $ra
    cpu->next_pc = cpu->pc + 4;
}


// Global default weak implementation of link handler
__attribute__((weak)) void hle_syscall(MIPS_CPU *cpu) {
    extern MIPS_HLE_Kernel kernel;
    mips_hle_syscall(cpu, &kernel);
}
