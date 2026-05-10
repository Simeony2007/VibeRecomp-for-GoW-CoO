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
#include "dispatcher.h"

// 1. Declare a variável global para o GP aqui
uint32_t global_gp = 0; 

#ifndef PSP_STACK_TOP
#define PSP_STACK_TOP 0x09FFF000
#endif

// 2. Função de leitura de JSON mantida intacta
static uint32_t read_json_field(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[8192]; 
    size_t len = fread(buf, 1, sizeof(buf)-1, f);
    buf[len] = '\0';
    fclose(f);

    char *p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    return (uint32_t)strtoul(p+1, NULL, 0);
}

/* ── Funções auxiliares do loader (sem main) ───────────────────────────────── */

/**
 * runtime/loader.c
 * Carrega o EBOOT.BIN na memória emulada e inicia o motor (Trampoline).
 */
/*
#include "cpu.h"
#include "memory.h"
#include "../out/func_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dispatcher.h"

// 1. Declare a variável global para o GP aqui
uint32_t global_gp = 0; 

#ifndef PSP_STACK_TOP
#define PSP_STACK_TOP 0x09FFF000
#endif

// 2. Função de leitura de JSON mantida intacta
static uint32_t read_json_field(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[8192]; 
    size_t len = fread(buf, 1, sizeof(buf)-1, f);
    buf[len] = '\0';
    fclose(f);

    char *p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    return (uint32_t)strtoul(p+1, NULL, 0);
}*/