#include "runtime/cpu.h"
#include "runtime/memory.h"
#include "out/func_table.h"
#include <stdio.h>  // Para printf

// Função simples para ler entry do JSON (igual ao loader.c)
static uint32_t read_entry_from_json(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t len = fread(buf, 1, sizeof(buf)-1, f);
    buf[len] = '\0';
    fclose(f);

    char *p = strstr(buf, "\"entry\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    return (uint32_t)strtoul(p+1, NULL, 0);
}

int main() {
    uint8_t *mem = mem_init();
    MIPS_CPU cpu;
    
    // Lê entry point do JSON gerado pelo parse_elf.py
    uint32_t entry = read_entry_from_json("elf_meta.json");
    if (entry == 0) {
        printf("[ERRO] Não conseguiu ler entry point de elf_meta.json!\n");
        printf("Rode: python parse_elf.py EBOOT.BIN\n");
        return 1;
    }

    // Configuração inicial
    memset(&cpu, 0, sizeof(MIPS_CPU));
    cpu.sp = 0x09FF0000; // Topo da RAM
    cpu.pc = entry;      // Usa o entry correto
    cpu.running = 1;

    printf("[RECOMP] Iniciando execução a partir de 0x%08X...\n", entry);

    while (cpu.running) {
        cpu.zero = 0; // Garante que $zero seja sempre 0
        cpu_dispatch(&cpu, mem, cpu.pc);
    }

    printf("[RECOMP] Execução finalizada.\n");
    mem_free(mem);
    return 0;
}