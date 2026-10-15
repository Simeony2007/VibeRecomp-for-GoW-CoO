/*
 * MIPS Allegrex Emulator - Main Core (v42)
 * Integrated with modular thread management (sceKernelThread)
 */

#include "mips_reloc.h"
#include "mips_cpu.h"
#include "mips_dispatcher.h"
#include "mips_interpreter.h"
#include "mips_hle.h"
#include "mips_video.h"
#include "mips_audio.h"
#include "sceKernelThread.h"
#include "sceSysMem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <dirent.h>

// Global linkage declarations for linker variables
extern PatchedFallbackStub fallback_stubs[MAX_PATCHED_FALLBACKS];
extern int fallback_stubs_count;
extern uint32_t relocated_gp;

// NOVO: pasta real onde o jogo vive (derivada de argv[1], ex: "./umd0CH").
// FIX: sanitize_path()/custom_sanitize_path() estavam com "./umd0/" e "./"
// fixos no codigo, sem nenhuma relacao com a pasta de onde o EBOOT.PBP foi
// realmente aberto - por isso arquivos soltos do jogo (data.csz etc), que
// ficam do LADO do EBOOT.PBP, nunca eram encontrados: o sceIoOpen procurava
// no diretorio de trabalho do emulador, nao na pasta do jogo.
char g_game_root[512] = ".";
uint32_t g_module_entry_point = 0;
// NOVO: contador de "power locks" ativos (sceKernelPowerLock incrementa,
// sceKernelPowerUnlock decrementa). Nao simulamos suspensao de verdade,
// mas contar certinho evita destravar mais vezes do que travou (o que na
// PSP real e' um erro de nesting) e deixa log honesto de quantos locks
// ainda estao pendentes.
int g_power_lock_count = 0;

// NOVO: guarda o callback ID registrado via sceKernelRegisterExitCallback,
// e uma pequena lista de callbacks registrados via scePowerRegisterCallback.
// Ainda nao disparamos essas notificacoes automaticamente (nao ha um evento
// real de "usuario apertou Home" ou "bateria mudou" neste emulador), mas
// agora pelo menos guardamos o estado corretamente em vez de fingir sucesso
// sem fazer nada - isso evita erros tipo "callback nao encontrado" se/quando
// o resto do jogo tentar consultar ou desregistrar esse callback depois, e
// deixa o terreno pronto para disparar essas notificacoes de verdade no
// futuro (ex: simular sceKernelExitGame chamando g_exit_callback_id).
#define MAX_POWER_CALLBACKS 4
int g_exit_callback_id = -1;
int g_power_callback_ids[MAX_POWER_CALLBACKS] = { -1, -1, -1, -1 };
// FIX (rodada 2): 'g_binary_segment_end' JA tinha uma definicao de verdade
// em outro arquivo do projeto (provavelmente mips_linker.c, que calcula o
// fim do segmento carregado) - o erro original so reclamava de
// 'g_binary_segment_start', que essa sim nao existia em lugar nenhum. Aqui
// ficamos so com o extern (apontando pra definicao que ja existe) e
// definimos de verdade apenas 'g_binary_segment_start', que era o que
// realmente faltava.
extern uint32_t g_binary_segment_end;
uint32_t g_binary_segment_start = 0x08800000;

// Weak fallback definition for EBOOT loader to ensure linkability
__attribute__ ((weak)) int load_eboot(MIPS_CPU *cpu, const char *path, uint32_t *entry_point) {
    (void)cpu; (void)path; (void)entry_point;
    printf("[Loader Warning] Weak load_eboot fallback executed (mips_reloc.c missing at link time).\n");
    return -1;
}

typedef struct {
    char magic[0x04];          // Magic bytes: "\0PBP" (0x00 0x50 0x42 0x50)
    uint32_t version;          // Versao do formato PBP (geralmente 0x00010001)
    uint32_t param_offset;     // Offset para param.sfo (metadados)
    uint32_t icon0_offset;     // Offset para icon0.png
    uint32_t icon1_offset;     // Offset para icon1.pmf
    uint32_t pic0_offset;      // Offset para pic0.png
    uint32_t pic1_offset;      // Offset para pic1.pmf
    uint32_t snd0_offset;      // Offset para snd0.at3
    uint32_t data_psp_offset;  // Offset para data.psp (O executavel ELF MIPS real!)
    uint32_t data_psar_offset; // Offset para data.psar (Sinaliza o fim do executavel)
} __attribute__ ((packed)) PBPHeader;

// Instancia Global do Kernel
MIPS_HLE_Kernel kernel;
void *global_video_system = NULL;

// Declaracao fraca para blocos AOT compilados
__attribute__ ((weak)) void aot_register_blocks(MIPS_Dispatcher *disp) {
    (void)disp;
    printf("[AOT] Nenhum bloco Ahead-of-Time encontrado (gow_core_aot.c nao compilado).\n");
    printf("[AOT] O emulador operara em modo 100%% Fallback Interpreter.\n");
}

/*
*  Funcao Extratora de Executaveis de dentro de Containers PBP
*/
int extract_pbp_data_psp(const char *pbp_path, const char *out_bin_path) {
    printf("[PBP Extractor] Abrindo container PBP: %s\n", pbp_path);
    FILE *f = fopen(pbp_path, "rb");
    if (!f) {
        printf("[PBP Extractor Erro] Falha ao abrir: %s\n", pbp_path);
        return -1;
    }
    PBPHeader header;
    if (fread(&header, 1, sizeof(PBPHeader), f) != sizeof(PBPHeader)) {
        printf("[PBP Extractor Erro] Falha ao ler cabecalho PBP.\n");
        fclose(f);
        return -2;
    }
    // Validacao do Magic do PSP
    if (memcmp(header.magic, "\x00PBP", 4) != 0) {
        printf("[PBP Extractor Erro] Arquivo nao e um container PBP valido!\n");
        fclose(f);
        return -3;
    }
    uint32_t offset_elf = header.data_psp_offset;
    uint32_t offset_psar = header.data_psar_offset;
    uint32_t elf_size = offset_psar - offset_elf;
    if (elf_size == 0) {
        printf("[PBP Extractor Erro] Secao data.psp possui tamanho zero.\n");
        fclose(f);
        return -4;
    }
    printf("[PBP Extractor] data.psp (ELF MIPS) encontrado no offset 0x%X (Tamanho: %u bytes).\n", offset_elf, elf_size);
    if (fseek(f, offset_elf, SEEK_SET) != 0) {
        printf("[PBP Extractor Erro] Falha de posicionamento (fseek) no container.\n");
        fclose(f);
        return -5;
    }
    uint8_t *elf_buffer = (uint8_t *)malloc(elf_size);
    if (!elf_buffer) {
        printf("[PBP Extractor Erro] Falha ao alocar memoria para extracao (%u bytes).\n", elf_size);
        fclose(f);
        return -6;
    }
    if (fread(elf_buffer, 1, elf_size, f) != elf_size) {
        printf("[PBP Extractor Erro] Falha ao ler binario encapsulado completo.\n");
        free(elf_buffer);
        fclose(f);
        return -7;
    }
    fclose(f);
    FILE *out = fopen(out_bin_path, "wb");
    if (!out) {
        printf("[PBP Extractor Erro] Falha ao criar arquivo de saida: %s\n", out_bin_path);
        free(elf_buffer);
        return -8;
    }
    if (fwrite(elf_buffer, 1, elf_size, out) != elf_size) {
        printf("[PBP Extractor Erro] Falha de escrita no disco.\n");
        fclose(out);
        free(elf_buffer);
        return -9;
    }
    fclose(out);
    free(elf_buffer);
    printf("[PBP Extractor SUCESSO] Executavel extraido com exito em '%s'!\n", out_bin_path);
    return 0;
}

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

typedef struct {
    DIR *dir;
    char psp_path[512];
    bool is_active;
} PSPDirSlot;

#define MAX_DIRS 8
static PSPDirSlot g_dir_slots[MAX_DIRS] = {0};

#define TRACE_SIZE 10
typedef struct {
    uint32_t pc;
    uint32_t inst;
    uint32_t regs[0x20];
} TraceEntry;

static TraceEntry trace_buffer[TRACE_SIZE];
static int trace_index = 0;
static bool trace_wrapped = false;
static uint32_t emu_thread_entry __attribute__ ((unused)) = 0;
static int32_t file_async_results[0x20] = {0};
static uint32_t vblank_counter = 0;

typedef struct {
    int id;
    char name[64];
    char path[512];
    uint32_t entry_point;
    uint32_t load_bias;
    bool is_active;
    bool is_started;
} PSPModule;

#define MAX_LOADED_MODULES 8
static PSPModule loaded_modules[MAX_LOADED_MODULES] = {0};
static int next_module_id = 0x200;
uint32_t g_next_module_load_addr = 0;

// HLECallback and g_callbacks moved to sceKernelThread.h / sceKernelThread.c

typedef struct {
    uint32_t id;
    char name[32];
    int32_t current_count;
    int32_t max_count;
    bool is_active;
} HLESema;

#define MAX_SEMAS 32
static HLESema g_semas[MAX_SEMAS];
static uint32_t g_next_sema_id = 100000;

// HLE_MemBlock structure and memory table moved to sceSysMem.c / sceSysMem.h

typedef struct {
    int slot_id;
    int cbid;
    bool is_active;
} HLE_PowerCallback;

#define MAX_POWER_CALLBACKS 16
static HLE_PowerCallback g_power_callbacks[MAX_POWER_CALLBACKS] = {0};

typedef struct {
    uint32_t time_stamp;
    uint32_t buttons;
    uint8_t lx;
    uint8_t ly;
    uint8_t reserved[6];
} PSP_CtrlData;

static PSP_CtrlData g_ctrl_state = {
    .time_stamp = 0,
    .buttons = 0,
    .lx = 128,
    .ly = 128
};
static uint32_t g_ctrl_sampling_cycle = 0;
static uint32_t g_ctrl_sampling_mode = 0;

typedef struct {
    uint32_t id;
    uint32_t list_addr;
    uint32_t stall_addr;
    int cb_id;
    bool is_active;
    bool completed;
} HLE_GEList;

#define MAX_GE_LISTS 16
static HLE_GEList g_ge_lists[MAX_GE_LISTS] = {0};
static uint32_t g_next_ge_list_id = 0x35000001;

typedef struct {
    uint32_t topaddr;
    uint32_t stride;
    uint32_t format;
    uint32_t sync;
    uint32_t mode;
    uint32_t disp_width;
    uint32_t disp_height;
    bool configured;
} HLE_DisplayState;

static HLE_DisplayState g_display = {
    .topaddr = 0x04000000,
    .format = 0,
    .configured = false
};

typedef struct {
    int channel_id;
    int sample_count;
    int format;
    bool is_reserved;
} HLE_AudioChannel;

#define MAX_AUDIO_CHANNELS 8
static HLE_AudioChannel g_audio_channels[MAX_AUDIO_CHANNELS] = {0};

static void custom_sanitize_path(char *dest, const char *src) {
    const char *p = src;
    char *d = dest;

    // FIX: "./umd0/" fixo nao tinha nenhuma relacao com a pasta real do
    // jogo (g_game_root, derivada de argv[1] - ex: "./umd0CH"). Um caminho
    // sem prefixo tambem deve resolver pra dentro da pasta do jogo, e nao
    // pro diretorio de trabalho do emulador ("./"), senao arquivos soltos
    // que ficam do lado do EBOOT.PBP (ex: data.csz) nunca sao encontrados.
    if (strncmp(p, "umd0:/", 6) == 0 || strncmp(p, "disc0:/", 7) == 0) {
        int skip = (strncmp(p, "umd0:/", 6) == 0) ? 6 : 7;
        d += snprintf(d, 480, "%s/", g_game_root);
        p += skip;
    } else if (strncmp(p, "ms0:/", 5) == 0) {
        d += snprintf(d, 480, "%s/ms0/", g_game_root);
        p += 5;
    } else {
        d += snprintf(d, 480, "%s/", g_game_root);
    }
    while (*p) {
        if (*p == '\\') {
            *d = '/';
        } else {
            *d = *p;
        }
        d++;
        p++;
    }
    *d = '\0';
}

static int64_t hle_sceIoLseek(MIPS_HLE_Kernel *kernel_ptr, int fd, int64_t offset, int whence) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel_ptr->files[slot].is_active) return -1;
    off_t new_pos = lseek(kernel_ptr->files[slot].linux_fd, offset, whence);
    return (int64_t)new_pos;
}

static int hle_sceIoLseek32(MIPS_HLE_Kernel *kernel_ptr, int fd, int offset, int whence) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel_ptr->files[slot].is_active) return -1;
    off_t new_pos = lseek(kernel_ptr->files[slot].linux_fd, offset, whence);
    return (int)new_pos;
}

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
    strcat(resolved, p);
}

void hle_syscall(MIPS_CPU *cpu) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;

    // --- SysMemUserForUser Modular Syscalls ---
    if (syscall_id == 0x90001) { hle_sceKernelTotalFreeMemSize(cpu); return; }
    if (syscall_id == 0x90002) { hle_sceKernelMaxFreeMemSize(cpu); return; }
    if (syscall_id == 0x90003) { hle_sceKernelAllocPartitionMemory(cpu); return; }
    if (syscall_id == 0x90004) { hle_sceKernelGetBlockHeadAddr(cpu); return; }
    if (syscall_id == 0x90005) { hle_sceKernelFreePartitionMemory(cpu); return; }
    if (syscall_id == 0x11117) { // sceIoLseek32
        uint32_t fd = cpu->gpr[0x04];
        int32_t offset = (int32_t)cpu->gpr[0x05];
        int whence = (int)cpu->gpr[0x06];
        int32_t result = hle_sceIoLseek32(&kernel, fd, offset, whence);
        cpu->gpr[0x02] = (uint32_t)result;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x11112) { // sceIoRead
        uint32_t fd = cpu->gpr[0x04];
        uint32_t buf_ptr = cpu->gpr[0x05];
        uint32_t size = cpu->gpr[0x06];
        int slot = fd - 1;
        int32_t bytes_read = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *dst = mips_get_ptr(cpu, buf_ptr);
            if (dst != NULL) {
                bytes_read = read(kernel.files[slot].linux_fd, dst, size);
                printf("[HLE IO] sceIoRead(fd=%u, size=%u): read %d bytes successfully to 0x%08X\n", fd, size, bytes_read, buf_ptr);
            }
        }
        cpu->gpr[0x02] = (uint32_t)bytes_read;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x1111C) { // sceIoWrite
        uint32_t fd = cpu->gpr[0x04];
        uint32_t buf_ptr = cpu->gpr[0x05];
        uint32_t size = cpu->gpr[0x06];
        int slot = fd - 1;
        int32_t bytes_written = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *src = mips_get_ptr(cpu, buf_ptr);
            if (src != NULL) {
                bytes_written = write(kernel.files[slot].linux_fd, src, size);
                printf("[HLE IO] sceIoWrite(fd=%u, size=%u): wrote %d bytes successfully from 0x%08X\n", fd, size, bytes_written, buf_ptr);
            }
        }
        cpu->gpr[0x02] = (uint32_t)bytes_written;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x1111D) { // sceIoWriteAsync
        uint32_t fd = cpu->gpr[0x04];
        uint32_t buf_ptr = cpu->gpr[0x05];
        uint32_t size = cpu->gpr[0x06];
        int slot = fd - 1;
        int32_t bytes_written = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *src = mips_get_ptr(cpu, buf_ptr);
            if (src != NULL) {
                bytes_written = write(kernel.files[slot].linux_fd, src, size);
                file_async_results[slot] = bytes_written;
            }
        }
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x11113) { // sceIoReadAsync
        uint32_t fd = cpu->gpr[0x04];
        uint32_t buf_ptr = cpu->gpr[0x05];
        uint32_t size = cpu->gpr[0x06];
        int slot = fd - 1;
        int32_t bytes_read = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *dst = mips_get_ptr(cpu, buf_ptr);
            if (dst != NULL) {
                bytes_read = read(kernel.files[slot].linux_fd, dst, size);
                file_async_results[slot] = bytes_read;
            }
        }
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x11114) { // sceIoClose
        uint32_t fd = cpu->gpr[0x04];
        int slot = fd - 1;
        int ret = -1;
        pthread_mutex_lock(&kernel.kernel_mutex);
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            ret = close(kernel.files[slot].linux_fd);
            kernel.files[slot].is_active = false;
        }
        pthread_mutex_unlock(&kernel.kernel_mutex);
        cpu->gpr[0x02] = (uint32_t)ret;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x1111E) { // sceIoGetstat
        uint32_t file_ptr = cpu->gpr[0x04];
        uint32_t stat_ptr = cpu->gpr[0x05];
        const char *file_path = (const char *)mips_get_ptr(cpu, file_ptr);
        int32_t ret_val = -1;

        if (file_path != NULL && stat_ptr != 0) {
            char host_path[512];
            custom_sanitize_path(host_path, file_path);
            char real_host_path[512];
            resolve_case_insensitive(real_host_path, host_path);

            struct stat st;
            if (stat(real_host_path, &st) == 0) {
                uint32_t psp_mode = S_ISDIR(st.st_mode) ? (0x0002 | 0755) : (0x0010 | 0644);
                int64_t psp_size = st.st_size;

                mips_write32(cpu, stat_ptr, psp_mode);
                mips_write32(cpu, stat_ptr + 4, 0);
                mips_write32(cpu, stat_ptr + 8, (uint32_t)(psp_size & 0xFFFFFFFF));
                mips_write32(cpu, stat_ptr + 12, (uint32_t)(psp_size >> 32));

                for (int t = 0; t < 3; t++) {
                    uint32_t t_offset = stat_ptr + 16 + t * 16;
                    mips_write16(cpu, t_offset, 2026);
                    mips_write16(cpu, t_offset + 2, 9);
                    mips_write16(cpu, t_offset + 4, 8);
                    mips_write16(cpu, t_offset + 6, 12);
                }
                ret_val = 0;
            }
        }
        cpu->gpr[0x02] = (uint32_t)ret_val;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id == 0x22224) { hle_sceKernelDelayThread(cpu); return; }
    if (syscall_id == 0x22225) { hle_sceKernelCreateThread(cpu); return; }
    if (syscall_id == 0x22226) { hle_sceKernelStartThread(cpu); return; }
    if (syscall_id == 0x22227) { hle_sceKernelExitThread(cpu); return; }

    if (syscall_id == 0x22228) { hle_sceKernelCheckCallback(cpu); return; }
    if (syscall_id == 0x22229) { hle_sceKernelDeleteCallback(cpu); return; }

    // FIX: sceDisplay* (0x60001-0x60003) e sceCtrl* (0x70001-0x70003) sao
    // >= 0x50000, entao antes caiam direto no bloco generico de fallback
    // logo abaixo. Nesse bloco 'idx = syscall_id - 0x50000' calculava um
    // indice gigante (ex: 0x10001 pra 0x60001), sempre maior que
    // fallback_stubs_count -> o 'if (idx < fallback_stubs_count)' falhava
    // e a funcao retornava 0 SEM NENHUM PRINT, silenciosamente. Era
    // exatamente isso que fazia o jogo "parar sem erro nenhum": o loop de
    // frame (SetFrameBuf -> WaitVblank -> ReadCtrl -> repete) continuava
    // girando pra sempre, so que 100% mudo, sem nunca configurar
    // framebuffer/vblank/controle de verdade.
    if (syscall_id == 0x60001) { // sceDisplaySetMode(mode, width, height)
        g_display.mode = cpu->gpr[0x04];
        g_display.disp_width = cpu->gpr[0x05];
        g_display.disp_height = cpu->gpr[0x06];
        printf("[HLE Display] sceDisplaySetMode(mode=%u, w=%u, h=%u)\n",
               g_display.mode, g_display.disp_width, g_display.disp_height);
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60002) { // sceDisplaySetFrameBuf(topaddr, stride, pixelformat, sync)
        g_display.topaddr = cpu->gpr[0x04];
        g_display.stride  = cpu->gpr[0x05];
        g_display.format  = cpu->gpr[0x06];
        g_display.sync    = cpu->gpr[0x07];
        g_display.configured = true;
        printf("[HLE Display] sceDisplaySetFrameBuf(topaddr=0x%08X, stride=%u, format=%u, sync=%u)\n",
               g_display.topaddr, g_display.stride, g_display.format, g_display.sync);
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60003) { // sceDisplayWaitVblankStart()
        vblank_counter++;
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x70001 || syscall_id == 0x70002) { // sceCtrlReadBufferPositive / sceCtrlPeekBufferPositive
        uint32_t pad_ptr = cpu->gpr[0x04];
        void *dst = mips_get_ptr(cpu, pad_ptr);
        if (dst != NULL) {
            memcpy(dst, &g_ctrl_state, sizeof(PSP_CtrlData));
        }
        cpu->gpr[0x02] = 1; // numero de amostras lidas
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x70003) { // sceCtrlSetSamplingCycle(cycle)
        g_ctrl_sampling_cycle = cpu->gpr[0x04];
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (syscall_id >= 0x60000 && syscall_id < 0x70000) {
        printf("[HLE BUG/DEBUG] Display syscall 0x%05X at PC=0x%08X RA=0x%08X\n",
           syscall_id, cpu->pc, cpu->gpr[31]);
    }

    if (syscall_id >= 0x70000 && syscall_id < 0x80000) {
        printf("[HLE BUG/DEBUG] Ctrl syscall 0x%05X at PC=0x%08X RA=0x%08X\n",
            syscall_id, cpu->pc, cpu->gpr[31]);
    }

    // Intercept our custom fallback syscall
    if (syscall_id >= 0x50000) {
        int idx = syscall_id - 0x50000;
        uint32_t ret_val = 0;
        if (idx >= 0 && idx < fallback_stubs_count) {
            uint32_t nid = fallback_stubs[idx].nid;

            if (nid == 0x82BC5777) { hle_sceKernelGetThreadCurrentPriority(cpu); return; }
            else if (nid == 0x293B45B8) { hle_sceKernelGetThreadState(cpu); return; }
            else if (nid == 0x71BC9871) { hle_sceKernelChangeThreadPriority(cpu); return; }
            else if (nid == 0x9FA03CD3) { hle_sceKernelDeleteThread(cpu); return; }
            else if (nid == 0x82826F70) { hle_sceKernelSleepThreadCB(cpu); return; }  // sceKernelSleepThread
            else if (nid == 0xC07BB470) { hle_sceKernelSleepThreadCB(cpu); return; }  // sceKernelSleepThreadCB (NID distinto, mesma logica)
            else if (nid == 0xD979E9BF) { hle_sceKernelWaitThreadEndCB(cpu); return; } // sceKernelWaitThreadEndCB
            else if (nid == 0xE81CAF8F) { hle_sceKernelCreateCallback(cpu); return; }
            else if (nid == 0x68DA9E36) { hle_sceKernelCheckCallback(cpu); return; }
            else if (nid == 0xEDBA5844) { hle_sceKernelDeleteCallback(cpu); return; }
            // NOVO: sceKernelRegisterExitCallback (LoadExecForUser). So
            // guarda o cbid recebido em $a0 - e o callback que deveria
            // rodar quando o jogo pede pra sair (botao Home / sceKernelExitGame).
            else if (nid == 0x4AC57943) {
                g_exit_callback_id = (int)cpu->gpr[0x04];
                printf("[HLE LoadExec] sceKernelRegisterExitCallback: registrado callback ID %d como callback de saida.\n",
                       g_exit_callback_id);
                cpu->gpr[0x02] = 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            // NOVO: scePowerRegisterCallback(int zero, int cbid) - guarda o
            // cbid numa lista pequena de callbacks de energia. FIX: a
            // assinatura real tem 2 parametros (o primeiro e' so um "zero"
            // reservado) - o cbid de verdade vem em $a1, nao $a0. Log
            // anterior mostrou "callback ID -1 registrado", confirmando que
            // estavamos lendo o registrador errado.
            else if (nid == 0x04B7766E) {
                int cbid = (int)cpu->gpr[0x05];
                int slot = -1;
                for (int i = 0; i < MAX_POWER_CALLBACKS; i++) {
                    if (g_power_callback_ids[i] == -1) { slot = i; break; }
                }
                if (slot != -1) {
                    g_power_callback_ids[slot] = cbid;
                    printf("[HLE Power] scePowerRegisterCallback: callback ID %d registrado (slot %d).\n", cbid, slot);
                    cpu->gpr[0x02] = 0;
                } else {
                    printf("[HLE Power Error] scePowerRegisterCallback: sem slots livres pra callback ID %d.\n", cbid);
                    cpu->gpr[0x02] = (uint32_t)-1;
                }
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            // NOVO: scePowerUnregisterCallback(int cbid) - remove da lista.
            else if (nid == 0xDFA8BAF8) {
                int cbid = (int)cpu->gpr[0x04];
                bool found = false;
                for (int i = 0; i < MAX_POWER_CALLBACKS; i++) {
                    if (g_power_callback_ids[i] == cbid) {
                        g_power_callback_ids[i] = -1;
                        found = true;
                        break;
                    }
                }
                printf("[HLE Power] scePowerUnregisterCallback: callback ID %d %s.\n",
                       cbid, found ? "removido" : "nao encontrado (ja tinha sido removido ou nunca foi registrado)");
                cpu->gpr[0x02] = found ? 0 : (uint32_t)-1;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            // NOVO: sceKernelPowerLock / sceKernelPowerUnlock (sceSuspendForUser).
            // Impedem/liberam a suspensao do sistema enquanto o jogo esta
            // fazendo algo critico (ex: gravando save). Este emulador nao
            // simula suspensao de verdade, entao o que importa e devolver
            // sucesso e manter a contagem de locks coerente (pra detectar
            // unlock sem lock correspondente, que seria um bug do proprio
            // jogo/SDK).
            else if (nid == 0xEADB1BD7) { // sceKernelPowerLock
                g_power_lock_count++;
                printf("[HLE Power] sceKernelPowerLock(tipo=%d) -> lock adquirido (locks ativos: %d).\n",
                       (int)cpu->gpr[0x04], g_power_lock_count);
                cpu->gpr[0x02] = 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            else if (nid == 0x3AEE7261) { // sceKernelPowerUnlock
                if (g_power_lock_count > 0) {
                    g_power_lock_count--;
                    printf("[HLE Power] sceKernelPowerUnlock(tipo=%d) -> lock liberado (locks ativos: %d).\n",
                           (int)cpu->gpr[0x04], g_power_lock_count);
                    cpu->gpr[0x02] = 0;
                } else {
                    printf("[HLE Power Warning] sceKernelPowerUnlock(tipo=%d) chamado sem lock correspondente ativo!\n",
                           (int)cpu->gpr[0x04]);
                    cpu->gpr[0x02] = (uint32_t)-1;
                }
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            else {
                printf("[HLE Fallback] Unimplemented API called: %s -> NID: 0x%08X at PC: 0x%08X (returns 0)\n",
                       fallback_stubs[idx].lib_name, nid, cpu->pc);
            }
        }
        cpu->gpr[0x02] = ret_val;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    uint32_t old_pc = cpu->pc;
    uint32_t old_ra = cpu->gpr[0x1F];
    mips_hle_syscall(cpu, &kernel);
    if (cpu->pc == old_pc) {
        cpu->pc = old_ra;
        cpu->next_pc = cpu->pc + 4;
    }
}

extern void mips_interpreter_step(MIPS_CPU *cpu);

void interpreter_step(MIPS_CPU *cpu) {
    uint32_t pc_val = cpu->pc;

    if (pc_val >= 0x08800000 && pc_val < 0x08800034) {
        printf(" ====================================================================== \n");
        printf("[HLE NULL CALLBACK TRAP] Thread '%s' (ID: %d) tentou executar ponteiro de callback nulo/relocado no PC: 0x%08X! \n",
               (current_thread_id >= 0 && current_thread_id < MAX_THREADS) ? threads[current_thread_id].name : "unknown",
               current_thread_id, pc_val);
        printf("  -> Caller  (GPR[31]): 0x%08X, SP: 0x%08X, V0: 0x%08X, A0: 0x%08X \n",
               cpu->gpr[31], cpu->gpr[29], cpu->gpr[2], cpu->gpr[4]);
        printf("  -> Executando retorno virtual seguro 'jr $ra' para o PC 0x%08X para manter a thread ativa... \n", cpu->gpr[31]);
        printf("======================================================================  \n");

        cpu->pc = cpu->gpr[31];
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (pc_val == CB_RETURN_HACK_ADDR) {
        uint32_t sp = cpu->gpr[29];
        uint32_t saved_pc = mips_read32(cpu, sp + 0);
        uint32_t saved_ra = mips_read32(cpu, sp + 31 * 4);

        for (int r = 4; r <= 25; r++) {
            cpu->gpr[r] = mips_read32(cpu, sp + (r * 4));
        }
        cpu->gpr[31] = saved_ra;
        cpu->gpr[29] += 128;

        printf("[HLE ThreadMan] Callback return trampoline executed! Restored $ra=0x%08X, returning to PC=0x%08X\n",
               saved_ra, saved_pc ? saved_pc : saved_ra);

        cpu->pc = saved_pc ? saved_pc : saved_ra;
        cpu->next_pc = cpu->pc + 4;
        return;
    }

    if (pc_val == 0x08000100) {
        // Guarda: só processa o trap se há uma thread realmente RUNNING.
        // Sem este check, quando run_scheduler retorna sem achar READY e o
        // PC não é alterado, o interpretador chama interpreter_step de novo,
        // o trap dispara outra vez para uma thread já FREE → loop infinito.
        bool is_running = false;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id && threads[i].state == THREAD_STATE_RUNNING) {
                is_running = true;
                break;
            }
        }
        if (!is_running) {
            // Nenhuma thread ativa — avança o PC para o idle address e sai
            cpu->pc      = 0x08000300;
            cpu->next_pc = 0x08000304;
            return;
        }
        printf("[HLE ThreadMan] Return address trap (0x08000100) triggered for Thread ID %d. Thread completed execution.\n", current_thread_id);
        run_scheduler(cpu, THREAD_STATE_FREE);
        return;
    }

    mips_interpreter_step(cpu);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("======================================================================\n");
    printf("     MIPS Allegrex Homebrew & PBP Testing Main - RK3326/R36S\n");
    printf("======================================================================\n");

    const char *target_path = "umd0/PSP_GAME/SYSDIR/EBOOT.BIN";
    if (argc >= 2) {
        target_path = argv[1];
    }

    // NOVO: extrai a pasta de target_path pra usar como raiz do sistema de
    // arquivos virtual (ver comentario na declaracao de g_game_root).
    {
        const char *last_slash = strrchr(target_path, '/');
        if (last_slash != NULL) {
            size_t dir_len = (size_t)(last_slash - target_path);
            if (dir_len >= sizeof(g_game_root)) dir_len = sizeof(g_game_root) - 1;
            memcpy(g_game_root, target_path, dir_len);
            g_game_root[dir_len] = '\0';
        } else {
            strcpy(g_game_root, ".");
        }
        printf("[Boot] Raiz de dados do jogo definida como: '%s'\n", g_game_root);
    }

    char extracted_path[0x100];
    const char *load_path = target_path;
    FILE *check_f = fopen(target_path, "rb");
    if (!check_f) {
        fprintf(stderr, "[ERRO] Nao foi possivel abrir o arquivo indicado: %s\n", target_path);
        return EXIT_FAILURE;
    }

    char magic[0x04];
    size_t read_bytes = fread(magic, 1, 4, check_f);
    fclose(check_f);

    if (read_bytes == 4 && memcmp(magic, "\x00PBP", 4) == 0) {
        printf("[Boot] Container .PBP detectado! Iniciando extracao automatica...\n");
        snprintf(extracted_path, sizeof(extracted_path), "extracted_eboot.bin");
        if (extract_pbp_data_psp(target_path, extracted_path) != 0) {
            fprintf(stderr, "[Boot ERRO] Falha ao extrair executavel encapsulado do PBP.\n");
            return EXIT_FAILURE;
        }
        load_path = extracted_path;
    }

    MIPS_CPU cpu;
    int init_ret = mips_cpu_init(&cpu);
    if (init_ret != 0) return EXIT_FAILURE;

    mips_hle_init(&kernel);

    MIPS_Dispatcher dispatcher;
    mips_dispatcher_init(&dispatcher);
    dispatcher.fallback_interpreter = interpreter_step;

    uint32_t entry_point = 0x08804000;
    if (load_eboot(&cpu, load_path, &entry_point) != 0) {
        fprintf(stderr, "[Boot ERRO] Falha ao carregar as stubs do executavel.\n");
        mips_dispatcher_free(&dispatcher);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    // Exposto globalmente para sceKernelThread.c: alguns binarios geram um
    // ponteiro de thread invalido (entry_pc == load_bias, ou seja, aponta
    // pro offset 0 do segmento, que nunca e codigo de funcao valido) porque
    // o simbolo original ja veio gravado como 0 no ELF (referencia
    // weak/nao-resolvida). Nesses casos usamos o entry point real do
    // modulo (que sabemos que funciona, e' onde o boot roda) como fallback
    // em vez de criar uma thread fadada a executar lixo.
    g_module_entry_point = entry_point;

    g_next_module_load_addr = (g_binary_segment_end + 0xFFFF) & ~0xFFFF;
    if (g_next_module_load_addr < 0x08900000) {
        g_next_module_load_addr = 0x08900000;
    }

    memset(threads, 0, sizeof(threads));
    threads[0].id = 0;
    snprintf(threads[0].name, sizeof(threads[0].name), "boot");
    threads[0].state = THREAD_STATE_RUNNING;
    threads[0].pc = entry_point + 4;
    threads[0].next_pc = entry_point + 4;
    current_thread_id = 0;
    next_thread_id_to_assign = 1;

    cpu.gpr[0x1D] = 0x0BFFF000;
    cpu.gpr[0x1F] = 0x08000100;
    cpu.gpr[0x1C] = relocated_gp ? relocated_gp : 0x08B533C0;
    cpu.pc = entry_point;
    cpu.next_pc = entry_point;

    mips_dispatcher_execute(&dispatcher, &cpu);

    mips_dispatcher_free(&dispatcher);
    mips_hle_free(&kernel);
    mips_cpu_free(&cpu);

    _exit(EXIT_SUCCESS);
}
