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
#include "mips_prx.h"
#include "sceKernelThread.h"
#include "sceSysMem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
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
static uint64_t fallback_call_counts[MAX_PATCHED_FALLBACKS];

// NOVO: pasta real onde o jogo vive (derivada de argv[1], ex: "./umd0CH").
// FIX: sanitize_path()/custom_sanitize_path() estavam com "./umd0/" e "./"
// fixos no codigo, sem nenhuma relacao com a pasta de onde o EBOOT.PBP foi
// realmente aberto - por isso arquivos soltos do jogo (data.csz etc), que
// ficam do LADO do EBOOT.PBP, nunca eram encontrados: o sceIoOpen procurava
// no diretorio de trabalho do emulador, nao na pasta do jogo.
char g_game_root[512] = ".";
char g_umd_root[512] = ".";
uint32_t g_module_entry_point = 0;
static bool g_umd_activated = true;

enum {
    PSP_UMD_NOT_PRESENT = 0x01,
    PSP_UMD_PRESENT = 0x02,
    PSP_UMD_NOT_READY = 0x08,
    PSP_UMD_READY = 0x10,
    PSP_UMD_READABLE = 0x20,
};

static bool umd_media_inserted(void) {
    struct stat root_stat;
    return g_game_root[0] != '\0' &&
           stat(g_game_root, &root_stat) == 0 &&
           S_ISDIR(root_stat.st_mode);
}

static uint32_t umd_drive_status(void) {
    if (!umd_media_inserted()) return PSP_UMD_NOT_PRESENT;

    uint32_t status = PSP_UMD_PRESENT | PSP_UMD_READY;
    if (g_umd_activated) status |= PSP_UMD_READABLE;
    return status;
}

static void hle_return_from_syscall(MIPS_CPU *cpu, uint32_t result) {
    cpu->gpr[0x02] = result;
    cpu->pc = cpu->gpr[0x1F];
    cpu->next_pc = cpu->pc + 4;
}

// NOVO: contador de "power locks" ativos (sceKernelPowerLock incrementa,
// sceKernelPowerUnlock decrementa). Nao simulamos suspensao de verdade,
// mas contar certinho evita destravar mais vezes do que travou (o que na
// PSP real e' um erro de nesting) e deixa log honesto de quantos locks
// ainda estao pendentes.
int g_power_lock_count = 0;
static bool g_volatile_mem_locked = false;
static bool g_sce_mpeg_initialized = false;
static uint16_t g_sce_mpeg_library_version = 0x0105;
static uint32_t g_impose_language = 1; /* PSP_SYSTEMPARAM_LANGUAGE_ENGLISH */
static uint32_t g_impose_button = 0;   /* PSP_SYSTEMPARAM_BUTTON_CIRCLE */

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
uint32_t g_binary_segment_start = 0;

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
    if (fseek(f, 0, SEEK_END) != 0) {
        printf("[PBP Extractor Erro] Falha ao determinar o tamanho do container.\n");
        fclose(f);
        return -4;
    }
    long file_size = ftell(f);
    if (file_size < 0 || offset_elf < sizeof(PBPHeader) ||
        offset_psar < offset_elf || (uint64_t)offset_psar > (uint64_t)file_size) {
        printf("[PBP Extractor Erro] Limites data.psp/data.psar invalidos.\n");
        fclose(f);
        return -4;
    }
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
static uint64_t vblank_wait_count = 0;
static uint64_t next_vblank_ns = 0;
static bool vblank_clock_initialized = false;
static uint32_t vblank_wait_syscall = 0;
static uint32_t vblank_wait_pc = 0;

typedef struct {
    int id;
    char name[64];
    char path[512];
    uint32_t entry_point;
    uint32_t load_bias;
    uint32_t module_start;
    uint32_t module_stop;
    uint32_t module_gp;
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
static HLE_DisplayState g_latched_display;
static bool g_display_framebuffer_latched = false;

typedef struct {
    int channel_id;
    int sample_count;
    int format;
    bool is_reserved;
} HLE_AudioChannel;

#define MAX_AUDIO_CHANNELS 8
static HLE_AudioChannel g_audio_channels[MAX_AUDIO_CHANNELS] = {0};

static void custom_sanitize_path(char *dest, const char *src) {
    char input[512];
    char combined[1024];
    char normalized[1024];
    const char *p;
    size_t len;

    // FIX: "./umd0/" fixo nao tinha nenhuma relacao com a pasta real do
    // jogo (g_game_root, derivada de argv[1] - ex: "./umd0CH"). Um caminho
    // sem prefixo tambem deve resolver pra dentro da pasta do jogo, e nao
    // pro diretorio de trabalho do emulador ("./"), senao arquivos soltos
    // que ficam do lado do EBOOT.PBP (ex: data.csz) nunca sao encontrados.
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    len = strnlen(src, sizeof(input) - 1);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    memcpy(input, src, len);
    input[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\\') input[i] = '/';
    }

    p = input;
    if (strncasecmp(p, "umd0:", 5) == 0 ||
        strncasecmp(p, "disc0:", 6) == 0) {
        p = strchr(p, ':') + 1;
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/%s", g_umd_root, p);
    } else if (strncasecmp(p, "ms0:", 4) == 0) {
        p = strchr(p, ':') + 1;
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/ms0/%s", g_game_root, p);
    } else {
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/%s", g_game_root, p);
    }

    normalized[0] = '\0';
    {
        char work[sizeof(combined)];
        char *save = NULL;
        char *part;
        snprintf(work, sizeof(work), "%s", combined);
        part = strtok_r(work, "/", &save);
        while (part != NULL) {
            size_t used = strlen(normalized);
            if (strcmp(part, ".") != 0 && part[0] != '\0') {
                if (strcmp(part, "..") == 0) {
                    char *slash = strrchr(normalized, '/');
                    if (slash != NULL) *slash = '\0';
                } else if (used + strlen(part) + 2 < sizeof(normalized)) {
                    if (used != 0) normalized[used++] = '/';
                    memcpy(normalized + used, part, strlen(part) + 1);
                }
            }
            part = strtok_r(NULL, "/", &save);
        }
    }
    snprintf(dest, 512, "%s", normalized);
}

static uint32_t hle_sceKernelLoadModule(MIPS_CPU *cpu) {
    char psp_path[256];
    char host_path[512];
    char resolved_path[1024];
    MIPS_PRX_File module_file;
    MIPS_PRX_Container container;
    MIPS_PRX_ReadStatus read_status;
    MIPS_PRX_PrepareStatus prepare_status;
    uint8_t *elf_data = NULL;
    size_t elf_size = 0;
    uint32_t entry_point = 0;
    uint32_t module_start = 0;
    uint32_t module_stop = 0;
    uint32_t module_gp = 0;
    uint32_t load_bias;
    int module_slot = -1;
    int load_result;
    uint32_t result;

    if (!safe_read_cstring(cpu, cpu->gpr[4], psp_path, sizeof(psp_path))) {
        result = 0x800200D3; // SCE_KERNEL_ERROR_ILLEGAL_ADDR
        printf("[HLE ModuleMgr] sceKernelLoadModule invalid path pointer=0x%08X "
               "result=0x%08X\n", cpu->gpr[4], result);
        return result;
    }

    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (!loaded_modules[i].is_active) {
            module_slot = i;
            break;
        }
    }
    if (module_slot < 0) {
        result = 0x80020190; // SCE_KERNEL_ERROR_NO_MEMORY
        printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
               "action=module_table_full\n", psp_path, result);
        return result;
    }

    custom_sanitize_path(host_path, psp_path);
    if (host_path[0] == '\0' ||
        mips_hle_resolve_case_insensitive(resolved_path, sizeof(resolved_path),
                                          host_path) != 0) {
        result = 0x80010002; // SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND
        printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
               "read=missing action=read_failed\n", psp_path, result);
        return result;
    }

    read_status = mips_prx_read_file(resolved_path, &module_file);
    if (read_status != MIPS_PRX_READ_OK) {
        result = read_status == MIPS_PRX_READ_OPEN_FAILED
            ? 0x80010002 // SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND
            : 0x80020130; // SCE_KERNEL_ERROR_FILEERR
        printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
               "read=%s action=read_failed\n",
               psp_path, result, mips_prx_read_status_name(read_status));
        return result;
    }

    prepare_status = mips_prx_prepare_elf(&module_file, &elf_data, &elf_size);
    if (prepare_status != MIPS_PRX_PREPARE_OK) {
        result = prepare_status == MIPS_PRX_PREPARE_OUT_OF_MEMORY
            ? 0x80020190 // SCE_KERNEL_ERROR_NO_MEMORY
            : 0x80020148; // SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE
        printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
               "read=ok prepare=%s\n",
               psp_path, result, mips_prx_prepare_status_name(prepare_status));
        mips_prx_file_release(&module_file);
        return result;
    }

    container = module_file.container;
    load_bias = g_next_module_load_addr != 0
        ? g_next_module_load_addr : PSP_USER_LOAD_BASE;
    load_result = load_prx_elf_buffer(cpu, elf_data, elf_size, &entry_point,
                                      &module_start, &module_stop, &module_gp);
    mips_prx_file_release(&module_file);
    if (load_result != 0) {
        result = 0x80020148; // SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE
        printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
               "prepare=ok elf_size=%zu loader_status=%d action=load_failed\n",
               psp_path, result, elf_size, load_result);
        return result;
    }

    PSPModule *module = &loaded_modules[module_slot];
    memset(module, 0, sizeof(*module));
    module->id = next_module_id++;
    snprintf(module->name, sizeof(module->name), "%s", psp_path);
    snprintf(module->path, sizeof(module->path), "%s", resolved_path);
    module->entry_point = entry_point;
    module->load_bias = load_bias;
    module->module_start = module_start;
    module->module_stop = module_stop;
    module->module_gp = module_gp;
    module->is_active = true;
    result = (uint32_t)module->id;
    printf("[HLE ModuleMgr] sceKernelLoadModule path=\"%s\" result=0x%08X "
           "prepare=ok container=%s elf_size=%zu entry=0x%08X "
           "module_start=0x%08X module_stop=0x%08X gp=0x%08X action=loaded\n",
           psp_path, result, mips_prx_container_name(container),
           elf_size, entry_point, module_start, module_stop, module_gp);
    return result;
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

static bool hle_monotonic_time_ns(uint64_t *time_ns) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("[HLE Display] clock_gettime(CLOCK_MONOTONIC)");
        return false;
    }
    *time_ns = (uint64_t)now.tv_sec * 1000000000ULL +
               (uint64_t)now.tv_nsec;
    return true;
}

static void hle_display_apply_framebuffer(MIPS_CPU *cpu,
                                          const HLE_DisplayState *state) {
    g_display.topaddr = state->topaddr;
    g_display.stride = state->stride;
    g_display.format = state->format;
    g_display.sync = state->sync;
    g_display.configured = state->configured;
    if (kernel.video_system != NULL && state->configured) {
        uint32_t width = g_display.disp_width != 0 ? g_display.disp_width : 480;
        uint32_t height = g_display.disp_height != 0 ? g_display.disp_height : 272;
        mips_video_set_framebuffer((MIPS_VideoSystem *)kernel.video_system,
                                   cpu, state->topaddr, state->stride,
                                   state->format, width, height);
    }
}

static void hle_display_service_vblank(MIPS_CPU *cpu) {
    const uint64_t vblank_period_ns = 16666667ULL;
    uint64_t now_ns;
    if (!hle_monotonic_time_ns(&now_ns)) return;

    if (!vblank_clock_initialized) {
        next_vblank_ns = now_ns + vblank_period_ns;
        vblank_clock_initialized = true;
        return;
    }
    if (now_ns < next_vblank_ns) return;

    uint64_t elapsed_vblanks =
        (now_ns - next_vblank_ns) / vblank_period_ns + 1;
    next_vblank_ns += elapsed_vblanks * vblank_period_ns;
    vblank_counter += (uint32_t)elapsed_vblanks;
    vblank_wait_count += elapsed_vblanks;
    if (g_display_framebuffer_latched) {
        HLE_DisplayState latched = g_latched_display;
        g_display_framebuffer_latched = false;
        hle_display_apply_framebuffer(cpu, &latched);
    }

    MIPS_VideoSystem *video = (MIPS_VideoSystem *)kernel.video_system;
    bool frame_pending = video != NULL &&
                         (video->frame_started || video->framebuffer_dirty);
    bool close_requested = video != NULL && mips_video_present(video);
    if (close_requested && cpu != NULL) {
        cpu->exit_requested = true;
    }

    static int trace_enabled = -1;
    if (trace_enabled < 0) {
        const char *value = getenv("MIPS_TRACE_VBLANK");
        trace_enabled = value != NULL && value[0] != '\0' &&
                        strcmp(value, "0") != 0;
    }
    if (trace_enabled &&
        (vblank_wait_count <= 8 || vblank_wait_count % 60 == 0)) {
        printf("[HLE Display VBlank] #%llu source=%s syscall=0x%05X "
               "pc=0x%08X video=%u frame_pending=%u close=%u counter=%u\n",
               (unsigned long long)vblank_wait_count,
               vblank_wait_syscall != 0 ? "wait" : "timer",
               vblank_wait_syscall, vblank_wait_pc, video != NULL,
               frame_pending, close_requested, vblank_counter);
        fflush(stdout);
    }
}

static void hle_display_wait_vblank(MIPS_CPU *cpu, uint32_t syscall_id) {
    static int trace_enabled = -1;
    if (trace_enabled < 0) {
        const char *value = getenv("MIPS_TRACE_VBLANK");
        trace_enabled = value != NULL && value[0] != '\0' &&
                        strcmp(value, "0") != 0;
    }

    uint32_t start_counter = vblank_counter;
    if (cpu == NULL) return;
    vblank_wait_syscall = syscall_id;
    vblank_wait_pc = cpu != NULL ? cpu->pc : 0;
    while (!cpu->exit_requested) {
        hle_display_service_vblank(cpu);
        if (vblank_counter != start_counter) break;

        uint64_t now_ns;
        if (!hle_monotonic_time_ns(&now_ns)) break;
        uint64_t remaining_ns = next_vblank_ns > now_ns
            ? next_vblank_ns - now_ns : 1000000ULL;
        if (remaining_ns > 2000000ULL) remaining_ns = 2000000ULL;
        struct timespec delay = {
            .tv_sec = (time_t)(remaining_ns / 1000000000ULL),
            .tv_nsec = (long)(remaining_ns % 1000000000ULL)
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
    vblank_wait_syscall = 0;
    vblank_wait_pc = 0;

    static uint64_t traced_wait_count;
    uint64_t wait_number = ++traced_wait_count;
    if (trace_enabled && (wait_number <= 8 || wait_number % 60 == 0)) {
        printf("[HLE Display Wait] #%llu syscall=0x%05X pc=0x%08X "
               "counter=%u\n",
               (unsigned long long)wait_number, syscall_id,
               cpu != NULL ? cpu->pc : 0, vblank_counter);
        fflush(stdout);
    }
}

static void hle_syscall_impl(MIPS_CPU *cpu) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;

    if (syscall_id == 0x80001) { // sceUmdCheckMedium
        hle_return_from_syscall(cpu, umd_media_inserted() ? 1 : 0);
        return;
    }
    if (syscall_id == 0x80002) { // sceUmdActivate
        uint32_t mode = cpu->gpr[0x04];
        uint32_t result = 0;
        if (mode < 1 || mode > 2) {
            result = 0x80010016; // SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT
        } else if (!umd_media_inserted()) {
            result = 0x80210001; // SCE_ERROR_UMD_NOT_READY
        } else {
            g_umd_activated = true;
        }
        hle_return_from_syscall(cpu, result);
        return;
    }
    if (syscall_id == 0x80003) { // sceUmdGetDriveStat
        hle_return_from_syscall(cpu, umd_drive_status());
        return;
    }
    if (syscall_id == 0x80004) { // sceUmdWaitDriveStatWithTimer
        uint32_t requested_status = cpu->gpr[0x04];
        uint32_t valid_status = PSP_UMD_NOT_PRESENT | PSP_UMD_PRESENT |
                                PSP_UMD_NOT_READY | PSP_UMD_READY |
                                PSP_UMD_READABLE;
        uint32_t result;
        if (requested_status == 0 || (requested_status & ~valid_status) != 0) {
            result = 0x80010016; // SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT
        } else if ((requested_status & umd_drive_status()) != 0) {
            result = 0;
        } else {
            result = 0x800201A8; // SCE_KERNEL_ERROR_WAIT_TIMEOUT
        }
        hle_return_from_syscall(cpu, result);
        return;
    }

    // --- SysMemUserForUser Modular Syscalls ---
    if (syscall_id == 0x90001) { hle_sceKernelTotalFreeMemSize(cpu); return; }
    if (syscall_id == 0x90002) { hle_sceKernelMaxFreeMemSize(cpu); return; }
    if (syscall_id == 0x90003) { hle_sceKernelAllocPartitionMemory(cpu); return; }
    if (syscall_id == 0x90004) { hle_sceKernelGetBlockHeadAddr(cpu); return; }
    if (syscall_id == 0x90005) { hle_sceKernelFreePartitionMemory(cpu); return; }
    if (syscall_id == 0x90006) { hle_sceKernelPrintf(cpu); return; }
    if (syscall_id == 0x33338) { // sceGeUnsetCallback
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x33334) { // sceGeEdramGetSize
        cpu->gpr[0x02] = 0x00200000;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60004) { // sceDisplayGetVcount
        cpu->gpr[0x02] = vblank_counter;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60005) { // sceDisplayWaitVblankStartCB
        hle_display_wait_vblank(cpu, syscall_id);
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x70004) { // sceCtrlSetSamplingMode / idle threshold
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
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
    if (syscall_id == 0x1111C) { // sceIoWrite
        uint32_t fd = cpu->gpr[0x04];
        uint32_t buf_ptr = cpu->gpr[0x05];
        uint32_t size = cpu->gpr[0x06];
        int slot = fd - 1;
        int32_t bytes_written = -1;
        if (slot >= 0 && slot < 32 && kernel.files[slot].is_active) {
            void *src = mips_get_ptr_range(cpu, buf_ptr, size);
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
            void *src = mips_get_ptr_range(cpu, buf_ptr, size);
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

    if (syscall_id == 0x1111E) { // sceIoGetstat
        uint32_t file_ptr = cpu->gpr[0x04];
        uint32_t stat_ptr = cpu->gpr[0x05];
        char file_path_buf[512];
        int32_t ret_val = -1;

        if (file_ptr != 0 && stat_ptr != 0 &&
            safe_read_cstring(cpu, file_ptr, file_path_buf, sizeof(file_path_buf)) &&
            mips_get_ptr_range(cpu, stat_ptr, 64) != NULL) {
            char host_path[512];
            custom_sanitize_path(host_path, file_path_buf);
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
    if (syscall_id == 0x22233) { hle_sceKernelCreateFpl(cpu); return; }
    if (syscall_id == 0x22234) { hle_sceKernelDeleteFpl(cpu); return; }
    if (syscall_id == 0x22235) { hle_sceKernelAllocateFpl(cpu); return; }
    if (syscall_id == 0x22236) { hle_sceKernelFreeFpl(cpu); return; }
    if (syscall_id == 0x22237) { hle_sceKernelGetThreadId(cpu); return; }
    if (syscall_id == 0x2223E) { hle_sceKernelGetSystemTimeWide(cpu); return; }

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
        hle_display_wait_vblank(cpu, syscall_id);
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60002) { // sceDisplaySetFrameBuf(topaddr, stride, pixelformat, sync)
        HLE_DisplayState requested = g_display;
        requested.topaddr = cpu->gpr[0x04];
        requested.stride  = cpu->gpr[0x05];
        requested.format  = cpu->gpr[0x06];
        requested.sync    = cpu->gpr[0x07];
        requested.configured = true;
        printf("[HLE Display] sceDisplaySetFrameBuf(topaddr=0x%08X, stride=%u, format=%u, sync=%u)\n",
               requested.topaddr, requested.stride, requested.format,
               requested.sync);
        if (requested.sync == 0) {
            g_display_framebuffer_latched = false;
            hle_display_apply_framebuffer(cpu, &requested);
            /* GE pixels are drawn to GLES; defer the host swap until VBlank. */
        } else if (requested.sync == 1) {
            g_latched_display = requested;
            g_display_framebuffer_latched = true;
        } else {
            fprintf(stderr,
                    "[HLE Display Error] Invalid sceDisplaySetFrameBuf sync=%u.\n",
                    requested.sync);
            cpu->gpr[0x02] = (uint32_t)-1;
            cpu->pc = cpu->gpr[0x1F];
            cpu->next_pc = cpu->pc + 4;
            return;
        }
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x60003) { // sceDisplayWaitVblankStart()
        hle_display_wait_vblank(cpu, syscall_id);
        cpu->gpr[0x02] = 0;
        cpu->pc = cpu->gpr[0x1F];
        cpu->next_pc = cpu->pc + 4;
        return;
    }
    if (syscall_id == 0x70001 || syscall_id == 0x70002) { // sceCtrlReadBufferPositive / sceCtrlPeekBufferPositive
        uint32_t pad_ptr = cpu->gpr[0x04];
        MIPS_VideoSystem *video = (MIPS_VideoSystem *)kernel.video_system;
        if (video != NULL) {
            mips_video_get_ctrl_state(video, &g_ctrl_state.buttons,
                                      &g_ctrl_state.lx, &g_ctrl_state.ly);
        }
        g_ctrl_state.time_stamp++;
        void *dst = mips_get_ptr_range(cpu, pad_ptr, sizeof(PSP_CtrlData));
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
            uint64_t calls = ++fallback_call_counts[idx];
            uint32_t nid = fallback_stubs[idx].nid;

            if (nid == 0x17C1684E) { // sceKernelReferThreadStatus
                ret_val = hle_sceKernelReferThreadStatus(cpu);
                cpu->gpr[0x02] = ret_val;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x977DE386) { // sceKernelLoadModule
                ret_val = hle_sceKernelLoadModule(cpu);
                cpu->gpr[0x02] = ret_val;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x50F0C1EC) { // sceKernelStartModule
                PSPModule *module = NULL;
                for (int module_index = 0;
                     module_index < MAX_LOADED_MODULES; module_index++) {
                    if (loaded_modules[module_index].is_active &&
                        (uint32_t)loaded_modules[module_index].id == cpu->gpr[4]) {
                        module = &loaded_modules[module_index];
                        break;
                    }
                }
                if (module == NULL) {
                    ret_val = 0x8002012E; // SCE_KERNEL_ERROR_UNKNOWN_MODULE
                    printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                           "result=0x%08X action=no_registered_module\n",
                           cpu->gpr[4], ret_val);
                } else if (module->is_started) {
                    ret_val = 0x80020001; // PPSSPP returns SCE_KERNEL_ERROR_ERROR here.
                    printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                           "result=0x%08X action=already_started\n",
                           cpu->gpr[4], ret_val);
                } else {
                    uint32_t start_pc = module->module_start != 0
                        ? module->module_start : module->entry_point;
                    if (start_pc == 0 || start_pc == UINT32_MAX) {
                        module->is_started = true;
                        ret_val = (uint32_t)module->id;
                        printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                               "result=0x%08X action=started_without_entry\n",
                               cpu->gpr[4], ret_val);
                    } else {
                        int thread_id = hle_create_and_start_thread(
                            cpu, module->name, start_pc);
                        if (thread_id < 0) {
                            ret_val = 0x80020190; // SCE_KERNEL_ERROR_NO_MEMORY
                            printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                                   "result=0x%08X action=start_thread_failed\n",
                                   cpu->gpr[4], ret_val);
                        } else {
                            for (int thread_index = 0;
                                 thread_index < MAX_THREADS; thread_index++) {
                                if (threads[thread_index].id == thread_id) {
                                    threads[thread_index].regs[4] = cpu->gpr[5];
                                    threads[thread_index].regs[5] = cpu->gpr[6];
                                    if (module->module_gp != 0) {
                                        threads[thread_index].regs[28] =
                                            module->module_gp;
                                    }
                                    break;
                                }
                            }
                            module->is_started = true;
                            ret_val = (uint32_t)module->id;
                            printf("[HLE ModuleMgr] sceKernelStartModule id=0x%08X "
                                   "result=0x%08X entry=0x%08X thread=%d "
                                   "action=start_waiting\n",
                                   cpu->gpr[4], ret_val, start_pc, thread_id);
                            hle_thread_wait_for_module_start(
                                cpu, thread_id, ret_val, cpu->gpr[7]);
                            return;
                        }
                    }
                }
                cpu->gpr[0x02] = ret_val;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0xD1FF982A) { // sceKernelStopModule
                PSPModule *module = NULL;
                for (int module_index = 0;
                     module_index < MAX_LOADED_MODULES; module_index++) {
                    if (loaded_modules[module_index].is_active &&
                        (uint32_t)loaded_modules[module_index].id == cpu->gpr[4]) {
                        module = &loaded_modules[module_index];
                        break;
                    }
                }
                if (module == NULL) {
                    ret_val = 0x8002012E; // SCE_KERNEL_ERROR_UNKNOWN_MODULE
                    printf("[HLE ModuleMgr] sceKernelStopModule id=0x%08X "
                           "result=0x%08X action=no_registered_module\n",
                           cpu->gpr[4], ret_val);
                } else if (!module->is_started) {
                    ret_val = 0x80020135; // SCE_KERNEL_ERROR_ALREADY_STOPPED
                    printf("[HLE ModuleMgr] sceKernelStopModule id=0x%08X "
                           "result=0x%08X action=already_stopped\n",
                           cpu->gpr[4], ret_val);
                } else if (module->module_stop == 0) {
                    module->is_started = false;
                    ret_val = 0;
                    printf("[HLE ModuleMgr] sceKernelStopModule id=0x%08X "
                           "result=0 action=no_stop_export\n",
                           cpu->gpr[4]);
                } else {
                    int thread_id = hle_create_and_start_thread(
                        cpu, module->name, module->module_stop);
                    if (thread_id < 0) {
                        ret_val = 0x80020190; // SCE_KERNEL_ERROR_NO_MEMORY
                        printf("[HLE ModuleMgr] sceKernelStopModule id=0x%08X "
                               "result=0x%08X action=stop_thread_failed\n",
                               cpu->gpr[4], ret_val);
                    } else {
                        for (int thread_index = 0;
                             thread_index < MAX_THREADS; thread_index++) {
                            if (threads[thread_index].id == thread_id) {
                                threads[thread_index].regs[4] = cpu->gpr[5];
                                threads[thread_index].regs[5] = cpu->gpr[6];
                                if (module->module_gp != 0) {
                                    threads[thread_index].regs[28] =
                                        module->module_gp;
                                }
                                break;
                            }
                        }
                        module->is_started = false;
                        printf("[HLE ModuleMgr] sceKernelStopModule id=0x%08X "
                               "result=0 stop=0x%08X thread=%d "
                               "action=stop_scheduled_waiting\n",
                               cpu->gpr[4], module->module_stop, thread_id);
                        cpu->gpr[4] = (uint32_t)thread_id;
                        hle_sceKernelWaitThreadEndCB(cpu);
                        return;
                    }
                }
                cpu->gpr[0x02] = ret_val;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x2E0911AA) { // sceKernelUnloadModule
                PSPModule *module = NULL;
                for (int module_index = 0;
                     module_index < MAX_LOADED_MODULES; module_index++) {
                    if (loaded_modules[module_index].is_active &&
                        (uint32_t)loaded_modules[module_index].id == cpu->gpr[4]) {
                        module = &loaded_modules[module_index];
                        break;
                    }
                }
                if (module == NULL) {
                    ret_val = 0x8002012E; // SCE_KERNEL_ERROR_UNKNOWN_MODULE
                    printf("[HLE ModuleMgr] sceKernelUnloadModule id=0x%08X "
                           "result=0x%08X action=no_registered_module\n",
                           cpu->gpr[4], ret_val);
                } else {
                    printf("[HLE ModuleMgr] sceKernelUnloadModule id=0x%08X "
                           "result=0 action=slot_released code_memory_retained\n",
                           cpu->gpr[4]);
                    memset(module, 0, sizeof(*module));
                    ret_val = 0;
                }
                cpu->gpr[0x02] = ret_val;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }

            /*
             * scePower_EBD177D6 is the PSP alias for
             * scePowerSetClockFrequency.  The R36S backend does not model
             * PSP clock changes, but the API is successful and intentionally
             * has no observable side effect for the emulated game.
             */
            if (nid == 0xEBD177D6) {
                cpu->gpr[0x02] = 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                printf("[HLE Power] scePowerSetClockFrequency alias 0xEBD177D6 -> PSP_OK (clock unchanged).\n");
                return;
            }
            /*
             * PPSSPP maps 0x71EC4271 to sceKernelLibcGettimeofday().
             * Returning a constant zero makes time-based polling loops
             * never observe progress.  PSP's timeval uses two 32-bit
             * fields (seconds and microseconds), so write it explicitly to
             * emulated RAM instead of exposing the host struct layout.
             */
            if (nid == 0x71EC4271) {
                struct timeval now;
                void *timeval_ptr = mips_get_ptr_range(cpu, cpu->gpr[0x04], 8);
                if (timeval_ptr == NULL) {
                    cpu->gpr[0x02] = (uint32_t)-1;
                } else if (gettimeofday(&now, NULL) != 0) {
                    cpu->gpr[0x02] = (uint32_t)-1;
                } else {
                    mips_write32(cpu, cpu->gpr[0x04] + 0, (uint32_t)now.tv_sec);
                    mips_write32(cpu, cpu->gpr[0x04] + 4, (uint32_t)now.tv_usec);
                    cpu->gpr[0x02] = 0;
                }
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0xC41C2853) { // sceRtcGetTickResolution
                cpu->gpr[0x02] = 1000000;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x3F7AD767) { // sceRtcGetCurrentTick
                struct timespec now;
                uint64_t tick;
                clock_gettime(CLOCK_MONOTONIC, &now);
                tick = (uint64_t)now.tv_sec * 1000000ULL +
                       (uint64_t)now.tv_nsec / 1000ULL;
                cpu->gpr[0x02] = (uint32_t)tick;
                cpu->gpr[0x03] = (uint32_t)(tick >> 32);
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x029CA3B3) { // sceRtcGetAccumulativeTime
                struct timespec now;
                uint64_t tick;
                clock_gettime(CLOCK_MONOTONIC, &now);
                tick = (uint64_t)now.tv_sec * 1000000ULL +
                       (uint64_t)now.tv_nsec / 1000ULL;
                cpu->gpr[0x02] = (uint32_t)tick;
                cpu->gpr[0x03] = (uint32_t)(tick >> 32);
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x4CFA57B0 || nid == 0xE7C27D1B) {
                /* ScePspDateTime: year, month, day, hour, minute, second,
                 * microsecond. The homebrew uses this for display/state,
                 * so provide a real UTC/local calendar instead of zeros. */
                uint32_t date_ptr = cpu->gpr[0x04];
                time_t now = time(NULL);
                struct tm calendar;
                if (date_ptr == 0 ||
                    mips_get_ptr_range(cpu, date_ptr, 28) == NULL) {
                    cpu->gpr[0x02] = (uint32_t)-1;
                } else {
                    if (nid == 0xE7C27D1B) {
                        localtime_r(&now, &calendar);
                    } else {
                        gmtime_r(&now, &calendar);
                    }
                    mips_write32(cpu, date_ptr + 0, (uint32_t)(calendar.tm_year + 1900));
                    mips_write32(cpu, date_ptr + 4, (uint32_t)(calendar.tm_mon + 1));
                    mips_write32(cpu, date_ptr + 8, (uint32_t)calendar.tm_mday);
                    mips_write32(cpu, date_ptr + 12, (uint32_t)calendar.tm_hour);
                    mips_write32(cpu, date_ptr + 16, (uint32_t)calendar.tm_min);
                    mips_write32(cpu, date_ptr + 20, (uint32_t)calendar.tm_sec);
                    mips_write32(cpu, date_ptr + 24, 0);
                    cpu->gpr[0x02] = 0;
                }
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x42307A17) { // sceRtcIsLeapYear
                uint32_t year = cpu->gpr[0x04];
                cpu->gpr[0x02] = ((year % 4 == 0 && year % 100 != 0) ||
                                  (year % 400 == 0)) ? 1 : 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x05EF322C) { // sceRtcGetDaysInMonth
                static const uint8_t days[] =
                    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                uint32_t year = cpu->gpr[0x04];
                uint32_t month = cpu->gpr[0x05];
                uint32_t result = 0;
                if (month >= 1 && month <= 12) {
                    result = days[month - 1];
                    if (month == 2 &&
                        ((year % 4 == 0 && year % 100 != 0) ||
                         (year % 400 == 0))) result++;
                }
                cpu->gpr[0x02] = result;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x27CC57F0) { // sceKernelLibcTime
                time_t now = time(NULL);
                if (cpu->gpr[0x04] != 0 &&
                    mips_get_ptr_range(cpu, cpu->gpr[0x04], 4) != NULL) {
                    mips_write32(cpu, cpu->gpr[0x04], (uint32_t)now);
                }
                cpu->gpr[0x02] = (uint32_t)now;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0xF919F628) {
                hle_sceKernelTotalFreeMemSize(cpu);
                return;
            }
            if (nid == 0xEA748E31) { // sceKernelChangeCurrentThreadAttr
                cpu->gpr[0x02] = 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            /*
             * sceKernelDcacheWritebackAll has no host-side cache to flush.
             * It still must return PSP_OK so callers do not fall into the
             * generic zero-valued fallback path.
             */
            if (nid == 0x79D1C3FA) {
                cpu->gpr[0x02] = 0;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            if (nid == 0x82BC5777) { hle_sceKernelGetThreadCurrentPriority(cpu); return; }
            else if (nid == 0x71BC9871) { hle_sceKernelChangeThreadPriority(cpu); return; }
            else if (nid == 0x9FA03CD3) { hle_sceKernelDeleteThread(cpu); return; }
            else if (nid == 0x82826F70) { hle_sceKernelSleepThreadCB(cpu); return; }  // sceKernelSleepThread
            else if (nid == 0x278C0DF5) { hle_sceKernelWaitThreadEndCB(cpu); return; } // sceKernelWaitThreadEnd
            else if (nid == 0xE81CAF8F) { hle_sceKernelCreateCallback(cpu); return; }
            else if (nid == 0x349D6D6C) { hle_sceKernelCheckCallback(cpu); return; }
            else if (nid == 0x68DA9E36) { hle_sceKernelDelayThread(cpu); return; }
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
            else if (nid == 0x3E0271D3 || nid == 0xA14F40B2) {
                // sceKernelVolatileMemLock / sceKernelVolatileMemTryLock
                uint32_t type = cpu->gpr[0x04];
                uint32_t address_out = cpu->gpr[0x05];
                uint32_t size_out = cpu->gpr[0x06];
                uint32_t result = 0;
                if (type != 0) {
                    result = 0x80000107; // SCE_KERNEL_ERROR_INVALID_MODE
                } else if (g_volatile_mem_locked) {
                    result = 0x802B0200; // SCE_KERNEL_ERROR_POWER_VMEM_IN_USE
                } else {
                    uint32_t *address_ptr =
                        (uint32_t *)mips_get_ptr_range(cpu, address_out, sizeof(uint32_t));
                    uint32_t *size_ptr =
                        (uint32_t *)mips_get_ptr_range(cpu, size_out, sizeof(uint32_t));
                    if (address_ptr != NULL) *address_ptr = 0x08400000;
                    if (size_ptr != NULL) *size_ptr = 0x00400000;
                    g_volatile_mem_locked = true;
                }
                printf("[HLE Power] sceKernelVolatileMem%s(type=%u, addr_out=0x%08X, "
                       "size_out=0x%08X) -> 0x%08X%s\n",
                       nid == 0xA14F40B2 ? "TryLock" : "Lock",
                       type, address_out, size_out, result,
                       result == 0 ? " (addr=0x08400000, size=0x00400000)" : "");
                cpu->gpr[0x02] = result;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            else if (nid == 0xA569E425) { // sceKernelVolatileMemUnlock
                uint32_t type = cpu->gpr[0x04];
                uint32_t result;
                if (type != 0) {
                    result = 0x80000107; // SCE_KERNEL_ERROR_INVALID_MODE
                } else if (!g_volatile_mem_locked) {
                    result = 0x800201AE; // SCE_KERNEL_ERROR_SEMA_OVF
                } else {
                    g_volatile_mem_locked = false;
                    result = 0;
                }
                printf("[HLE Power] sceKernelVolatileMemUnlock(type=%u) -> 0x%08X\n",
                       type, result);
                cpu->gpr[0x02] = result;
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            else if (nid == 0x090CCB3F) { // sceKernelPowerTick
                cpu->gpr[0x02] = 0;
                printf("[HLE Power] sceKernelPowerTick(type=%u) -> 0\n",
                       cpu->gpr[0x04]);
                cpu->pc = cpu->gpr[0x1F];
                cpu->next_pc = cpu->pc + 4;
                return;
            }
            else if (nid == 0x3AD58B8C) { // sceKernelSuspendDispatchThread
                ret_val = hle_sceKernelSuspendDispatchThread();
            }
            else if (nid == 0x27E22EC2) { // sceKernelResumeDispatchThread
                ret_val = hle_sceKernelResumeDispatchThread(cpu->gpr[0x04]);
            }
            else if (nid == 0xA177798D) { // sceMeMalloc
                ret_val = hle_sceMeMalloc(cpu, cpu->gpr[0x04]);
            }
            else if (nid == 0xC86500E3) { // sceMeCalloc
                ret_val = hle_sceMeCalloc(cpu, cpu->gpr[0x04], cpu->gpr[0x05]);
            }
            else if (nid == 0x5AD660FA) { // sceMeFree
                ret_val = hle_sceMeFree(cpu->gpr[0x04]);
            }
            else if (nid == 0x682A619B) { // sceMpegInit
                if (fallback_stubs[idx].library_version != 0) {
                    g_sce_mpeg_library_version =
                        fallback_stubs[idx].library_version;
                }
                if (g_sce_mpeg_initialized) {
                    printf("[HLE sceMpeg] sceMpegInit called again\n");
                }
                g_sce_mpeg_initialized = true;
                ret_val = 0;
                printf("[HLE sceMpeg] sceMpegInit version=0x%04X -> 0\n",
                       g_sce_mpeg_library_version);
            }
            else if (nid == 0xC132E22F) { // sceMpegQueryMemSize (mpeg.prx 1.05)
                if (fallback_stubs[idx].library_version != 0) {
                    g_sce_mpeg_library_version =
                        fallback_stubs[idx].library_version;
                }
                ret_val = g_sce_mpeg_library_version < 0x0105
                    ? 0x0000B3DB : 0x00010000;
                printf("[HLE sceMpeg] QueryMemSize version=0x%04X -> 0x%08X\n",
                       g_sce_mpeg_library_version, ret_val);
            }
            else if (nid == 0xD7A29F46) { // sceMpegRingbufferQueryMemSize
                ret_val = cpu->gpr[0x04] * (2048u + 104u);
                printf("[HLE sceMpeg] RingbufferQueryMemSize packets=%u -> %u\n",
                       cpu->gpr[0x04], ret_val);
            }
            else if (nid == 0x24FD7BCF) { // sceImposeGetLanguageMode
                uint32_t language_out = cpu->gpr[0x04];
                uint32_t button_out = cpu->gpr[0x05];
                if (language_out != 0 &&
                    mips_get_ptr_range(cpu, language_out, sizeof(uint32_t)) != NULL) {
                    mips_write32(cpu, language_out, g_impose_language);
                }
                if (button_out != 0 &&
                    mips_get_ptr_range(cpu, button_out, sizeof(uint32_t)) != NULL) {
                    mips_write32(cpu, button_out, g_impose_button);
                }
                ret_val = 0;
            }
            else if (nid == 0x36AA6E91) { // sceImposeSetLanguageMode
                g_impose_language = cpu->gpr[0x04];
                g_impose_button = cpu->gpr[0x05];
                ret_val = 0;
            }
            else if (nid == 0x29681260) { // sceAudiocodecReleaseEDRAM
                ret_val = 0;
                printf("[HLE sceAudiocodec] ReleaseEDRAM ctx=0x%08X codec=0x%08X\n",
                       cpu->gpr[0x04], cpu->gpr[0x05]);
            }
            else {
                if (calls <= 3 || calls % 10000 == 0) {
                    printf("[HLE Fallback] Unimplemented API called: %s -> NID: 0x%08X at PC: 0x%08X args=(%08X,%08X,%08X,%08X) (returns 0; call #%llu)\n",
                           fallback_stubs[idx].lib_name, nid, cpu->pc,
                           cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7],
                           (unsigned long long)calls);
                    if (nid == 0x977DE386) {
                        char module_path[256];
                        size_t path_length = 0;
                        bool path_valid = false;
                        for (; path_length + 1 < sizeof(module_path); ++path_length) {
                            uint64_t address = (uint64_t)cpu->gpr[4] + path_length;
                            if (address > UINT32_MAX) break;
                            uint8_t *byte = (uint8_t *)mips_get_ptr_range(
                                cpu, (uint32_t)address, sizeof(uint8_t));
                            if (byte == NULL) break;
                            module_path[path_length] = (char)*byte;
                            if (*byte == '\0') {
                                path_valid = true;
                                break;
                            }
                        }
                        module_path[path_length] = '\0';
                        printf("[HLE Fallback Detail] sceKernelLoadModule path=\"%s\" "
                               "valid=%s flags=0x%08X option=0x%08X\n",
                               module_path, path_valid ? "yes" : "no",
                               cpu->gpr[5], cpu->gpr[6]);
                    }
                }
            }
        }
        if (cpu->gpr[0x1F] == 0) {
            printf("[HLE Fallback] Syscall 0x%05X retornou com $ra=0; encerrando a thread com seguranca\n",
                   syscall_id);
            cpu->gpr[0x02] = ret_val;
            cpu->pc = 0x08000100;
            cpu->next_pc = cpu->pc + 4;
            return;
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

void hle_syscall(MIPS_CPU *cpu) {
    static uint64_t syscall_trace_count = 0;
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;
    uint32_t caller_pc = cpu->pc;
    uint32_t arg0 = cpu->gpr[4];
    uint32_t arg1 = cpu->gpr[5];
    uint32_t arg2 = cpu->gpr[6];
    uint32_t arg3 = cpu->gpr[7];
    const uint32_t args[4] = {arg0, arg1, arg2, arg3};

    cpu->scheduler_saved_v0_valid = false;
    hle_syscall_impl(cpu);
    uint32_t syscall_result = cpu->scheduler_saved_v0_valid
        ? cpu->scheduler_saved_v0 : cpu->gpr[2];
    cpu->scheduler_saved_v0_valid = false;
    syscall_trace_count++;
    mips_trace_hle(cpu, caller_pc, syscall_id, args, syscall_result);
    if (syscall_id != MIPS_HLE_SAS_END_FLAG &&
        syscall_trace_count <= 2000 &&
        (syscall_result == 0xFFFFFFFF ||
         (int32_t)syscall_result < 0)) {
        printf("[HLE TRACE] #%llu syscall=0x%05X "
               "args=(0x%08X,0x%08X,0x%08X,0x%08X) "
               "v0=0x%08X caller_pc=0x%08X ra=0x%08X\n",
               (unsigned long long)syscall_trace_count,
               syscall_id, arg0, arg1, arg2, arg3,
               syscall_result, cpu->pc, cpu->gpr[31]);
        fflush(stdout);
    }
}

extern void mips_interpreter_step(MIPS_CPU *cpu);

void interpreter_step(MIPS_CPU *cpu) {
    /*
     * AOT blocks are generated from the pre-relocation image and may invoke
     * this fallback directly with a relative control-flow address.  Convert
     * it before touching emulated RAM; otherwise a valid branch into a
     * relocated stub is mistaken for an invalid host address.
     */
    if (cpu->pc != 0 && cpu->pc < 0x04000000) {
        cpu->pc += PSP_USER_LOAD_BASE;
    }
    if (cpu->next_pc != 0 && cpu->next_pc < 0x04000000) {
        cpu->next_pc += PSP_USER_LOAD_BASE;
    }
    uint32_t pc_val = cpu->pc;

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
        // FIX (deadlock silencioso pós-PowerLock): o atalho antigo, quando a
        // thread atual já não estava mais RUNNING, setava cpu->pc = 0x08000300
        // NA MÃO e retornava sem chamar run_scheduler(). Isso pulava por
        // completo a lógica que já existe dentro de run_scheduler() pra esse
        // exato cenário — acordar a próxima thread WAITING (ex: user_main
        // dormindo esperando um callback) ou terminar limpo se não sobrou
        // nenhuma thread. Resultado: PC ficava preso em 0x08000300, que não
        // tem handler em lugar nenhum (nem aqui, nem em mips_interpreter.c),
        // e caía em execução crua de memória vazia pra sempre — sem crash,
        // sem log novo, sem avançar.
        //
        // run_scheduler() já é seguro de chamar aqui mesmo quando a thread
        // atual não está RUNNING: o próprio topo da função (linha ~100) só
        // salva contexto se threads[i].state == THREAD_STATE_RUNNING: caso
        // contrário, pula direto pra busca de READY/WAITING. Então não
        // precisamos mais do atalho — só evitamos imprimir a mensagem de
        // "thread completed" quando não havia thread RUNNING de fato.
        bool is_running = false;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (threads[i].id == current_thread_id && threads[i].state == THREAD_STATE_RUNNING) {
                is_running = true;
                break;
            }
        }
        if (is_running) {
            printf("[HLE ThreadMan] Return address trap (0x08000100) triggered for Thread ID %d. Thread completed execution.\n", current_thread_id);
        }
        run_scheduler(cpu, THREAD_STATE_FREE);
        return;
    }

    mips_interpreter_step(cpu);
}

int main(int argc, char **argv) {
    printf("[BUILD] %s %s\n", __DATE__, __TIME__);
#ifdef FORCE_INTERPRETER
    printf("[BUILD MODE] PURE_INTERPRETER\n");
#else
    printf("[BUILD MODE] HYBRID_AOT_INTERPRETER\n");
#endif
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
        const char *last_backslash = strrchr(target_path, '\\');
        if (last_backslash != NULL && (last_slash == NULL || last_backslash > last_slash)) {
            last_slash = last_backslash;
        }
        if (last_slash != NULL) {
            size_t dir_len = (size_t)(last_slash - target_path);
            if (dir_len >= sizeof(g_game_root)) dir_len = sizeof(g_game_root) - 1;
            memcpy(g_game_root, target_path, dir_len);
            g_game_root[dir_len] = '\0';
            for (size_t i = 0; i < dir_len; i++) {
                if (g_game_root[i] == '\\') g_game_root[i] = '/';
            }
        } else {
            strcpy(g_game_root, ".");
        }
        snprintf(g_umd_root, sizeof(g_umd_root), "%s", g_game_root);
        {
            char *psp_game = NULL;
            for (char *p = g_umd_root; *p != '\0'; p++) {
                if (strncasecmp(p, "/PSP_GAME/SYSDIR", 16) == 0 &&
                    (p[16] == '\0' || p[16] == '/')) {
                    psp_game = p;
                    break;
                }
            }
            if (psp_game != NULL) {
                *psp_game = '\0';
            } else {
                size_t root_len = strlen(g_umd_root);
                const char *suffix = "/PSP_GAME";
                size_t suffix_len = strlen(suffix);
                if (root_len >= suffix_len &&
                    strcasecmp(g_umd_root + root_len - suffix_len, suffix) == 0) {
                    g_umd_root[root_len - suffix_len] = '\0';
                }
            }
            if (g_umd_root[0] == '\0') {
                snprintf(g_umd_root, sizeof(g_umd_root), ".");
            }
        }
        printf("[Boot] Raiz de dados do jogo definida como: '%s'\n", g_game_root);
        printf("[Boot] Raiz da midia UMD definida como: '%s'\n", g_umd_root);
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

    if (mips_hle_init(&kernel) != 0) {
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }

    MIPS_Dispatcher dispatcher;
    mips_dispatcher_init(&dispatcher);
    dispatcher.fallback_interpreter = interpreter_step;
    dispatcher.service_callback = hle_display_service_vblank;

    uint32_t entry_point = 0x08804000;
    int load_ret = load_eboot(&cpu, load_path, &entry_point);
    if (load_ret != 0) {
        fprintf(stderr, "[Boot ERRO] Falha ao carregar/linkar o executavel (status %d).\n", load_ret);
        mips_dispatcher_free(&dispatcher);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    if (mips_dispatcher_aot_image_matches(&cpu)) {
        aot_register_blocks(&dispatcher);
        printf("[AOT] GOW image signature matched; static AOT blocks enabled.\n");
    } else {
        printf("[AOT] GOW image signature mismatch; using interpreter for this executable.\n");
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
    hle_scheduler_set_dispatch_enabled(true);
    g_sce_mpeg_initialized = false;

    cpu.gpr[0x1D] = 0x0BFFF000;
    cpu.gpr[0x1F] = 0x08000100;
    cpu.gpr[0x1C] = relocated_gp ? relocated_gp : 0x08B533C0;
    cpu.pc = entry_point;
    cpu.next_pc = entry_point + 4;

    mips_dispatcher_execute(&dispatcher, &cpu);
    printf("[Boot] Dispatcher encerrou: exit_requested=%s pc=0x%08X next=0x%08X "
           "bad_vaddr=0x%08X last_fault_pc=0x%08X last_fault_inst=0x%08X "
           "last_fault_access=0x%03X faults=%u current_thread=%d\n",
           cpu.exit_requested ? "true" : "false",
           cpu.pc, cpu.next_pc, cpu.bad_vaddr, cpu.last_fault_pc,
           cpu.last_fault_inst, cpu.last_fault_access,
           cpu.memory_fault_count, current_thread_id);
    for (int i = 0; i < fallback_stubs_count; i++) {
        printf("[HLE Fallback Summary] module=%s NID=0x%08X calls=%llu\n",
               fallback_stubs[i].lib_name, fallback_stubs[i].nid,
               (unsigned long long)fallback_call_counts[i]);
    }
    fflush(stdout);

    mips_dispatcher_free(&dispatcher);
    mips_hle_free(&kernel);
    mips_cpu_free(&cpu);

    return EXIT_SUCCESS;
}
