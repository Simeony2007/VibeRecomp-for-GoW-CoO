#define _DEFAULT_SOURCE
#include "cpu.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "scheduler.h"
#include <pthread.h>
#include <sys/time.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Tabela de Arquivos Abertos (O Emulador suporta até 32 arquivos simultâneos)
#define MAX_OPEN_FILES 32
FILE* psp_file_table[MAX_OPEN_FILES] = {NULL};

// ─── Estado do Sistema HLE ───────────────────────────────────────────────────
MIPS_CPU main_thread_snapshot;
uint32_t saved_main_pc = 0;
static int thread_count = 0;
static int started_threads = 0;

#define MAX_DIRS 16
DIR *psp_dirs[MAX_DIRS] = {NULL};

#define MAX_THREADS 16
uint32_t threads_entry[MAX_THREADS] = {0};

#define MAX_FDS 32
int psp_fds[MAX_FDS] = {0};

typedef void (*HLE_Func)(MIPS_CPU *cpu, uint8_t *mem);

typedef struct {
    const char *name;
    HLE_Func handler;
} SyscallEntry;

extern void hle_sceCtrlSetSamplingCycle(MIPS_CPU*, uint8_t*);
extern void hle_sceCtrlSetSamplingMode(MIPS_CPU*, uint8_t*);
extern void hle_sceCtrlReadBufferPositive(MIPS_CPU*, uint8_t*);
extern void hle_sceCtrlPeekBufferPositive(MIPS_CPU*, uint8_t*);
extern void ctrl_update(uint8_t *mem);

extern PSP_EventFlag evflag_pool[];
extern int evflag_count;

// ─── Subsistema UMD ──────────────────────────────────────────────────────────

void hle_sceUmdActivate(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE] sceUmdActivate | Drive UMD ativado.\n");
    cpu->v0 = 0;
}

void hle_sceUmdWaitDriveStatWithTimer(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE] sceUmdWaitDriveStatWithTimer | Drive UMD pronto.\n");
    cpu->v0 = 0;
}

// ─── Subsistema de I/O ───────────────────────────────────────────────────────

// Função para traduzir o caminho do PSP para o Linux
void translate_psp_path(const char* psp_path, char* host_path) {
    if (strncmp(psp_path, "disc0:/", 7) == 0) {
        sprintf(host_path, "./GOW_DATA/%s", psp_path + 7);
    } else if (strncmp(psp_path, "host0:/", 7) == 0) {
        sprintf(host_path, "./GOW_DATA/%s", psp_path + 7);
    } else if (strncmp(psp_path, "ms0:/", 5) == 0) {
        sprintf(host_path, "./GOW_DATA/%s", psp_path + 5);
    } else {
        // Se for um caminho relativo ou sem prefixo, joga na USRDIR
        sprintf(host_path, "./GOW_DATA/PSP_GAME/USRDIR/%s", psp_path);
    }
}

void read_psp_string(char *dest, uint8_t *mem, uint32_t addr, int max) {
    if (addr == 0) {
        strcpy(dest, "");
        return;
    }
    // Traduz o endereço virtual do PSP para o índice real no nosso array mem
    uint32_t real_addr = psp_translate_addr(addr); 
    
    int i = 0;
    for (i = 0; i < max - 1; i++) {
        uint8_t c = mem[real_addr + i];
        if (c == 0 || c < 10 || c > 126) break; // Para no nulo ou lixo
        dest[i] = (char)c;
    }
    dest[i] = '\0';
}

void hle_sceIoChdir(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t path_addr = cpu->a0;
    char path[256];
    read_psp_string(path, mem, path_addr, 256);
    // FIX: log do endereço bruto para diagnóstico de ponteiros corrompidos
    printf("[HLE] sceIoChdir | a0=0x%08X -> '%s'\n", path_addr, path);
    cpu->v0 = 0;
}

void hle_sceIoDopen(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t dir_addr = cpu->a0;
    
    // MODO DIAGNÓSTICO: Vamos fazer um Hex Dump dos 16 bytes que o Kratos está lendo
    uint32_t base = psp_translate_addr(dir_addr);
    printf("[DIAG] Memoria bruta em 0x%08X (Fisico: 0x%08X): ", dir_addr, base);
    for(int i=0; i<16; i++) printf("%02X ", mem[base + i]);
    printf("\n");

    char path[256];
    read_psp_string(path, mem, dir_addr, 256);

    // FIX: log do endereço bruto para diagnóstico de ponteiros corrompidos
    char local_path[512] = "./GOW_DATA/";
    printf("[HLE] sceIoDopen | a0=0x%08X -> '%s' => '%s'\n", dir_addr, path, local_path);

    DIR *d = opendir(local_path);
    if (d) {
        for (int i = 1; i < MAX_DIRS; i++) {
            if (psp_dirs[i] == NULL) {
                psp_dirs[i] = d;
                cpu->v0 = i;
                return;
            }
        }
    }
    printf("      -> [ERRO] Diretório não encontrado!\n");
    cpu->v0 = 0x80010002;
}

void hle_sceIoDread(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    uint32_t dirent_addr = cpu->a1;

    if (fd > 0 && fd < MAX_DIRS && psp_dirs[fd] != NULL) {
        struct dirent *dir = readdir(psp_dirs[fd]);
        if (dir) {
            uint32_t base = dirent_addr & 0x01FFFFFF;
            memset(&mem[base], 0, 344);
            strncpy((char*)&mem[base + 0x58], dir->d_name, 255);
            mem[base + 0x58 + 255] = '\0';
            mem[base + 0x58 + 254] = '\0';  // FIX: terminador explícito (silencia warning)

            uint64_t size = (dir->d_type == DT_DIR) ? 0 : 2048;
            MEM_W32(mem, base + 0x08, (uint32_t)size);
            MEM_W32(mem, base + 0x0C, (uint32_t)(size >> 32));

            uint32_t attr = (dir->d_type == DT_DIR) ? 0x10 : 0x20;
            MEM_W32(mem, base + 0x04, attr);

            cpu->v0 = 1;
        } else {
            cpu->v0 = 0;
        }
    } else {
        cpu->v0 = 0x80010009;
    }
}

void hle_sceIoDclose(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    if (fd > 0 && fd < MAX_DIRS && psp_dirs[fd] != NULL) {
        closedir(psp_dirs[fd]);
        psp_dirs[fd] = NULL;
        printf("[HLE] sceIoDclose | Fechou FD do diretório %d\n", fd);
        cpu->v0 = 0;
    } else {
        cpu->v0 = 0x80010009;
    }
}

// 1. ABRIR ARQUIVO
void hle_sceIoOpen(MIPS_CPU *cpu, uint8_t *mem) {
    char psp_path[256];
    char host_path[512];
    read_psp_string(psp_path, mem, cpu->a0, 256);
    translate_psp_path(psp_path, host_path);
    
    printf("[HLE IO] sceIoOpen | Pediu: '%s' -> Abrindo real: '%s'\n", psp_path, host_path);
    
    FILE *f = fopen(host_path, "rb");
    if (!f) {
        printf("[HLE IO ERRO] Arquivo não encontrado no PC!\n");
        cpu->v0 = 0x80020323; // SCE_KERNEL_ERROR_NOFILE
        return;
    }
    
    // Procura um slot livre na nossa tabela (começando do 10 para evitar conflitos com stdin/stdout)
    for (int i = 10; i < MAX_OPEN_FILES; i++) {
        if (psp_file_table[i] == NULL) {
            psp_file_table[i] = f;
            cpu->v0 = i; // Devolve o FD para o jogo
            return;
        }
    }
    fclose(f);
    cpu->v0 = 0x80020324; // MFILE (Muitos arquivos abertos)
}

// 2. LER ARQUIVO
void hle_sceIoRead(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    uint32_t data_addr = cpu->a1;
    int size = cpu->a2;
    
    if (fd >= 10 && fd < MAX_OPEN_FILES && psp_file_table[fd] != NULL) {
        // Usa o psp_translate_addr para saber onde escrever na RAM com segurança
        uint8_t *dest = &mem[psp_translate_addr(data_addr)];
        size_t read_bytes = fread(dest, 1, size, psp_file_table[fd]);
        cpu->v0 = read_bytes;
        printf("[HLE IO] sceIoRead | FD %d leu %zu bytes para a RAM.\n", fd, read_bytes);
    } else {
        cpu->v0 = 0x80020320; // BADF (Arquivo inválido)
    }
}

// 3. FECHAR ARQUIVO
void hle_sceIoClose(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    if (fd >= 10 && fd < MAX_OPEN_FILES && psp_file_table[fd] != NULL) {
        fclose(psp_file_table[fd]);
        psp_file_table[fd] = NULL;
        cpu->v0 = 0; // Sucesso
    } else {
        cpu->v0 = 0x80020320; 
    }
}

// 4. MOVER O PONTEIRO DE LEITURA (LSEEK)
void hle_sceIoLseek(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    // No PSP (O32 ABI), um int de 64-bits usa dois registradores (a2 e a3). 
    // a1 fica vazio por alinhamento!
    long offset = (long)cpu->a2; 
    int whence = cpu->a3;
    
    if (fd >= 10 && fd < MAX_OPEN_FILES && psp_file_table[fd] != NULL) {
        int seek_mode = SEEK_SET;
        if (whence == 1) seek_mode = SEEK_CUR;
        if (whence == 2) seek_mode = SEEK_END;
        
        fseek(psp_file_table[fd], offset, seek_mode);
        cpu->v0 = ftell(psp_file_table[fd]); // Retorna a nova posição
    } else {
        cpu->v0 = 0x80020320;
    }
}

// 5. INFORMAÇÕES DO ARQUIVO (GETSTAT)
void hle_sceIoGetstat(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t ptr_original = cpu->a0; // <-- Faltou esta linha aqui!
    
    char psp_path[256];
    char host_path[512];
    read_psp_string(psp_path, mem, cpu->a0, 256);
    translate_psp_path(psp_path, host_path);

    printf("[HLE IO] sceIoGetstat | Ponteiro: 0x%08X -> Leu: '%s'\n", ptr_original, psp_path);
    
    uint32_t stat_ptr = cpu->a1;
    struct stat st;
    
    if (stat(host_path, &st) == 0) {
        // Grava o tamanho do arquivo na struct (Offset 0x28 é st_size no PSP)
        MEM_W32(mem, stat_ptr + 0x28, st.st_size);
        cpu->v0 = 0; // Sucesso
        printf("[HLE IO] sceIoGetstat | '%s' existe. Tamanho: %ld bytes\n", host_path, st.st_size);
    } else {
        cpu->v0 = 0x80020323; // Arquivo não encontrado
    }
}

void hle_sceIoChangeAsyncPriority(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = (int)cpu->a0;
    int prio = (int)cpu->a1;
    
    if (fd < 0) {
        printf("[HLE IO ERRO] sceIoChangeAsyncPriority ignorado! O jogo mandou um erro (0x%08X) em vez de um FD!\n", fd);
        cpu->v0 = 0x80020320; // BADF
        return;
    }
    
    printf("[HLE IO] sceIoChangeAsyncPriority | FD: %d mudou para prioridade %d\n", fd, prio);
    cpu->v0 = 0; 
}

// ─── Threads e Scheduler ─────────────────────────────────────────────────────

void hle_sceKernelSetCompiledSdkVersion(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE] sceKernelSetCompiledSdkVersion | Ignorando checagem de versao.\n");
    cpu->v0 = 0;
}

void hle_sceKernelCreateThread(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t name_addr = cpu->a0;
    uint32_t entry_pc  = cpu->a1;
    int init_priority  = cpu->a2; // PSP passa a prioridade aqui!
    char name[64];
    
    read_psp_string(name, mem, name_addr, 64);
    name[63] = '\0';
    if (entry_pc < 0x08000000) entry_pc += 0x08800000;
    
    cpu->v0 = scheduler_create_thread(name, entry_pc, init_priority, cpu);
}

void hle_sceKernelStartThread(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t uid    = cpu->a0;
    uint32_t arglen = cpu->a1;
    uint32_t argp   = cpu->a2;

    for (int i = 0; i < thread_count; i++) {
        if (thread_pool[i].uid == uid) {
            thread_pool[i].context.a0 = arglen;
            thread_pool[i].context.a1 = argp;
            break;
        }
    }

    scheduler_start_thread(uid);

    if (current_thread_idx != -1 && thread_pool[current_thread_idx].uid == uid) {
        *cpu = thread_pool[current_thread_idx].context;
    }
    cpu->v0 = 0;
}

void hle_sceKernelDelayThread(MIPS_CPU *cpu, uint8_t *mem) {
    mem[0x0032E530 & 0x1FFFFFFF] = 0;
    scheduler_yield(cpu, cpu->a0 / 1000);
    cpu->v0 = 0;
}

void hle_sceKernelCreateSema(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t name_addr = cpu->a0;
    // a1 = attr (ignoramos), a2 = initCount, a3 = maxCount
    int init_count = (int)cpu->a2;
    int max_count  = (int)cpu->a3;

    char name[64];
    // FIX: ler via read_psp_string que sanitiza corretamente, com fallback
    if (name_addr != 0) {
        read_psp_string(name, mem, name_addr, 64);
        // Se leu lixo (string vazia após sanitização), marca como desconhecido
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "sema_0x%08X", name_addr);
        }
    } else {
        strcpy(name, "sema_sem_nome");
    }

    int uid = scheduler_create_sema(name, init_count, max_count);
    printf("[HLE] sceKernelCreateSema | a0=0x%08X nome='%s' init=%d max=%d -> UID %d\n",
           name_addr, name, init_count, max_count, uid);
    cpu->v0 = uid;
}

void hle_sceKernelWaitSema(MIPS_CPU *cpu, uint8_t *mem) {
    int sema_id = cpu->a0;
    int signal  = cpu->a1;

    int res = scheduler_wait_sema(sema_id, signal, cpu);

    if (res == 1) {
        printf("[HLE] sceKernelWaitSema | Thread bloqueada esperando Semáforo %d\n", sema_id);
    } else if (res == -1) {
        cpu->v0 = 0x800201B8;
        return;
    }
    cpu->v0 = 0;
}

void hle_sceKernelSignalSema(MIPS_CPU *cpu, uint8_t *mem) {
    int sema_id = cpu->a0;
    int signal  = cpu->a1;

    printf("[HLE] sceKernelSignalSema | Disparou Semáforo %d (+%d)\n", sema_id, signal);
    scheduler_signal_sema(sema_id, signal);
    cpu->v0 = 0;
}

void hle_sceKernelGetThreadId(MIPS_CPU *cpu, uint8_t *mem) {
    cpu->v0 = thread_pool[current_thread_idx].uid;
}

void hle_sceKernelLoadModule(MIPS_CPU *cpu, uint8_t *mem) {
    char name[256] = {0};
    if (cpu->a0 != 0) read_psp_string(name, mem, cpu->a0, 256);
    
    // Agora logamos o ponteiro bruto também!
    printf("[HLE] sceKernelLoadModule | Pediu para carregar: '%s' (Ponteiro a0: 0x%08X)\n", name, cpu->a0);
    cpu->v0 = 1; 
}

// Importa as funções geradas pelo Tradutor_PRX
extern void module_start_render_prx(MIPS_CPU *cpu, uint8_t *mem);
extern void module_start_audio_prx(MIPS_CPU *cpu, uint8_t *mem);

void hle_sceKernelStartModule(MIPS_CPU *cpu, uint8_t *mem) {
    // Permite que o fluxo nativo da Thread 1 alcance a GPU sozinho
    cpu->v0 = 0; 
}

void hle_DummySuccess(MIPS_CPU *cpu, uint8_t *mem) {
    cpu->v0 = 0;
}

// ─── Conversão de Tempo e Física (SysClock) ─────────────────────────────────
// No PSP, 1 SceSysClock = 1 Microsegundo (1 MHz). A conversão é 1:1!

// int sceKernelSysClock2USec(SceSysClock *sysclock, u32 *sec, u32 *usec)
void hle_sceKernelSysClock2USec(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t clock_ptr = cpu->a0;
    uint32_t sec_ptr   = cpu->a1;
    uint32_t usec_ptr  = cpu->a2;

    if (clock_ptr != 0) {
        // SceSysClock é um inteiro de 64-bits na memória
        uint64_t clock_val = MEM_R32(mem, clock_ptr) | ((uint64_t)MEM_R32(mem, clock_ptr + 4) << 32);
        
        uint32_t sec = (uint32_t)(clock_val / 1000000ULL);
        uint32_t usec = (uint32_t)(clock_val % 1000000ULL);

        if (sec_ptr)  MEM_W32(mem, sec_ptr, sec);
        if (usec_ptr) MEM_W32(mem, usec_ptr, usec);
    }
    cpu->v0 = 0;
}

// u64 sceKernelSysClock2USecWide(u32 clock_low, u32 clock_high) 
void hle_sceKernelSysClock2USecWide(MIPS_CPU *cpu, uint8_t *mem) {
    // Retorna um valor de 64-bits direto para os registradores v0 e v1.
    // Como a conversão é 1:1, só devolvemos o que entrou nos argumentos a0 e a1!
    cpu->v0 = cpu->a0;
    cpu->v1 = cpu->a1;
}

// int sceKernelUSec2SysClock(u32 usec, SceSysClock *clock)
void hle_sceKernelUSec2SysClock(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t usec = cpu->a0;
    uint32_t clock_ptr = cpu->a1;

    if (clock_ptr != 0) {
        MEM_W32(mem, clock_ptr, usec);
        MEM_W32(mem, clock_ptr + 4, 0); // Zera a parte alta porque a entrada é de 32-bits
    }
    cpu->v0 = 0;
}

// u64 sceKernelUSec2SysClockWide(u32 usec)
void hle_sceKernelUSec2SysClockWide(MIPS_CPU *cpu, uint8_t *mem) {
    cpu->v0 = cpu->a0;
    cpu->v1 = 0;
}

// ─── GPU Stubs ───────────────────────────────────────────────────────────────

void hle_sceGeListEnQueue(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE GPU] sceGeListEnQueue | Fila de graficos recebida. Retornando ID 1.\n");
    cpu->v0 = 1;
}

// Variável global para rastrear o último tempo de VBlank no mundo real
static struct timeval last_vblank_time = {0};

// Em syscalls.c
void hle_sceDisplayWaitVblankStart(MIPS_CPU *cpu, uint8_t *mem) {
    struct timeval now;
    gettimeofday(&now, NULL);

    if (last_vblank_time.tv_sec == 0) {
        last_vblank_time = now;
    }

    long elapsed = (now.tv_sec - last_vblank_time.tv_sec) * 1000000 +
                   (now.tv_usec - last_vblank_time.tv_usec);

    // 1 frame a 60Hz leva ~16666 microssegundos
    if (elapsed < 16666) {
        // O VBlank real ainda não aconteceu. 
        // Em vez de congelar com usleep, pausamos APENAS esta thread do PSP!
        cpu->zero = 1; // Flag secreta pro Dispatcher repetir esta syscall
        scheduler_yield(cpu, 100); // Deixa as threads de I/O e Áudio respirarem
        return; 
    }

    // Bateu os 16.6ms! O "monitor" do PSP atualizou.
    gettimeofday(&last_vblank_time, NULL);
    
    // Atualiza os inputs do controle uma vez por frame
    ctrl_update(mem);

    cpu->v0 = 0; // Retorna Sucesso pro Kratos
}


/* --- FUNÇÕES DA GRAPHICS ENGINE (GE) E DISPLAY --- */

// nid_0x1F6752AD
void hle_sceGeEdramGetSize(MIPS_CPU *cpu, uint8_t *mem) {
    // O PSP original tem 2MB (0x00200000 bytes) de EDRAM.
    // Retornamos esse valor exato para o Kratos não surtar.
    printf("[HLE GPU] sceGeEdramGetSize | O jogo perguntou o tamanho da VRAM. Retornando 2MB.\n");
    cpu->v0 = 0x00200000;
}

// nid_0xE47E40E4
void hle_sceGeEdramSetAddrTranslation(MIPS_CPU *cpu, uint8_t *mem) {
    // Define a largura da VRAM (normalmente 512 pixels).
    printf("[HLE GPU] sceGeEdramSetAddrTranslation | Largura configurada para: %d\n", cpu->a0);
    cpu->v0 = 0; // Sucesso
}

// nid_0x24FD7BCF
void hle_sceGeSetCallback(MIPS_CPU *cpu, uint8_t *mem) {
    // O jogo registra um callback para a GPU.
    // Retornamos um ID falso válido (ex: 1) para ele achar que registrou.
    printf("[HLE GPU] sceGeSetCallback | Callback registrado.\n");
    cpu->v0 = 1; 
}

// nid_0x0E20F177
void hle_sceDisplaySetMode(MIPS_CPU *cpu, uint8_t *mem) {
    int mode   = cpu->a0;
    int width  = cpu->a1;
    int height = cpu->a2;
    printf("[HLE GPU] sceDisplaySetMode | Tela configurada: Modo %d, Resolucao %dx%d\n", mode, width, height);
    cpu->v0 = 0;
}

// nid_0xB77905EA
void hle_sceDisplayWaitVblankStartMulti(MIPS_CPU *cpu, uint8_t *mem) {
    // 1. Executa o freio de 60FPS que você já tem
    hle_sceDisplayWaitVblankStart(cpu, mem);

    // 2. O PULO DO GATO: God of War checa se o campo v0 é 0.
    // Algumas versões da engine esperam um valor positivo para indicar "Ready".
    cpu->v0 = 0; 
}

// nid_0x36AA6E91 - sceGeSetCmdList
void hle_sceGeSetCmdList(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t list_addr = cpu->a0;
    printf("[HLE GPU] sceGeSetCmdList | Lista em: 0x%08X. Forçando ID de conclusão 1.\n", list_addr);
    
    // Retornamos um ID de lista (1). 
    // O jogo vai usar esse ID depois para perguntar "A lista 1 já terminou?"
    cpu->v0 = 1; 
}

// VAMOS CRIAR ESTA NOVA PARA MATAR O LOOP:
// nid_0xE0BB3D96 ou nid_0x03444EB4 (Depende da versão, mapeie ambas se aparecerem)
void hle_sceGeListSync(MIPS_CPU *cpu, uint8_t *mem) {
    // Esta é a função que o jogo chama para saber se a GPU terminou.
    // Retornamos 0 para dizer "Sim, a GPU está ociosa/terminou o desenho".
    printf("[HLE GPU] sceGeListSync | Sincronizando... Retornando 0 (Pronto).\n");
    cpu->v0 = 0;
}

// ─── Event Flags ─────────────────────────────────────────────────────────────

extern int scheduler_create_evflag(const char *name, int attr, uint32_t init_pattern);
extern void scheduler_set_evflag(int uid, uint32_t bits, MIPS_CPU *current_cpu);

void hle_sceKernelCreateEventFlag(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t name_addr    = cpu->a0;
    int attr              = cpu->a1;
    uint32_t init_pattern = cpu->a2;
    char name[64] = {0};

    if (name_addr) {
        read_psp_string(name, mem, name_addr, 64);
    } else {
        strcpy(name, "EvFlag_SemNome");
    }

    int uid = scheduler_create_evflag(name, attr, init_pattern);
    cpu->v0 = uid;
    printf("[HLE] sceKernelCreateEventFlag | Nome: '%s', Init: 0x%08X -> Retornou UID %d\n", name, init_pattern, uid);
}

void hle_sceKernelSetEventFlag(MIPS_CPU *cpu, uint8_t *mem) {
    int uid = cpu->a0;
    uint32_t bits = cpu->a1;
    
    scheduler_set_evflag(uid, bits, cpu);
    cpu->v0 = 0;
}

void hle_sceKernelWaitEventFlag(MIPS_CPU *cpu, uint8_t *mem) {
    int evid      = (int)cpu->a0;
    uint32_t bits = cpu->a1;
    int wait_type = (int)cpu->a2;
    uint32_t out_ptr = cpu->a3;

    uint32_t out_bits = 0;
    extern int scheduler_wait_evflag(int, uint32_t, int, uint32_t*, MIPS_CPU*);
    int res = scheduler_wait_evflag(evid, bits, wait_type, &out_bits, cpu);

    if (res == 0) {
        if (out_ptr) MEM_W32(mem, out_ptr, out_bits);
        cpu->v0 = 0;
    } else if (res == 1) {
        cpu->v0 = 0; 
    } else {
        cpu->v0 = 0x800201C8; 
        scheduler_yield(cpu, 0); 
    }
}

// ─── Syscall por opcode MIPS (antes era stub vazio) ──────────────────────────
// FIX: agora loga em vez de engolir silenciosamente
void psp_syscall_mips(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    printf("[WARN] SYSCALL nativa por opcode: code=0x%05X (não implementada) | a0=0x%08X\n",
           code, cpu->a0);
    cpu->v0 = 0;
}

// ─── Motor de I/O Assíncrono Nativo (POSIX Threads) ──────────────────────────

typedef struct {
    pthread_t thread;
    int active;
    int fd;
    uint8_t *mem;
    uint32_t data_ptr;
    size_t size;
    ssize_t result;
    volatile int done; // Flag atômica de conclusão
} AsyncIOReq;

AsyncIOReq async_reqs[MAX_FDS] = {0};

// Esta função roda em um NÚCLEO SEPARADO do processador do PC/Servidor
void* io_read_worker(void* arg) {
    AsyncIOReq *req = (AsyncIOReq*)arg;
    
    // O Linux faz o trabalho pesado de I/O em paralelo com a CPU MIPS
    req->result = read(psp_fds[req->fd], &req->mem[req->data_ptr & 0x01FFFFFF], req->size);
    
    req->done = 1; // Avisa que terminou
    return NULL;
}

void hle_sceIoReadAsync(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    uint32_t data_ptr = cpu->a1;
    size_t size = cpu->a2;

    if (fd > 0 && fd < MAX_FDS && psp_fds[fd] != 0) {
        if (async_reqs[fd].active) {
            pthread_join(async_reqs[fd].thread, NULL); // Limpa resíduo anterior
        }
        async_reqs[fd].active = 1;
        async_reqs[fd].done = 0;
        async_reqs[fd].fd = fd;
        async_reqs[fd].mem = mem;
        async_reqs[fd].data_ptr = data_ptr;
        async_reqs[fd].size = size;

        // Lança a thread de hardware do sistema operacional!
        pthread_create(&async_reqs[fd].thread, NULL, io_read_worker, &async_reqs[fd]);
        
        printf("[HLE] sceIoReadAsync | FD %d lendo 0x%zX bytes em uma Thread nativa ARM64...\n", fd, size);
        cpu->v0 = 0; // Retorna imediatamente (Não bloqueia o PSP!)
    } else {
        cpu->v0 = 0x80010009; // EBADF
    }
}

void hle_sceIoPollAsync(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    uint32_t out_ptr = cpu->a1; // Ponteiro SceInt64

    if (fd > 0 && fd < MAX_FDS && async_reqs[fd].active) {
        if (async_reqs[fd].done) {
            pthread_join(async_reqs[fd].thread, NULL);
            async_reqs[fd].active = 0;
            if (out_ptr) {
                MEM_W32(mem, out_ptr, (uint32_t)async_reqs[fd].result);
                MEM_W32(mem, out_ptr + 4, 0); // 64-bit clear
            }
            cpu->v0 = 0;
        } else {
            cpu->v0 = 1; // 1 = Ocupado / Lendo
        }
    } else {
        cpu->v0 = 0x80010009;
    }
}

void hle_sceIoWaitAsync(MIPS_CPU *cpu, uint8_t *mem) {
    int fd = cpu->a0;
    uint32_t out_ptr = cpu->a1;

    if (fd > 0 && fd < MAX_FDS && async_reqs[fd].active) {
        if (!async_reqs[fd].done) {
            // A thread do Linux ainda está lendo o disco!
            // Para não congelar o emulador inteiro com um "while", nós usamos
            // o HLE Yield: Suspendemos a thread MIPS atual e voltamos na próxima.
            cpu->zero = 1; // FLAG SECRETA de hardware para o Dispatcher
            scheduler_yield(cpu, 100); 
        } else {
            // Terminou de ler em background!
            pthread_join(async_reqs[fd].thread, NULL);
            async_reqs[fd].active = 0;
            if (out_ptr) {
                MEM_W32(mem, out_ptr, (uint32_t)async_reqs[fd].result);
                MEM_W32(mem, out_ptr + 4, 0);
            }
            printf("[HLE] sceIoWaitAsync | Leitura background do FD %d FINALIZADA! (%zd bytes)\n", fd, async_reqs[fd].result);
            cpu->v0 = 0;
        }
    } else {
        cpu->v0 = 0x80010009;
    }
}

// ─── Gerenciador de Memória do Usuário (SysMemUser) ─────────────────────────
#define MAX_MEM_BLOCKS 64
uint32_t mem_blocks[MAX_MEM_BLOCKS] = {0};
int mem_block_count = 1; // UID não pode ser 0

// Mudamos a Heap para começar DEPOIS de 5MB. 
// O EBOOT tem 3.5MB, então agora ele está 100% blindado contra canibalismo!
uint32_t user_mem_bump = 0x08D00000;

void hle_sceKernelAllocPartitionMemory(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t size = cpu->a3;
    
    // Alinha o tamanho para blocos de 256 bytes (padrão PSP)
    if (size % 256 != 0) size = (size + 255) & ~255;

    if (mem_block_count >= MAX_MEM_BLOCKS) {
        cpu->v0 = 0x800200D9; // Erro: Out of memory
        return;
    }

    int uid = mem_block_count++;
    mem_blocks[uid] = user_mem_bump;
    user_mem_bump += size; // Avança o ponteiro livre para a próxima alocação

    cpu->v0 = uid; // O PSP retorna um UID (ID do bloco), não o ponteiro direto!
    printf("[HLE] sceKernelAllocPartitionMemory | Tamanho pedido: %u bytes -> Retornou UID %d (Em 0x%08X)\n", size, uid, mem_blocks[uid]);
}

void hle_sceKernelGetBlockHeadAddr(MIPS_CPU *cpu, uint8_t *mem) {
    int uid = cpu->a0;
    
    if (uid > 0 && uid < mem_block_count) {
        cpu->v0 = mem_blocks[uid]; // Transforma o UID de volta no ponteiro real de RAM
        printf("[HLE] sceKernelGetBlockHeadAddr | UID %d -> Retornou Ponteiro 0x%08X\n", uid, cpu->v0);
    } else {
        cpu->v0 = 0;
        printf("[HLE ERRO] sceKernelGetBlockHeadAddr | UID %d invalido!\n", uid);
    }
}

///---------------
extern void scheduler_sleep_thread(MIPS_CPU *current_cpu);
extern int scheduler_wakeup_thread(int uid);

void hle_sceKernelSleepThread(MIPS_CPU *cpu, uint8_t *mem) {
    scheduler_sleep_thread(cpu);
    // Nota: O v0 é setado dentro do scheduler no caso de crédito.
    // Mas se ele dormiu, o yield cuidará de salvar o contexto.
    cpu->v0 = 0; 
}

void hle_sceKernelSleepThreadCB(MIPS_CPU *cpu, uint8_t *mem) {
    // Comporta-se igual ao Sleep normal na nossa emulação atual
    scheduler_sleep_thread(cpu);
    cpu->v0 = 0;
}

void hle_sceKernelWakeupThread(MIPS_CPU *cpu, uint8_t *mem) {
    int uid = cpu->a0;
    cpu->v0 = scheduler_wakeup_thread(uid);
}

extern void scheduler_change_priority(int uid, int priority, MIPS_CPU *current_cpu);

void hle_sceKernelChangeThreadPriority(MIPS_CPU *cpu, uint8_t *mem) {
    int uid = cpu->a0;
    int priority = cpu->a1;
    scheduler_change_priority(uid, priority, cpu);
    cpu->v0 = 0;
}

extern void hle_sceDisplaySetFrameBuf(MIPS_CPU *cpu, uint8_t *mem);

// nid_0x383F7BCC
void hle_sceRtcGetCurrentTick(MIPS_CPU *cpu, uint8_t *mem) {
    // O jogo passa no a0 o endereço de memória onde quer que gravemos o "Tick" de 64 bits.
    static uint64_t fake_ticks = 0;
    fake_ticks += 16666; // Simula a passagem de ~16ms (1 frame a 60FPS) por cada chamada
    
    uint32_t addr = cpu->a0;
    if (addr != 0) {
        // Grava os 64 bits na memória RAM do PSP (dividido em dois blocos de 32 bits)
        MEM_W32(mem, addr, (uint32_t)(fake_ticks & 0xFFFFFFFF));
        MEM_W32(mem, addr + 4, (uint32_t)(fake_ticks >> 32));
    }
    
    cpu->v0 = 0; // Retorna sucesso
    printf("[HLE RTC] sceRtcGetCurrentTick | Tempo simulado enviado para 0x%08X\n", addr);
}

// ─── Tabela de Despacho ───────────────────────────────────────────────────────

SyscallEntry hle_table[] = {
    // I/O
    {"sceIoOpen",    hle_sceIoOpen},
    {"sceIoClose",   hle_sceIoClose},
    {"sceIoRead",    hle_sceIoRead},
    {"sceIoLseek",   hle_sceIoLseek},
    {"sceIoDread",   hle_sceIoDread},
    {"nid_0x109F50BC",           hle_sceIoGetstat},
    {"nid_0xB293727F",           hle_sceIoChangeAsyncPriority},

    // Threads
    {"sceKernelCreateThread",     hle_sceKernelCreateThread},
    {"sceKernelStartThread",      hle_sceKernelStartThread},
    {"sceKernelDelayThread",      hle_sceKernelDelayThread},
    {"sceKernelDelayThreadCB",    hle_sceKernelDelayThread},
    {"nid_0xEBD177D6",            hle_sceKernelDelayThread},
    {"sceKernelGetThreadId",      hle_sceKernelGetThreadId},
    {"nid_0x293B45B8",            hle_sceKernelGetThreadId},
    {"sceKernelSleepThread",      hle_sceKernelSleepThread},
    {"sceKernelSleepThreadCB",    hle_sceKernelSleepThreadCB},
    {"nid_0x82826F70",            hle_sceKernelSleepThreadCB}, // Alias do SleepCB
    {"sceKernelWakeupThread",     hle_sceKernelWakeupThread},
    {"nid_0xD59EAD2F",            hle_sceKernelWakeupThread},  // Alias do Wakeup
    {"sceKernelChangeThreadPriority",    hle_sceKernelChangeThreadPriority},
    {"nid_0x71BC9871",                   hle_sceKernelChangeThreadPriority},

    // Semáforos
    {"sceKernelCreateSema",  hle_sceKernelCreateSema},
    {"nid_0x82BC5777",       hle_sceKernelCreateSema},
    {"sceKernelWaitSema",    hle_sceKernelWaitSema},
    {"nid_0x46EBB729",       hle_sceKernelWaitSema},
    {"sceKernelSignalSema",  hle_sceKernelSignalSema},
    {"nid_0x3F53E640",       hle_sceKernelSignalSema},

    // Módulos
    {"sceKernelLoadModule",  hle_sceKernelLoadModule},
    {"sceKernelStartModule", hle_sceKernelStartModule},

    // GPU
    {"sceGeListEnQueue", hle_sceGeListEnQueue},
    {"sceGeListSync",    hle_sceGeListSync},

    // Dummies (LIMPOS DOS RELÓGIOS)
    {"sceKernelSetCompiledSdkVersion",   hle_DummySuccess},
    {"nid_0x342061E5",                   hle_DummySuccess},
    {"nid_0xF77D77CB",                   hle_DummySuccess},
    {"sceKernelRegisterExitCallback",    hle_DummySuccess},
    {"sceKernelSleepThreadCB",           hle_DummySuccess},
    {"nid_0x03444EB4",                   hle_sceDisplaySetFrameBuf},
    {"nid_0xAB49E76A",                   hle_sceDisplayWaitVblankStart},
    {"nid_0x4AC57943",                   hle_DummySuccess},
    {"nid_0x82826F70",                   hle_DummySuccess},
    {"sceKernelSetCompiledSdkVersion_2", hle_DummySuccess},

    // sceCtrl
    {"nid_0x6A2774F3", hle_sceCtrlSetSamplingCycle},
    {"nid_0xA7144800", hle_sceCtrlSetSamplingMode},
    {"nid_0x1F4011E6", hle_sceCtrlReadBufferPositive},
    {"nid_0x3A622550", hle_sceCtrlPeekBufferPositive},

    // UMD e Diretórios
    {"nid_0xC6183D47", hle_sceUmdActivate},
    {"nid_0x56202973", hle_sceUmdWaitDriveStatWithTimer},
    {"nid_0x55F4717D", hle_sceIoChdir},
    {"nid_0xB29DDF9C", hle_sceIoDopen},
    {"nid_0xE3EB004C", hle_sceIoDread},
    {"nid_0xEB092469", hle_sceIoDclose},

    // Event Flags
    {"nid_0xEF9E4C70",       hle_sceKernelCreateEventFlag},
    {"nid_0x3E0271D3",       hle_sceKernelWaitEventFlag},
    {"nid_0x1FB15A32",       hle_sceKernelSetEventFlag},
    {"nid_0x402FCF22",       hle_sceKernelWaitEventFlag},
    {"sceKernelWaitEventFlag", hle_sceKernelWaitEventFlag},

    // Assync I/O (POSIX Threads)
    {"nid_0xCEADEB47", hle_sceKernelDelayThread}, 
    {"nid_0xA0B5A7C2", hle_sceIoReadAsync},       
    {"nid_0xE23EEC33", hle_sceIoWaitAsync},       
    {"nid_0x3251EA56", hle_sceIoPollAsync},

    // Memória (SysMemUserForUser)
    {"nid_0x237DBD4F", hle_sceKernelAllocPartitionMemory},
    {"nid_0x9D9A5BA1", hle_sceKernelGetBlockHeadAddr},
    
    // Clocks e Tempo (Física do Jogo)
    {"sceKernelSysClock2USec", hle_sceKernelSysClock2USec},
    {"nid_0xDFA8BAF8",         hle_sceKernelSysClock2USecWide},
    {"sceKernelUSec2SysClock", hle_sceKernelUSec2SysClock},
    {"nid_0xEDBA5844",         hle_sceKernelUSec2SysClockWide},

    {"nid_0xEADB1BD7", hle_DummySuccess}, // sceKernelPowerLock
    {"nid_0x3AEE7261", hle_DummySuccess}, // sceKernelPowerUnlock

    {"nid_0x1F6752AD", hle_sceGeEdramGetSize},
    {"nid_0xE47E40E4", hle_sceGeEdramSetAddrTranslation},
    {"nid_0x24FD7BCF", hle_sceGeSetCallback},
    {"nid_0x36AA6E91", hle_sceGeSetCmdList},
    {"nid_0x0E20F177", hle_sceDisplaySetMode},
    
    // Use seu hle_sceDisplaySetFrameBuf que já está no gpu.c para esta:
    {"nid_0x289D82FE", hle_sceDisplaySetFrameBuf},
    {"nid_0xB77905EA", hle_sceDisplayWaitVblankStartMulti},
    {"nid_0x03444EB4", hle_sceGeListSync}, // sceGeListSync
    {"nid_0xE0BB3D96", hle_sceGeListSync}, // Nome alternativo
    
    {"nid_0x383F7BCC", hle_sceRtcGetCurrentTick},
};

#define HLE_TABLE_SIZE (sizeof(hle_table) / sizeof(SyscallEntry))

// ─── Motor de Busca ───────────────────────────────────────────────────────────

void execute_hle_call(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    for (int i = 0; i < HLE_TABLE_SIZE; i++) {
        if (strcmp(name, hle_table[i].name) == 0) {
            hle_table[i].handler(cpu, mem);
            return;
        }
    }
    printf("[SYSCALL NÃO IMPLEMENTADA] %s | a0=0x%08X a1=0x%08X\n", name, cpu->a0, cpu->a1);
    cpu->v0 = 0;
}

void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    execute_hle_call(cpu, mem, name);
}

void real_hle_call(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    execute_hle_call(cpu, mem, name);
}