/**
 * runtime/loader.c
 * Carrega o EBOOT.BIN na memória emulada e inicia o motor (Trampoline).
 */

#include "cpu.h"
#include "memory.h"
#include "../out/func_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef PSP_STACK_TOP
#define PSP_STACK_TOP 0x09FFF000
#endif

/* ── Leitura de campos ELF ───────────────────────────────────────────────── */
static uint16_t r16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t r32(const uint8_t *b) { return (uint32_t)(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24)); }

/* ── Leitura simples do entry point do JSON ───────────────────────────────── */
static uint32_t read_entry_from_json(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t len = fread(buf, 1, sizeof(buf)-1, f);
    buf[len] = '\0';
    fclose(f);

    // Procura por "entry": seguido de número
    char *p = strstr(buf, "\"entry\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    return (uint32_t)strtoul(p+1, NULL, 0);
}

/* ── Loader ELF (Opcional, caso queira carregar o ELF original) ──────────── */
typedef struct { uint32_t entry; uint32_t gp_value; } LoadResult;

/* ── Entry point principal do Recompilador ───────────────────────────────── */
int main(int argc, char **argv) {
    printf("=== PSP Recomp Engine ===\n");
    printf("Jogo: God of War: Chains of Olympus\n\n");

    /* Aloca 32MB de RAM */
    uint8_t *mem = mem_init();

    /* Carrega imagem já relocada */
    FILE *f = fopen("elf_meta_image.bin", "rb");
    if (!f) {
        printf("[LOADER] Erro abrindo elf_meta_image.bin. Rode o script Python primeiro!\n");
        exit(1);
    }

    /* CORREÇÃO DO AVISO DO FREAD */
    size_t lidos = fread(mem, 1, PSP_MEM_SIZE, f);
    if (lidos == 0) { printf("[AVISO] O binário lido tem 0 bytes!\n"); }
    fclose(f);

    /* Lê o endereço exato de entrada do JSON gerado pelo parse_elf.py */
    uint32_t entry = read_entry_from_json("elf_meta.json");
    if (entry == 0) {
        printf("[ERRO] Não conseguiu ler entry point de elf_meta.json!\n");
        printf("Rode: python parse_elf.py EBOOT.BIN\n");
        exit(1);
    }

    /* Inicializa CPU */
    MIPS_CPU cpu;
    cpu_init(&cpu, entry, PSP_STACK_TOP);
    printf("[LOADER] Iniciando Trampoline Dispatcher em 0x%08X...\n", entry);

    /* 
     * O CORAÇÃO DA EMULAÇÃO
     * O jogo vai rodar aqui dentro pra sempre (até pedir pra sair).
     */
    while (cpu.running) {
        cpu.zero = 0; // Proteção absoluta do registrador zero
        cpu_dispatch(&cpu, mem, cpu.pc);
    }

    printf("\n[LOADER] Execução finalizada graciosamente.\n");
    mem_free(mem);
    return 0;
}