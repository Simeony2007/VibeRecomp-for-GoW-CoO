#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>   // Para open()
#include <unistd.h>  // Para read(), close()
#include "scheduler.h"

// Snapshots e Gerenciamento de Threads
MIPS_CPU main_thread_snapshot;
uint32_t saved_main_pc = 0;
static int thread_count = 0;
static int started_threads = 0;

// Segurança: Array de threads com limite definido
#define MAX_THREADS 16
uint32_t threads_entry[MAX_THREADS] = {0};

// FDs (File Descriptors) do PSP começam em 1
#define MAX_FDS 32
int psp_fds[MAX_FDS] = {0}; 

void hle_sceIoOpen(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t file_addr = cpu->a0; 
    uint32_t flags     = cpu->a1; 

    // Extrai o nome do arquivo da memória
    char psp_path[256];
    strncpy(psp_path, (char*)&mem[file_addr & 0x1FFFFFFF], 255);
    psp_path[255] = '\0';

    // O God of War costuma pedir algo como "host0:/USRDIR/GOW.ARC"
    // Vamos converter isso para procurar em uma pasta local chamada "GOW_DATA"
    char local_path[512] = "./GOW_DATA/";
    char *filename = strrchr(psp_path, '/');
    
    if (filename) {
        strcat(local_path, filename + 1); // Pega só o nome final do arquivo
    } else {
        strcat(local_path, psp_path);
    }

    printf("[HLE] sceIoOpen | Traduzindo '%s' -> '%s'\n", psp_path, local_path);

    // Tenta abrir o arquivo real no seu sistema
    int fd = open(local_path, O_RDONLY); // Modo leitura
    if (fd >= 0) {
        for (int i = 1; i < MAX_FDS; i++) {
            if (psp_fds[i] == 0) {
                psp_fds[i] = fd; // Salva o FD real no slot do PSP
                cpu->v0 = i;     // Retorna o FD simulado
                printf("      -> [SUCESSO] Arquivo aberto no slot FD %d!\n", i);
                return;
            }
        }
    }
    
    printf("      -> [ERRO] Arquivo não encontrado no disco!\n");
    cpu->v0 = 0x80010002; // Erro padrão do PSP: Arquivo inexistente
}

/**
 * sceKernelSetCompiledSdkVersion
 * O jogo informa a versão do SDK para a qual foi compilado.
 * Retornar 0 indica que o nosso "Kernel" aceita essa versão.
 */
void hle_sceKernelSetCompiledSdkVersion(MIPS_CPU *cpu) {
    uint32_t sdk_ver = cpu->a0;
    // Logamos para ter certeza de que o valor está chegando certo nos registradores
    printf("[HLE] sceKernelSetCompiledSdkVersion | Versão do SDK: 0x%08X\n", sdk_ver);
    
    // Retorno de Sucesso
    cpu->v0 = 0; 
}

/* --- HLE: Implementações Reais do Sistema Operacional --- */

/**
 * sceKernelCreateThread
 * Aloca uma nova thread no sistema e retorna o UID dela.
 */
void hle_sceKernelCreateThread(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t name_addr = cpu->a0;  
    uint32_t entry_pc  = cpu->a1;  
    
    char name[64];
    strncpy(name, (char*)&mem[name_addr & 0x1FFFFFFF], 63);
    name[63] = '\0';
    
    if (entry_pc < 0x08000000) entry_pc += 0x08800000;

    // Delega para o Escalonador Real
    cpu->v0 = scheduler_create_thread(name, entry_pc, cpu);
}

void hle_sceKernelStartThread(MIPS_CPU *cpu) {
    uint32_t uid = cpu->a0;
    scheduler_start_thread(uid);
    
    // Se a thread recém-iniciada assumiu o controle (idx 0), carregamos o contexto dela
    if (current_thread_idx != -1 && thread_pool[current_thread_idx].uid == uid) {
        *cpu = thread_pool[current_thread_idx].context;
    }
    cpu->v0 = 0;
}

void hle_sceKernelDelayThread(MIPS_CPU *cpu) {
    uint32_t microseconds = cpu->a0;
    // O jogo quer dormir. Nós cedemos o controle da CPU para outra thread.
    // (Dividimos por um valor arbitrário apenas para simular ciclos de emulador)
    scheduler_yield(cpu, microseconds / 1000); 
}

// --- O ROTEADOR HLE DEFINITIVO ---
void real_hle_call(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    if (strcmp(name, "sceKernelCreateThread") == 0) {
        hle_sceKernelCreateThread(cpu, mem);
    }
    else if (strcmp(name, "sceKernelStartThread") == 0) {
        hle_sceKernelStartThread(cpu);
    }
    else if (strcmp(name, "sceKernelDelayThread") == 0) {
        // 1. Zera a flag de carregamento para o Kratos sair do loop quando acordar
        mem[0x0032E530 & 0x1FFFFFFF] = 0; 
        
        // 2. Chama a função que faz o scheduler_yield(cpu, microsegundos)
        hle_sceKernelDelayThread(cpu); 
    }
    else if (strcmp(name, "sceIoOpen") == 0) {
        hle_sceIoOpen(cpu, mem);
    }
    else if (strcmp(name, "sceKernelSetCompiledSdkVersion") == 0 || 
             strcmp(name, "nid_0x342061E5") == 0 || 
             strcmp(name, "nid_0xF77D77CB") == 0) {
        // SDK Version Checkers: Ignora e retorna sucesso absoluto.
        cpu->v0 = 0;
    }
    else if (strcmp(name, "sceKernelCreateSema") == 0 || strcmp(name, "nid_0x82BC5777") == 0) {
        printf("[HLE] sceKernelCreateSema | Semaforo criado com sucesso.\n");
        cpu->v0 = 0x10850014; // Retorna o ID que o Kratos quer
    }
    else if (strcmp(name, "sceKernelWaitSema") == 0 || strcmp(name, "nid_0x46EBB729") == 0) {
        printf("[HLE] sceKernelWaitSema | Kratos aguardando sincronizacao da Engine...\n");
        
        // HACK DE ENGENHARIA REVERSA:
        // A func_0899F118 exige que o bit 0x08 do endereço 0x00017A38 esteja ligado.
        // Se escrevermos isso na memória, o loop interno do jogo quebra na hora!
        mem[0x00017A38 & 0x1FFFFFFF] |= 0x08; 
        
        cpu->v0 = 0; // Retorna sucesso para a Syscall
    }
    else if (strcmp(name, "sceKernelDelayThreadCB") == 0 || strcmp(name, "nid_0xEBD177D6") == 0) {
        hle_sceKernelDelayThread(cpu); // Dorme um pouco
    }
    else if (strcmp(name, "sceKernelGetThreadId") == 0 || strcmp(name, "nid_0x293B45B8") == 0) {
        cpu->v0 = thread_pool[current_thread_idx].uid; // Devolve o UID do escalonador
    }
    else if (strcmp(name, "sceKernelSysClock2USec") == 0 || strcmp(name, "nid_0xDFA8BAF8") == 0 || 
             strcmp(name, "sceKernelUSec2SysClock") == 0 || strcmp(name, "nid_0xEDBA5844") == 0 ||
             strcmp(name, "sceKernelChangeThreadPriority") == 0 || strcmp(name, "nid_0x71BC9871") == 0) {
        // Funções de conversão matemática e prioridade: a gente só diz "Ok, chefe!"
        cpu->v0 = 0; 
    }
    else if (strcmp(name, "sceKernelLoadModule") == 0) {
        printf("[HLE] sceKernelLoadModule | Kratos pediu para carregar um modulo...\n");
        cpu->v0 = 1; // Retorna ID de sucesso
    }
    else if (strcmp(name, "sceKernelStartModule") == 0) {
        printf("[HLE] sceKernelStartModule | Módulo iniciado. Injetando flag de concluido!\n");
        
        // O Kratos criou uma struct na memória para monitorar o carregamento.
        // O offset 0x0032E530 é o campo "is_finished". 
        // Escrever 0 aqui faz a Thread passar direto pelo loop sem precisar dormir!
        mem[0x0032E530 & 0x1FFFFFFF] = 0; 
        
        cpu->v0 = 0; // Retorna sucesso
    }
    else if (strcmp(name, "sceKernelDelayThread") == 0) {
        printf("[HLE] DelayThread | Kratos pediu para dormir por %d microsegundos\n", cpu->a0);
        hle_sceKernelDelayThread(cpu);
    }
    else {
        // O CATCH-ALL: Mostra o que o jogo pediu mas ainda não programamos!
        printf("[SYSCALL NÃO IMPLEMENTADA] %s | a0=0x%08X a1=0x%08X\n", name, cpu->a0, cpu->a1);
        cpu->v0 = 0; // Finge sucesso pra ele não travar e continuar o boot
    }
}

// --- O BURACO NEGRO ---
// Essa função continua existindo só para os arquivos .c antigos não darem erro de compilação,
// mas ela não faz ABSOLUTAMENTE NADA. Ignora os falsos positivos!
void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, const char *name) {
    // Shhh... o Kratos está dormindo.
}

void psp_syscall_mips(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    // Placeholder para syscalls executadas diretamente via opcode no MIPS.
    // Por enquanto, apenas ignoramos silenciosamente para não travar a execução.
}