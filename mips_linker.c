/*
 * MIPS Allegrex Dynamic Linker & EBOOT Loader (mips_linker.c)
 * Restored with complete load_eboot function and updated NID mappings.
 */

#include "mips_reloc.h"
#include "mips_cpu.h"
#include "mips_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

PatchedFallbackStub fallback_stubs[MAX_PATCHED_FALLBACKS];
int fallback_stubs_count = 0;
uint32_t relocated_gp = 0;
uint32_t g_binary_segment_end = 0;
extern uint32_t g_binary_segment_start;
static uint32_t last_module_start = 0;
static uint32_t last_module_stop = 0;
static uint32_t last_module_gp = 0;

static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias, uint32_t mod_addr);
static void resolve_module_info_section_headers(MIPS_CPU *cpu, SceModuleInfo *mod, Elf32_Shdr *sh_table, int sh_num, const char *shstrtab, size_t shstrtab_size, uint32_t load_bias);
static int load_elf_buffer(MIPS_CPU *cpu, uint8_t *data, size_t size,
                           uint32_t *entry_point, uint32_t forced_load_bias,
                           bool allow_missing_entry);

static bool loaded_range_valid(uint32_t addr, size_t size) {
    uint64_t end = (uint64_t)addr + size;
    return g_binary_segment_end > g_binary_segment_start &&
           addr >= g_binary_segment_start &&
           end <= g_binary_segment_end;
}

static const char *elf_section_name(const char *strtab, size_t strtab_size,
                                    uint32_t offset) {
    if (strtab == NULL || offset >= strtab_size) return NULL;
    const char *name = strtab + offset;
    if (memchr(name, '\0', strtab_size - offset) == NULL) return NULL;
    return name;
}

static bool critical_import_library(const char *lib_name) {
    return strcmp(lib_name, "IoFileMgrForUser") == 0 ||
           strcmp(lib_name, "ThreadManForUser") == 0 ||
           strcmp(lib_name, "SysMemUserForUser") == 0 ||
           strcmp(lib_name, "sceGe_Driver") == 0 ||
           strcmp(lib_name, "sceGe") == 0 ||
           strcmp(lib_name, "sceGe_user") == 0 ||
           strcmp(lib_name, "sceDisplay") == 0 ||
           strcmp(lib_name, "sceCtrl") == 0 ||
           strcmp(lib_name, "sceAudio") == 0 ||
           strcmp(lib_name, "sceUmdUser") == 0;
}

static inline uint32_t relocate_addr(uint32_t addr, uint32_t load_bias) {
    if (addr == 0) return 0;
    uint32_t relative_addr = addr & 0x0FFFFFFF;
    if (relative_addr >= 0x08000000 && relative_addr < 0x0C000000) {
        return 0x08000000 + (relative_addr & 0x03FFFFFF);
    }
    if (relative_addr < 0x04000000) {
        return relative_addr + load_bias;
    }
    return addr;
}

typedef struct {
    uint32_t name;
    uint16_t version;
    uint16_t flags;
    uint8_t size;
    uint8_t variable_count;
    uint16_t function_count;
    uint32_t resident;
} __attribute__((packed)) PspLibraryExport;

static void find_module_start_export(MIPS_CPU *cpu, SceModuleInfo *module,
                                     uint32_t load_bias) {
    const uint32_t export_begin = relocate_addr(module->ent_top, load_bias);
    const uint32_t export_end = relocate_addr(module->ent_btm, load_bias);
    uint32_t cursor = export_begin;

    last_module_gp = relocate_addr(module->gp_value, load_bias);
    if (export_begin == 0 || export_end <= export_begin ||
        !loaded_range_valid(export_begin, (size_t)(export_end - export_begin))) {
        return;
    }

    while (cursor < export_end) {
        PspLibraryExport *entry = (PspLibraryExport *)mips_get_ptr_range(
            cpu, cursor, sizeof(*entry));
        if (entry == NULL || entry->size < 4 ||
            (size_t)(export_end - cursor) < entry->size * 4U) {
            return;
        }

        uint32_t variable_count = entry->variable_count;
        if (entry->size > 4) {
            uint16_t extended_count;
            memcpy(&extended_count, (const uint8_t *)entry + 16,
                   sizeof(extended_count));
            if (extended_count > variable_count) variable_count = extended_count;
        }
        const uint64_t word_count =
            (uint64_t)entry->function_count + variable_count;
        const uint64_t table_bytes = word_count * 8;
        const uint32_t resident = relocate_addr(entry->resident, load_bias);
        if (table_bytes != 0 && loaded_range_valid(resident, (size_t)table_bytes)) {
            uint32_t *table = (uint32_t *)mips_get_ptr_range(
                cpu, resident, (size_t)table_bytes);
            if (table != NULL) {
                for (uint32_t i = 0; i < entry->function_count; i++) {
                    if (table[i] == 0xD632ACDB) {
                        uint32_t function = relocate_addr(
                            table[entry->function_count + variable_count + i],
                            load_bias);
                        if ((function & 3U) == 0 &&
                            loaded_range_valid(function, sizeof(uint32_t))) {
                            last_module_start = function;
                            printf("[Loader] module_start export resolved at 0x%08X\n",
                                   function);
                        }
                    }
                    if (table[i] == 0xCEE8593C) {
                        uint32_t function = relocate_addr(
                            table[entry->function_count + variable_count + i],
                            load_bias);
                        if ((function & 3U) == 0 &&
                            loaded_range_valid(function, sizeof(uint32_t))) {
                            last_module_stop = function;
                            printf("[Loader] module_stop export resolved at 0x%08X\n",
                                   function);
                        }
                    }
                }
            }
        }
        cursor += entry->size * 4U;
    }
}

static inline uint32_t resolve_stub_address(MIPS_CPU *cpu, uint32_t stub_val, uint32_t mod_addr, uint32_t load_bias) {
    if (stub_val == 0) return 0;
    if (stub_val >= 0x08800000 && stub_val < 0x0C000000) {
        return stub_val;
    }

    // Candidate 1: relative byte offset
    uint32_t cand1 = stub_val + load_bias;
    SceLibraryStubHeader *hdr1 = loaded_range_valid(cand1, sizeof(*hdr1))
        ? (SceLibraryStubHeader *)mips_get_ptr_range(cpu, cand1, sizeof(*hdr1)) : NULL;
    if (hdr1 && hdr1->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr1->lib_name_ptr, load_bias);
        char name_buf[64];
        if (loaded_range_valid(name_vaddr, 1) &&
            safe_read_cstring(cpu, name_vaddr, name_buf, sizeof(name_buf))) {
            return cand1;
        }
    }

    // Candidate 2: word offset relative to the SceModuleInfo structure address
    uint32_t cand2 = mod_addr + (stub_val * 4);
    SceLibraryStubHeader *hdr2 = loaded_range_valid(cand2, sizeof(*hdr2))
        ? (SceLibraryStubHeader *)mips_get_ptr_range(cpu, cand2, sizeof(*hdr2)) : NULL;
    if (hdr2 && hdr2->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr2->lib_name_ptr, load_bias);
        char name_buf[64];
        if (loaded_range_valid(name_vaddr, 1) &&
            safe_read_cstring(cpu, name_vaddr, name_buf, sizeof(name_buf))) {
            return cand2;
        }
    }

    // Candidate 3: byte offset relative to SceModuleInfo
    uint32_t cand3 = mod_addr + stub_val;
    SceLibraryStubHeader *hdr3 = loaded_range_valid(cand3, sizeof(*hdr3))
        ? (SceLibraryStubHeader *)mips_get_ptr_range(cpu, cand3, sizeof(*hdr3)) : NULL;
    if (hdr3 && hdr3->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr3->lib_name_ptr, load_bias);
        char name_buf[64];
        if (loaded_range_valid(name_vaddr, 1) &&
            safe_read_cstring(cpu, name_vaddr, name_buf, sizeof(name_buf))) {
            return cand3;
        }
    }

    return cand1;
}

static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias, uint32_t mod_addr) {
    if (!mod) { printf("[HLE Linker DEBUG] is_valid_module_info: mod == NULL (endereco 0x%08X nao mapeado)\n", mod_addr); return 0; }
    if (!loaded_range_valid(mod_addr, sizeof(SceModuleInfo)) ||
        mips_get_ptr_range(cpu, mod_addr, sizeof(SceModuleInfo)) != mod) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: estrutura fora do segmento carregado\n", mod_addr);
        return 0;
    }

    // NOVO: dump bruto dos 64 primeiros bytes da estrutura (hex + ASCII),
    // pra conferirmos visualmente onde os campos de verdade estao, em vez
    // de continuar assumindo o layout classico de 52 bytes (que pode estar
    // desalinhado nessa build - a secao real tem 64 bytes, 12 a mais que o
    // struct que a gente usa).
    {
        const uint8_t *raw = loaded_range_valid(mod_addr, 64)
            ? (const uint8_t *)mips_get_ptr_range(cpu, mod_addr, 64) : NULL;
        if (raw == NULL) raw = (const uint8_t *)mod;
        printf("[HLE Linker DEBUG] Dump bruto de SceModuleInfo em 0x%08X (%u bytes):\n", mod_addr,
               raw == (const uint8_t *)mod ? (unsigned)sizeof(SceModuleInfo) : 64U);
        size_t dump_size = raw == (const uint8_t *)mod ? sizeof(SceModuleInfo) : 64;
        for (size_t row = 0; row < dump_size; row += 16) {
            printf("  +0x%02X:", (unsigned)row);
            size_t row_size = dump_size - row;
            if (row_size > 16) row_size = 16;
            for (size_t col = 0; col < row_size; col++) {
                printf(" %02X", raw[row + col]);
            }
            printf("  |");
            for (size_t col = 0; col < row_size; col++) {
                uint8_t b = raw[row + col];
                putchar((b >= 32 && b < 127) ? (char)b : '.');
            }
            printf("|\n");
        }
    }

    if (mod->modAttribute > 0x1FFF) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: modAttribute=0x%04X > 0x1FFF\n", mod_addr, mod->modAttribute);
        return 0;
    }
    if (mod->modName[0] < 32 || mod->modName[0] >= 127) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: modName[0]=0x%02X nao e ASCII imprimivel "
               "(bytes brutos: %02X %02X %02X %02X %02X %02X %02X %02X)\n",
               mod_addr, (unsigned char)mod->modName[0],
               (unsigned char)mod->modName[0], (unsigned char)mod->modName[1], (unsigned char)mod->modName[2], (unsigned char)mod->modName[3],
               (unsigned char)mod->modName[4], (unsigned char)mod->modName[5], (unsigned char)mod->modName[6], (unsigned char)mod->modName[7]);
        return 0;
    }
    int name_len = 0;
    while (name_len < 27) {
        char c = mod->modName[name_len];
        if (c == '\0') break;
        if (c < 32 || c >= 127) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: modName[%d]=0x%02X nao e ASCII imprimivel (nome ate aqui: '%.*s')\n",
                   mod_addr, name_len, (unsigned char)c, name_len, mod->modName);
            return 0;
        }
        name_len++;
    }
    if (name_len == 0) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: modName vazio\n", mod_addr);
        return 0;
    }

    char first = mod->modName[0];
    // FIX: 'Cave Story' foi compilado com aspas literais no nome do modulo
    // ("Cave Story" com " " mesmo, provavel erro de quoting no Makefile do
    // port) - o dump bruto confirmou que o resto da struct (gp_value,
    // ent_top/btm, stub_top/btm) esta perfeitamente valido logo depois
    // desse nome, entao isso e' dado real, nao corrupcao. Aceitar aspas
    // como caractere valido pra nao rejeitar modulos legitimos so por causa
    // de uma escolha de build incomum.
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9') || first == '_' || first == '"')) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: primeiro char '%c' (0x%02X) invalido pra nome de modulo\n",
               mod_addr, first, (unsigned char)first);
        return 0;
    }
    for (int i = 0; i < name_len; i++) {
        char c = mod->modName[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ' || c == '"')) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO em 0x%08X: char '%c' (0x%02X) na posicao %d do nome '%.*s' invalido\n",
                   mod_addr, c, (unsigned char)c, i, name_len, mod->modName);
            return 0;
        }
    }

    printf("[HLE Linker DEBUG] is_valid_module_info: nome '%.*s' passou (modAttribute=0x%04X). Campos brutos: gp_value=0x%08X ent_top=0x%08X ent_btm=0x%08X stub_top=0x%08X stub_btm=0x%08X\n",
           name_len, mod->modName, mod->modAttribute, mod->gp_value, mod->ent_top, mod->ent_btm, mod->stub_top, mod->stub_btm);

    uint32_t gp = relocate_addr(mod->gp_value, load_bias);
    if (gp != 0 && (gp < 0x08800000 || gp >= 0x0C000000)) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): gp resolvido=0x%08X fora de [0x08800000,0x0C000000) (bruto=0x%08X, load_bias=0x%08X)\n",
               name_len, mod->modName, gp, mod->gp_value, load_bias);
        return 0;
    }

    uint32_t stub_top = resolve_stub_address(cpu, mod->stub_top, mod_addr, load_bias);
    uint32_t stub_btm = resolve_stub_address(cpu, mod->stub_btm, mod_addr, load_bias);
    printf("[HLE Linker DEBUG] is_valid_module_info ('%.*s'): stub_top resolvido=0x%08X, stub_btm resolvido=0x%08X\n",
           name_len, mod->modName, stub_top, stub_btm);

    if (stub_top != 0 && (stub_top < 0x08800000 || stub_top >= 0x0C000000)) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): stub_top resolvido=0x%08X fora de [0x08800000,0x0C000000)\n",
               name_len, mod->modName, stub_top);
        return 0;
    }
    if (stub_btm != 0 && (stub_btm < 0x08800000 || stub_btm >= 0x0C000000)) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): stub_btm resolvido=0x%08X fora de [0x08800000,0x0C000000)\n",
               name_len, mod->modName, stub_btm);
        return 0;
    }
    if (stub_top != 0 && stub_btm != 0 && stub_top > stub_btm) {
        printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): stub_top (0x%08X) > stub_btm (0x%08X)\n",
               name_len, mod->modName, stub_top, stub_btm);
        return 0;
    }

    if (stub_top != 0 && stub_btm != 0 && stub_top < stub_btm && (stub_btm - stub_top) >= 20) {
        SceLibraryStubHeader *stub_hdr =
            loaded_range_valid(stub_top, sizeof(SceLibraryStubHeader))
                ? (SceLibraryStubHeader *)mips_get_ptr_range(cpu, stub_top, sizeof(*stub_hdr))
                : NULL;
        if (!stub_hdr) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): mips_get_ptr(stub_top=0x%08X) retornou NULL\n",
                   name_len, mod->modName, stub_top);
            return 0;
        }

        uint32_t lib_name_vaddr = relocate_addr(stub_hdr->lib_name_ptr, load_bias);
        if (lib_name_vaddr < 0x08800000 || lib_name_vaddr >= 0x0C000000) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): lib_name_vaddr resolvido=0x%08X (bruto stub_hdr->lib_name_ptr=0x%08X) fora de [0x08800000,0x0C000000)\n",
                   name_len, mod->modName, lib_name_vaddr, stub_hdr->lib_name_ptr);
            return 0;
        }

        char lib_name_buf[128];
        if (!loaded_range_valid(lib_name_vaddr, 1) ||
            !safe_read_cstring(cpu, lib_name_vaddr, lib_name_buf, sizeof(lib_name_buf))) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): mips_get_ptr(lib_name_vaddr=0x%08X) retornou NULL\n",
                   name_len, mod->modName, lib_name_vaddr);
            return 0;
        }
        if (lib_name_buf[0] == '\0') {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): lib_name vazio em 0x%08X\n",
                   name_len, mod->modName, lib_name_vaddr);
            return 0;
        }
    }

    printf("[HLE Linker DEBUG] is_valid_module_info: '%.*s' VALIDADO com sucesso.\n", name_len, mod->modName);
    return 1;
}

static void resolve_module_info_section_headers(MIPS_CPU *cpu, SceModuleInfo *mod, Elf32_Shdr *sh_table, int sh_num, const char *shstrtab, size_t shstrtab_size, uint32_t load_bias) {
    (void)cpu;
    if (!mod) return;

    uint32_t lib_stub_addr = 0;
    uint32_t lib_stub_size = 0;

    if (shstrtab != NULL) {
        for (int i = 0; i < sh_num; i++) {
            const char *sect_name = elf_section_name(shstrtab, shstrtab_size, sh_table[i].sh_name);
            if (sect_name != NULL && strcmp(sect_name, ".lib.stub") == 0) {
                lib_stub_addr = sh_table[i].sh_addr + load_bias;
                lib_stub_size = sh_table[i].sh_size;
                break;
            }
        }
    }

    if (lib_stub_addr != 0 && loaded_range_valid(lib_stub_addr, lib_stub_size) &&
        (mod->stub_top >= (uint32_t)sh_num || strcmp(mod->modName, "s") == 0)) {
        printf("[HLE Linker ACTIVE CORRUPTION DETECTOR] Tentando reescrever mod->stub_top/btm para o modulo '%s'!\n", mod->modName);
        printf("  -> GP lido do modulo: 0x%08X, stub_top atual (sera reescrito): 0x%08X, stub_btm atual: 0x%08X\n", mod->gp_value, mod->stub_top, mod->stub_btm);
        printf("  -> Novo stub_top: 0x%08X, Novo stub_btm: 0x%08X\n", lib_stub_addr, lib_stub_addr + lib_stub_size);
        printf("[HLE Linker] Overriding stale indices for module '%s' using real ELF '.lib.stub' section: VAddr 0x%08X (Size: %u bytes)\n",
               mod->modName, lib_stub_addr, lib_stub_size);
        mod->stub_top = lib_stub_addr;
        mod->stub_btm = lib_stub_addr + lib_stub_size;
    } else if (lib_stub_addr != 0 && !loaded_range_valid(lib_stub_addr, lib_stub_size)) {
        printf("[HLE Linker Error] ELF .lib.stub section is outside the loaded segment; keeping module metadata unchanged.\n");
    } else {
        if (mod->stub_top != 0 && mod->stub_top < (uint32_t)sh_num) {
            uint32_t sect_idx = mod->stub_top;
            mod->stub_top = sh_table[sect_idx].sh_addr + load_bias;
            printf("[HLE Linker] Resolved mod->stub_top section index %u to VAddr 0x%08X\n", sect_idx, mod->stub_top);
        }
        if (mod->stub_btm != 0 && mod->stub_btm < (uint32_t)sh_num) {
            uint32_t sect_idx = mod->stub_btm;
            mod->stub_btm = sh_table[sect_idx].sh_addr + load_bias;
            printf("[HLE Linker] Resolved mod->stub_btm section index %u to VAddr 0x%08X\n", sect_idx, mod->stub_btm);
        }
    }

    if (mod->ent_top != 0 && mod->ent_top < (uint32_t)sh_num) {
        uint32_t sect_idx = mod->ent_top;
        mod->ent_top = sh_table[sect_idx].sh_addr + load_bias;
    }
    if (mod->ent_btm != 0 && mod->ent_btm < (uint32_t)sh_num) {
        uint32_t sect_idx = mod->ent_btm;
        mod->ent_btm = sh_table[sect_idx].sh_addr + load_bias;
    }
}

static int load_elf_buffer(MIPS_CPU *cpu, uint8_t *data, size_t size,
                           uint32_t *entry_point, uint32_t forced_load_bias,
                           bool allow_missing_entry) {
    if (data == NULL || entry_point == NULL) return -1;
    uint8_t *elf_data = data;
    size_t elf_size = size;
    (void)elf_size;

    uint32_t modinfo_offset1 = 0;
    uint32_t modinfo_offset2 = 0;

    if (size >= 4 && memcmp(data, "~PSP", 4) == 0) {
        printf("[Loader] Detected ~PSP (PRX) executable container.\n");
        if (size < 56) {
            printf("[Loader Error] PRX header is truncated.\n");
            free(data);
            return -4;
        }
        modinfo_offset1 = *(uint32_t*)&data[52];

        uint8_t *elf_ptr = NULL;
        for (size_t i = 0; i + 4 <= size; i++) {
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
            uint16_t comp_type = *(uint16_t*)&data[6];
            if ((comp_type & 0xF00) != 0x300) {
                printf("[Loader Error] PRX file is encrypted or compressed (Type 0x%03X).\n", comp_type & 0xF00);
                free(data);
                return -4;
            }

            uint32_t boot_entry = *(uint32_t*)&data[48];
            printf("[Loader] Mapping flat program space directly to standard base address 0x08804000.\n");

            if (size < 0x150 || size - 0x150 > 0x0C000000U - 0x08804000U) {
                printf("[Loader Error] Flat PRX payload does not fit in PSP user RAM.\n");
                free(data);
                return -5;
            }
            void *dest = mips_get_ptr_range(cpu, 0x08804000, size - 0x150);
            if (!dest) {
                printf("[Loader Error] Target physical memory at 0x08804000 is unavailable.\n");
                free(data);
                return -5;
            }
            memcpy(dest, data + 0x150, size - 0x150);
            *entry_point = boot_entry + 0x08804000;
            if (g_binary_segment_end == 0) g_binary_segment_start = 0x08804000;
            if (0x08804000 < g_binary_segment_start) g_binary_segment_start = 0x08804000;
            if (0x08804000 + (uint32_t)(size - 0x150) > g_binary_segment_end) {
                g_binary_segment_end = 0x08804000 + (uint32_t)(size - 0x150);
            }
            if (*entry_point < g_binary_segment_start || *entry_point >= g_binary_segment_end) {
                printf("[Loader Error] Flat PRX entry point is outside its loaded payload.\n");
                free(data);
                return -10;
            }
            free(data);
            return 0;
        }

    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    if (elf_size < sizeof(Elf32_Ehdr) ||
        ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        printf("[Loader Error] Game binary does not have a valid ELF magic number.\n");
        free(data);
        return -6;
    }

    if (ehdr->e_phentsize < sizeof(Elf32_Phdr) ||
        ehdr->e_phoff > elf_size ||
        (size_t)ehdr->e_phnum > (elf_size - ehdr->e_phoff) / ehdr->e_phentsize) {
        printf("[Loader Error] ELF program-header table is truncated or malformed.\n");
        free(data);
        return -7;
    }

    extern uint32_t g_next_module_load_addr;
    uint32_t load_bias = 0;
    if (forced_load_bias != 0) {
        load_bias = forced_load_bias;
        printf("[Loader] PRX ELF mapped with dynamic load bias: 0x%08X\n",
               load_bias);
    } else if (ehdr->e_entry < 0x08000000) {
        if (g_next_module_load_addr != 0) {
            load_bias = g_next_module_load_addr;
            printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying dynamic load bias: 0x%08X\n", ehdr->e_entry, load_bias);
        } else {
            // Match PPSSPP's default PSP user-RAM load base for relocatable game modules:
            // the ELF is not loaded at the base of user RAM, it is loaded at 0x08804000.
            load_bias = PSP_USER_LOAD_BASE;
            printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying PPSSPP-compatible load bias: 0x%08X\n", ehdr->e_entry, load_bias);
        }
    }

    if (ehdr->e_shoff != 0 &&
        (ehdr->e_shentsize != sizeof(Elf32_Shdr) ||
         ehdr->e_shoff > elf_size ||
         (size_t)ehdr->e_shnum > (elf_size - ehdr->e_shoff) / ehdr->e_shentsize)) {
        printf("[Loader Error] ELF section-header table is truncated or malformed.\n");
        free(data);
        return -7;
    }

    const bool missing_module_entry = allow_missing_entry &&
        (ehdr->e_entry == 0 || ehdr->e_entry == UINT32_MAX);
    *entry_point = missing_module_entry
        ? ehdr->e_entry : ehdr->e_entry + load_bias;
    printf("[Loader] Successfully parsed MIPS ELF Header. Entry Point: 0x%08X\n", *entry_point);

    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    int loaded_count = 0;
    bool entry_in_exec_segment = false;
    uint32_t seg0_file_offset = 0;
    // FIX: antes so guardavamos o offset do arquivo do segmento 0
    // (seg0_file_offset), mas o debug de bissecao la embaixo usava um
    // endereco de RAM FIXO (0x08800000) para comparar - certo so quando o
    // load_bias realmente era 0x08800000. Pra ELFs de endereco fixo tipo o
    // Cave Story (load_bias=0, carrega direto em p_vaddr, que pode ser
    // 0x08900000 ou qualquer outro valor), a comparacao lia de um endereco
    // de RAM completamente errado (sempre zero, por nunca ter sido escrito
    // ali) e dava falso positivo de "segmento nao foi carregado". Agora
    // guardamos o vaddr REAL onde o segmento 0 foi colocado, pra usar como
    // base da comparacao.
    uint32_t seg0_target_vaddr = 0;
    if (g_binary_segment_end == 0) {
        g_binary_segment_start = UINT32_MAX;
    }
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD
            uint64_t target_end64;
            uint32_t target_vaddr;
            printf("  -> Loading PT_LOAD segment %d: VADDR 0x%08X, File Offset: 0x%08X, File Size: %u bytes, Memory Size: %u bytes\n",
            loaded_count, phdr->p_vaddr + load_bias, phdr->p_offset, phdr->p_filesz, phdr->p_memsz);

            if (phdr->p_filesz > phdr->p_memsz ||
                phdr->p_offset > elf_size ||
                phdr->p_filesz > elf_size - phdr->p_offset) {
                printf("[Loader Error] PT_LOAD segment has invalid file/memory bounds.\n");
                free(data);
                return -8;
            }
            target_end64 = (uint64_t)phdr->p_vaddr + load_bias + phdr->p_memsz;
            if (target_end64 > 0x0C000000ULL ||
                (uint64_t)phdr->p_vaddr + load_bias < 0x08000000ULL) {
                printf("[Loader Error] PT_LOAD segment is outside PSP user RAM.\n");
                free(data);
                return -9;
            }
            target_vaddr = phdr->p_vaddr + load_bias;

            if (loaded_count == 0) {
                seg0_file_offset = phdr->p_offset;
                seg0_target_vaddr = target_vaddr;
            }

            if ((phdr->p_flags & 0x1U) != 0 &&
                (uint64_t)*entry_point >= target_vaddr &&
                (uint64_t)*entry_point < target_end64) {
                entry_in_exec_segment = true;
            }
            void *dest = mips_get_ptr_range(cpu, target_vaddr, phdr->p_memsz);
            if (!dest) {
                printf("[Loader Error] Memory mapping violation: Segment virtual range is out of bounds!\n");
                free(data);
                return -7;
            }
            memcpy(dest, elf_data + phdr->p_offset, phdr->p_filesz);
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((uint8_t*)dest + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
            }

            uint32_t seg_end = target_vaddr + phdr->p_memsz;
            if (target_vaddr < g_binary_segment_start) {
                g_binary_segment_start = target_vaddr;
            }
            if (seg_end > g_binary_segment_end) {
                g_binary_segment_end = seg_end;
            }

            loaded_count++;
        }
    }

    printf("[Loader] Successfully loaded %d executable segments into emulated memory spaces.\n", loaded_count);
    if (loaded_count == 0 || g_binary_segment_start == UINT32_MAX ||
        (!missing_module_entry &&
         (!entry_in_exec_segment ||
          *entry_point < g_binary_segment_start ||
          *entry_point >= g_binary_segment_end))) {
        printf("[Loader Error] ELF has no valid executable segment containing its entry point.\n");
        free(data);
        return -10;
    }

    {
        uint32_t *entry_snapshot = (uint32_t *)mips_get_ptr(cpu, *entry_point);
        if (entry_snapshot) {
            printf("[DEBUG PRE-RELOC] Bytes no entry point 0x%08X ANTES da relocacao: %08X %08X %08X\n",
                   *entry_point, entry_snapshot[0], entry_snapshot[1], entry_snapshot[2]);
        }
    }

    {
        uint32_t offsets_to_check[] = {
            0x3BA4, 0x1000, 0x2000, 0x4000, 0x8000,
            0xC000, 0x10000, 0x14000, 0x18000, 0x1A000,
            0x1C000, 0x1D000, 0x1E000, 0x1E200, 0x1E350
        };
        int n = sizeof(offsets_to_check) / sizeof(offsets_to_check[0]);
        printf("[DEBUG BISSECAO] Comparando RAM vs arquivo em varios offsets (base real do segmento 0: 0x%08X):\n", seg0_target_vaddr);
        for (int k = 0; k < n; k++) {
            uint32_t off = offsets_to_check[k];
            // FIX: era 'mips_get_ptr(cpu, 0x08800000 + off)' - agora usa a
            // base de verdade do segmento 0 (seg0_target_vaddr), que pode
            // ser 0x08800000, 0x08900000 ou qualquer outro valor
            // dependendo de onde o ELF foi realmente carregado.
            uint32_t *ram_word = (uint32_t *)mips_get_ptr(cpu, seg0_target_vaddr + off);
            size_t file_offset = (size_t)seg0_file_offset + off;
            uint32_t file_word = (file_offset + sizeof(uint32_t) <= elf_size) ?
                *(uint32_t *)(elf_data + file_offset) : 0xDEADDEAD;
            printf("  offset=0x%06X | arquivo=%08X | RAM=%08X | %s\n",
                off, file_word, ram_word ? *ram_word : 0xDEADDEAD,
                (ram_word && file_word == *ram_word) ? "OK" : "DIVERGIU");
        }
    }

    const char *shstrtab = NULL;
    size_t shstrtab_size = 0;
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        if (ehdr->e_shstrndx < ehdr->e_shnum) {
            Elf32_Shdr *shstr = &sh_table[ehdr->e_shstrndx];
            if (shstr->sh_offset <= elf_size &&
                shstr->sh_size <= elf_size - shstr->sh_offset) {
                shstrtab = (const char *)(elf_data + shstr->sh_offset);
                shstrtab_size = shstr->sh_size;
            } else {
                printf("[Loader Warning] ELF section-name string table is truncated.\n");
            }
        }
    }

    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *dbg_sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        printf("[DEBUG SECTIONS] Total: %d\n", ehdr->e_shnum);
        for (int s = 0; s < ehdr->e_shnum; s++) {
            const char *sname = elf_section_name(shstrtab, shstrtab_size, dbg_sh_table[s].sh_name);
            if (sname == NULL) sname = "???";
            printf("  [%2d] name='%s' type=0x%X offset=0x%X size=%u link=%u info=%u entsize=%u\n",
                   s, sname, dbg_sh_table[s].sh_type, dbg_sh_table[s].sh_offset,
                   dbg_sh_table[s].sh_size, dbg_sh_table[s].sh_link, dbg_sh_table[s].sh_info,
                   dbg_sh_table[s].sh_entsize);
        }
    }

    int reloc_ret = mips_apply_relocations(cpu, elf_data, elf_size, load_bias, entry_point);
    if (reloc_ret != 0) {
        printf("[Loader Error] Relocation phase failed with status %d.\n", reloc_ret);
        free(data);
        return -11;
    }
    if (getenv("MIPS_DEBUG_RELOC_WINDOW") != NULL) {
        printf("[Loader Relocation Check] Loaded instructions after relocations:\n");
        for (uint32_t offset = 0; offset < 0x100; offset += 4) {
            uint32_t address = seg0_target_vaddr + offset;
            uint32_t *word = (uint32_t *)mips_get_ptr_range(
                cpu, address, sizeof(uint32_t));
            printf("  0x%08X: %08X\n", address,
                   word != NULL ? *word : 0xDEADDEAD);
        }
    }

    if (ehdr->e_phnum > 0) {
        Elf32_Phdr *first_phdr = (Elf32_Phdr*)(phdr_table);
        if (first_phdr->p_paddr != 0 && first_phdr->p_paddr != first_phdr->p_vaddr) {
            modinfo_offset2 = first_phdr->p_paddr & 0x7FFFFFFF;
            printf("[Loader] ELF format detected. Retrieved SceModuleInfo offset from first phdr p_paddr: 0x%08X\n", modinfo_offset2);
        }
    }

    // NOVO: terceira forma de achar a SceModuleInfo - direto pelo NOME da
    // secao (.rodata.sceModuleInfo), sem depender do truque de p_paddr (que
    // varios toolchains, incluindo o que gerou o Cave Story, simplesmente
    // nao preenchem). sh_offset ja e' o offset de arquivo, no mesmo formato
    // que modinfo_offset1/2 usam, entao entra direto no mesmo esquema de
    // resolucao la embaixo.
    uint32_t modinfo_offset3 = 0;
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0 && shstrtab != NULL) {
        Elf32_Shdr *sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        for (int s = 0; s < ehdr->e_shnum; s++) {
            const char *sname = elf_section_name(shstrtab, shstrtab_size, sh_table[s].sh_name);
            if (sname != NULL && strcmp(sname, ".rodata.sceModuleInfo") == 0) {
                modinfo_offset3 = sh_table[s].sh_offset;
                printf("[Loader] SceModuleInfo localizada por nome de secao '.rodata.sceModuleInfo' no offset de arquivo: 0x%08X\n", modinfo_offset3);
                break;
            }
        }
    }

    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        int sh_num = ehdr->e_shnum;

        uint32_t offsets[3] = { modinfo_offset1, modinfo_offset2, modinfo_offset3 };
        for (int o = 0; o < 3; o++) {
            uint32_t offset = offsets[o];
            if (offset != 0) {
                for (int i = 0; i < ehdr->e_phnum; i++) {
                    Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
                    if (phdr->p_type == 1) {
                        if ((uint64_t)offset >= phdr->p_offset &&
                            (uint64_t)offset < (uint64_t)phdr->p_offset + phdr->p_filesz) {
                            uint32_t relative_offset = offset - phdr->p_offset;
                            uint32_t file_addr = phdr->p_vaddr + relative_offset + load_bias;
                            SceModuleInfo *fmod = (SceModuleInfo *)mips_get_ptr(cpu, file_addr);
                            if (fmod && is_valid_module_info(cpu, fmod, load_bias, file_addr)) {
                                printf("[HLE Linker] Resolving section headers for valid module '%s' at 0x%08X...", fmod->modName, file_addr);
                                resolve_module_info_section_headers(cpu, fmod, sh_table, sh_num, shstrtab, shstrtab_size, load_bias);
                            }
                        }
                    }
                }
            }
        }
    }

    uint32_t resolved_addrs[32];
    int resolved_count = 0;
    int unresolved_critical_imports = 0;
    int malformed_import_tables = 0;

    uint32_t offsets[3] = { modinfo_offset1, modinfo_offset2, modinfo_offset3 };
    for (int o = 0; o < 3; o++) {
        uint32_t offset = offsets[o];
        if (offset != 0) {
            for (int i = 0; i < ehdr->e_phnum; i++) {
                Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
                if (phdr->p_type == 1) {
                    if ((uint64_t)offset >= phdr->p_offset &&
                        (uint64_t)offset < (uint64_t)phdr->p_offset + phdr->p_filesz) {
                        uint32_t relative_offset = offset - phdr->p_offset;
                        uint32_t file_addr = phdr->p_vaddr + relative_offset + load_bias;
                        SceModuleInfo *fmod = (SceModuleInfo *)mips_get_ptr(cpu, file_addr);
                        if (fmod && is_valid_module_info(cpu, fmod, load_bias, file_addr)) {
                            bool dup = false;
                            for (int d = 0; d < resolved_count; d++) {
                                if (resolved_addrs[d] == file_addr) dup = true;
                            }
                            if (!dup) {
                                resolved_addrs[resolved_count++] = file_addr;
                                printf("[HLE Linker] Direct modinfo file offset 0x%08X is VALID ('%s').", file_addr, fmod->modName);
                            }
                        }
                    }
                }
            }
        }
    }

    printf("[HLE Linker] Total modules identified for patching: %d", resolved_count);

    int total_patches = 0;
    for (int m = 0; m < resolved_count; m++) {
        uint32_t mod_addr = resolved_addrs[m];
        SceModuleInfo *mod_info = (SceModuleInfo *)mips_get_ptr(cpu, mod_addr);
        if (mod_info != NULL) {
            uint32_t current_gp = relocate_addr(mod_info->gp_value, load_bias);
            if (current_gp != 0) { relocated_gp = current_gp; }
            find_module_start_export(cpu, mod_info, load_bias);

            printf("[HLE Linker] >>> Patching Module %d/%d: '%s' (Attributes: 0x%04X, GP: 0x%08X) <<<\n", 
                   m + 1, resolved_count, mod_info->modName, mod_info->modAttribute, current_gp);
            printf("[HLE Linker DEBUG] Module '%s' raw fields: gp_value=0x%08X, ent_top=0x%08X, ent_btm=0x%08X, stub_top=0x%08X, stub_btm=0x%08X\n",
                   mod_info->modName, mod_info->gp_value, mod_info->ent_top, mod_info->ent_btm, mod_info->stub_top, mod_info->stub_btm);

            uint32_t stub_top_addr = resolve_stub_address(cpu, mod_info->stub_top, mod_addr, load_bias);
            uint32_t stub_btm_addr = resolve_stub_address(cpu, mod_info->stub_btm, mod_addr, load_bias);
            printf("  -> Stub table range: 0x%08X - 0x%08X\n", stub_top_addr, stub_btm_addr);

            if (stub_top_addr == 0 || stub_btm_addr <= stub_top_addr ||
                !loaded_range_valid(stub_top_addr, (size_t)stub_btm_addr - stub_top_addr)) {
                if (mod_info->stub_top != 0 || mod_info->stub_btm != 0) {
                    printf("[HLE Linker Error] Import table for '%s' is outside the loaded segment or has invalid bounds.\n",
                           mod_info->modName);
                    malformed_import_tables++;
                }
                continue;
            }

            uint32_t curr_stub_addr = stub_top_addr;
            int patch_count = 0;
            int total_stubs = 0;

            while (curr_stub_addr < stub_btm_addr) {
                if ((size_t)(stub_btm_addr - curr_stub_addr) < sizeof(SceLibraryStubHeader)) {
                    printf("[HLE Linker Error] Truncated library stub header at 0x%08X.\n", curr_stub_addr);
                    malformed_import_tables++;
                    break;
                }
                SceLibraryStubHeader *stub_hdr =
                    (SceLibraryStubHeader *)mips_get_ptr_range(cpu, curr_stub_addr, sizeof(*stub_hdr));
                if (stub_hdr == NULL || stub_hdr->lib_name_ptr == 0) {
                    printf("[HLE Linker Error] Invalid library stub header at 0x%08X.\n", curr_stub_addr);
                    malformed_import_tables++;
                    break;
                }

                uint32_t lib_name_vaddr = relocate_addr(stub_hdr->lib_name_ptr, load_bias);
                char lib_name_buf[64];
                if (!loaded_range_valid(lib_name_vaddr, 1) ||
                    !safe_read_cstring(cpu, lib_name_vaddr, lib_name_buf, sizeof(lib_name_buf))) {
                    printf("[HLE Linker Error] Invalid library name pointer 0x%08X at stub 0x%08X.\n",
                           lib_name_vaddr, curr_stub_addr);
                    malformed_import_tables++;
                    curr_stub_addr += 4;
                    continue;
                }
                const char *lib_name = lib_name_buf;

                uint32_t nid_table_vaddr = relocate_addr(stub_hdr->nid_table_ptr, load_bias);
                uint32_t stub_table_vaddr = relocate_addr(stub_hdr->stub_table_ptr, load_bias);
                size_t nid_bytes = (size_t)stub_hdr->num_funcs * sizeof(uint32_t);
                size_t stub_bytes = (size_t)stub_hdr->num_funcs * 8U;
                uint32_t *nid_table = (stub_hdr->num_funcs > 0 &&
                                       loaded_range_valid(nid_table_vaddr, nid_bytes))
                    ? (uint32_t *)mips_get_ptr_range(cpu, nid_table_vaddr, nid_bytes) : NULL;

                if (nid_table != NULL && stub_table_vaddr != 0 &&
                    stub_hdr->num_funcs > 0 && stub_hdr->num_funcs <= 512 &&
                    loaded_range_valid(stub_table_vaddr, stub_bytes)) {

                    uint32_t num_stubs_to_patch = stub_hdr->num_funcs;

                    for (uint32_t j = 0; j < num_stubs_to_patch; j++) {
                        uint32_t nid = nid_table[j];
                        uint32_t target_stub_vaddr = stub_table_vaddr + j * 8;

                        if (!loaded_range_valid(target_stub_vaddr, 8) ||
                            mips_get_ptr_range(cpu, target_stub_vaddr, 8) == NULL) {
                            printf("[HLE Linker WARN] target_stub_vaddr fora dos limites (0x%08X) para '%s' NID 0x%08X — pulando patch, tabela pode estar desalinhada.\n",
                                   target_stub_vaddr, lib_name, nid);
                            continue;
                        }

                        uint32_t syscall_code = 0;
                        const char *func_name = "unknown";

                        if (strcmp(lib_name, "IoFileMgrForUser") == 0) {
                            if (nid == 0x109F504F || nid == 0x109F50BC) { syscall_code = 0x11111; func_name = "sceIoOpen"; }
                            else if (nid == 0x6A638D83) { syscall_code = 0x11112; func_name = "sceIoRead"; }
                            else if (nid == 0x42EC03AC) { syscall_code = 0x1111C; func_name = "sceIoWrite"; }
                            else if (nid == 0x0925712E) { syscall_code = 0x1111D; func_name = "sceIoWriteAsync"; }
                            else if (nid == 0x27EB27B8) { syscall_code = 0x11116; func_name = "sceIoLseek"; }
                            else if (nid == 0x68963324) { syscall_code = 0x11117; func_name = "sceIoLseek32"; }
                            else if (nid == 0xA0B5A7C2) { syscall_code = 0x11113; func_name = "sceIoReadAsync"; }
                            else if (nid == 0x89AA9906) { syscall_code = 0x11126; func_name = "sceIoOpenAsync"; }
                            else if (nid == 0xB293727F) { syscall_code = 0x11127; func_name = "sceIoChangeAsyncPriority"; }
                            else if (nid == 0x810C4BC3) { syscall_code = 0x11114; func_name = "sceIoClose"; }
                            else if (nid == 0x54F5FB11) { syscall_code = 0x11115; func_name = "sceIoDevctl"; }
                            else if (nid == 0x63632449) { syscall_code = 0x11118; func_name = "sceIoIoctl"; }
                            else if (nid == 0xE95A012B) { syscall_code = 0x11119; func_name = "sceIoIoctlAsync"; }
                            else if (nid == 0xE23EEC33) { syscall_code = 0x1111A; func_name = "sceIoWaitAsync"; }
                            else if (nid == 0x3251EA56) { syscall_code = 0x1111B; func_name = "sceIoPollAsync"; }
                            else if (nid == 0x55F4717D) { syscall_code = 0x1111E; func_name = "sceIoGetstat"; }
                            else if (nid == 0xB29DDF9C) { syscall_code = 0x1111F; func_name = "sceIoDopen"; }
                            else if (nid == 0xE3EB004C) { syscall_code = 0x11120; func_name = "sceIoDread"; }
                            else if (nid == 0xACE946E8) { syscall_code = 0x11125; func_name = "sceIoGetstat"; }
                            else if (nid == 0xEB092469) { syscall_code = 0x11121; func_name = "sceIoDclose"; }
                            else if (nid == 0x06A70004) { syscall_code = 0x11128; func_name = "sceIoMkdir"; }
                            else if (nid == 0x779103A0) { syscall_code = 0x11129; func_name = "sceIoRename"; }
                            else if (nid == 0xF27A9C51) { syscall_code = 0x1112A; func_name = "sceIoRemove"; }
                            else if (nid == 0x1117C65F) { syscall_code = 0x1112B; func_name = "sceIoRmdir"; }
                            else if (nid == 0x71B19E77) { syscall_code = 0x1112C; func_name = "sceIoLseekAsync"; }
                            else if (nid == 0x35DBD746) { syscall_code = 0x1112D; func_name = "sceIoWaitAsyncCB"; }
                        } else if (strcmp(lib_name, "StdioForUser") == 0) {
                            if (nid == 0x172D316E) { syscall_code = 0x11122; func_name = "sceKernelStdin"; }
                            else if (nid == 0xA6BAB2E9) { syscall_code = 0x11123; func_name = "sceKernelStdout"; }
                            else if (nid == 0xF78BA90A) { syscall_code = 0x11124; func_name = "sceKernelStderr"; }
                        } else if (strcmp(lib_name, "ThreadManForUser") == 0) {
                            if (nid == 0xD6DA4BA1) { syscall_code = 0x22221; func_name = "sceKernelCreateSema"; }
                            else if (nid == 0x4E3A1105) { syscall_code = 0x22222; func_name = "sceKernelWaitSema"; }
                            else if (nid == 0x3F53E640) { syscall_code = 0x22223; func_name = "sceKernelSignalSema"; }
                            else if (nid == 0x28B6489C) { syscall_code = 0x2223D; func_name = "sceKernelDeleteSema"; }
                            else if (nid == 0xCEADEB47) { syscall_code = 0x22224; func_name = "sceKernelDelayThread"; }
                            else if (nid == 0x446D8DE6) { syscall_code = 0x22225; func_name = "sceKernelCreateThread"; }
                            else if (nid == 0xF475845D) { syscall_code = 0x22226; func_name = "sceKernelStartThread"; }
                            else if (nid == 0xAA73C935 || nid == 0x8011F9B0) { syscall_code = 0x22227; func_name = "sceKernelExitThread"; }
                            else if (nid == 0x349D6D6C) { syscall_code = 0x22228; func_name = "sceKernelCheckCallback"; }
                            else if (nid == 0x68DA9E36) { syscall_code = 0x22224; func_name = "sceKernelDelayThreadCB"; }
                            else if (nid == 0xEDBA5844) { syscall_code = 0x22229; func_name = "sceKernelDeleteCallback"; }
                            else if (nid == 0x55C20A00) { syscall_code = 0x22230; func_name = "sceKernelCreateEventFlag"; }
                            else if (nid == 0x1FB15A32) { syscall_code = 0x22231; func_name = "sceKernelSetEventFlag"; }
                            else if (nid == 0x812346E4) { syscall_code = 0x22232; func_name = "sceKernelClearEventFlag"; }
                            else if (nid == 0xEF9E4C70) { syscall_code = 0x22240; func_name = "sceKernelDeleteEventFlag"; }
                            else if (nid == 0x402FCF22) { syscall_code = 0x22241; func_name = "sceKernelWaitEventFlag"; }
                            else if (nid == 0xA66B0120) { syscall_code = 0x22242; func_name = "sceKernelReferEventFlagStatus"; }
                            else if (nid == 0x369ED59D) { syscall_code = 0x22243; func_name = "sceKernelGetSystemTimeLow"; }
                            else if (nid == 0x616403BA) { syscall_code = 0x22244; func_name = "sceKernelTerminateThread"; }
                            else if (nid == 0x383F7BCC) { syscall_code = 0x22245; func_name = "sceKernelTerminateDeleteThread"; }
                            else if (nid == 0xBC6FEBC5) { syscall_code = 0x22246; func_name = "sceKernelReferSemaStatus"; }
                            else if (nid == 0xC07BB470) { syscall_code = 0x22233; func_name = "sceKernelCreateFpl"; }
                            else if (nid == 0xED1410E0) { syscall_code = 0x22234; func_name = "sceKernelDeleteFpl"; }
                            else if (nid == 0xD979E9BF || nid == 0xE7282CB6 ||
                                     nid == 0x623AE665) {
                                syscall_code = 0x22235;
                                func_name = "sceKernelAllocateFpl";
                            }
                            else if (nid == 0xF6414A71) { syscall_code = 0x22236; func_name = "sceKernelFreeFpl"; }
                            else if (nid == 0x293B45B8) { syscall_code = 0x22237; func_name = "sceKernelGetThreadId"; }
                            else if (nid == 0x82BC5777) { syscall_code = 0x2223E; func_name = "sceKernelGetSystemTimeWide"; }
                            else if (nid == 0x58B1F937) { syscall_code = 0x22238; func_name = "sceKernelPollSema"; }
                        } else if (strcmp(lib_name, "ThreadManForKernel") == 0) {
                            if (nid == 0xD6DA4BA1) { syscall_code = 0x22221; func_name = "sceKernelCreateSema"; }
                            else if (nid == 0x4E3A1105) { syscall_code = 0x22222; func_name = "sceKernelWaitSema"; }
                            else if (nid == 0x3F53E640) { syscall_code = 0x22223; func_name = "sceKernelSignalSema"; }
                            else if (nid == 0x28B6489C) { syscall_code = 0x2223D; func_name = "sceKernelDeleteSema"; }
                            else if (nid == 0x58B1F937) { syscall_code = 0x22238; func_name = "sceKernelPollSema"; }
                            else if (nid == 0x82BC5777) { syscall_code = 0x2223E; func_name = "sceKernelGetSystemTimeWide"; }
                        } else if (strcmp(lib_name, "Kernel_Library") == 0) {
                            if (nid == 0x092968F4) { syscall_code = 0x22239; func_name = "sceKernelCpuSuspendIntr"; }
                            else if (nid == 0x5F10D406) { syscall_code = 0x2223A; func_name = "sceKernelCpuResumeIntr"; }
                            else if (nid == 0x3B84732D) { syscall_code = 0x2223B; func_name = "sceKernelCpuResumeIntrWithSync"; }
                            else if (nid == 0xB55249D2) { syscall_code = 0x2223C; func_name = "sceKernelIsCpuIntrEnable"; }
                        } else if (strcmp(lib_name, "SysMemUserForUser") == 0) {
                            if (nid == 0x342061E5 || nid == 0xF919F628) { syscall_code = 0x90001; func_name = "sceKernelTotalFreeMemSize"; }
                            else if (nid == 0xA291F107 || nid == 0xF77D77CB) { syscall_code = 0x90002; func_name = "sceKernelMaxFreeMemSize"; }
                            else if (nid == 0x237DBD4F) { syscall_code = 0x90003; func_name = "sceKernelAllocPartitionMemory"; }
                            else if (nid == 0x9D9A5BA1) { syscall_code = 0x90004; func_name = "sceKernelGetBlockHeadAddr"; }
                            else if (nid == 0xB6D61D02) { syscall_code = 0x90005; func_name = "sceKernelFreePartitionMemory"; }
                            else if (nid == 0x13A5ABEF) { syscall_code = 0x90006; func_name = "sceKernelPrintf"; }
                        } else if (strcmp(lib_name, "sceGe_Driver") == 0 || strcmp(lib_name, "sceGe") == 0 || strcmp(lib_name, "sceGe_user") == 0) {
                            if (nid == 0xAB49E76A || nid == 0xAB49E7EC) { syscall_code = 0x33331; func_name = "sceGeListEnqueue"; }
                            else if (nid == 0x03444EB4 || nid == 0x034C113E) { syscall_code = 0x33332; func_name = "sceGeListSync"; }
                            else if (nid == 0xE47E40E4) { syscall_code = 0x33333; func_name = "sceGeEdramGetAddr"; }
                            else if (nid == 0x1F6752AD) { syscall_code = 0x33334; func_name = "sceGeEdramGetSize"; }
                            else if (nid == 0xB287BD61) { syscall_code = 0x33335; func_name = "sceGeDrawSync"; }
                            else if (nid == 0xE0D68148) { syscall_code = 0x33336; func_name = "sceGeListUpdateStallAddr"; }
                            else if (nid == 0xA4FC06A4) { syscall_code = 0x33337; func_name = "sceGeSetCallback"; }
                            else if (nid == 0x05DB22CE) { syscall_code = 0x33338; func_name = "sceGeUnsetCallback"; }
                            else if (nid == 0xB77905EA) { syscall_code = 0x33339; func_name = "sceGeEdramSetAddrTranslation"; }
                        } else if (strcmp(lib_name, "sceDisplay") == 0) {
                            if (nid == 0x0E20F177) { syscall_code = 0x60001; func_name = "sceDisplaySetMode"; }
                            else if (nid == 0x289D82FE) { syscall_code = 0x60002; func_name = "sceDisplaySetFrameBuf"; }
                            else if (nid == 0x984C27E7) { syscall_code = 0x60003; func_name = "sceDisplayWaitVblankStart"; }
                            else if (nid == 0x9C6EAAD7) { syscall_code = 0x60004; func_name = "sceDisplayGetVblankCounter"; }
                            else if (nid == 0xDBA6C4C4) { syscall_code = 0x60005; func_name = "sceDisplayWaitVblankStartCB"; }
                            else if (nid == 0x9C6EAAD7) { syscall_code = 0x60004; func_name = "sceDisplayGetVcount"; }
                        } else if (strcmp(lib_name, "sceCtrl") == 0) {
                            if (nid == 0x1F4011E6 || nid == 0x1F803938) { syscall_code = 0x70001; func_name = "sceCtrlReadBufferPositive"; }
                            else if (nid == 0x3A622550) { syscall_code = 0x70002; func_name = "sceCtrlPeekBufferPositive"; }
                            else if (nid == 0x6A2774F3) { syscall_code = 0x70003; func_name = "sceCtrlSetSamplingCycle"; }
                            else if (nid == 0xA7144800) { syscall_code = 0x70004; func_name = "sceCtrlSetSamplingMode"; }
                        } else if (strcmp(lib_name, "sceAudio") == 0) {
                            if (nid == 0x5EC81C55) { syscall_code = 0x40001; func_name = "sceAudioChReserve"; }
                            else if (nid == 0x6FC46853) { syscall_code = 0x40002; func_name = "sceAudioChRelease"; }
                            else if (nid == 0x13F592BC) { syscall_code = 0x40003; func_name = "sceAudioOutputPannedBlocking"; }
                            else if (nid == 0xE2D56B2D) { syscall_code = 0x40004; func_name = "sceAudioOutputPanned"; }
                            else if (nid == 0x14074D7E || nid == 0x08DF58A5) { syscall_code = 0x40003; func_name = "sceAudioOutputPannedBlocking"; }
                            else if (nid == 0xCB2E439E) { syscall_code = 0x40005; func_name = "sceAudioSetChannelDataLen"; }
                        } else if (strcmp(lib_name, "sceAudiocodec") == 0) {
                            if (nid == 0x9D3F790C) { syscall_code = MIPS_HLE_AUDIOCODEC_CHECK_NEED_MEM; func_name = "sceAudiocodecCheckNeedMem"; }
                            else if (nid == 0x3A20A200) { syscall_code = MIPS_HLE_AUDIOCODEC_GET_EDRAM; func_name = "sceAudiocodecGetEDRAM"; }
                        } else if (strcmp(lib_name, "sceSasCore") == 0) {
                            if (nid == 0x42778A9F) { syscall_code = MIPS_HLE_SAS_INIT; func_name = "__sceSasInit"; }
                            else if (nid == 0xA3589D81) { syscall_code = MIPS_HLE_SAS_CORE; func_name = "__sceSasCore"; }
                            else if (nid == 0x68A46B95) { syscall_code = MIPS_HLE_SAS_END_FLAG; func_name = "__sceSasGetEndFlag"; }
                            else if (nid == 0xE175EF66) { syscall_code = MIPS_HLE_SAS_GET_OUTPUT_MODE; func_name = "__sceSasGetOutputMode"; }
                            else if (nid == 0x33D4AB37) { syscall_code = MIPS_HLE_SAS_REV_TYPE; func_name = "__sceSasRevType"; }
                            else if (nid == 0xF983B186) { syscall_code = MIPS_HLE_SAS_REV_VON; func_name = "__sceSasRevVON"; }
                            else if (nid == 0xD5A229C9) { syscall_code = MIPS_HLE_SAS_REV_EVOL; func_name = "__sceSasRevEVOL"; }
                            else if (nid == 0x267A6DD2) { syscall_code = MIPS_HLE_SAS_REV_PARAM; func_name = "__sceSasRevParam"; }
                            else if (nid == 0x99944089) { syscall_code = MIPS_HLE_SAS_SET_VOICE; func_name = "__sceSasSetVoice"; }
                            else if (nid == 0x76F01ACA) { syscall_code = MIPS_HLE_SAS_KEY_ON; func_name = "__sceSasSetKeyOn"; }
                        } else if (strcmp(lib_name, "sceUmdUser") == 0) {
                            if (nid == 0x46EBB729) { syscall_code = 0x80001; func_name = "sceUmdCheckMedium"; }
                            else if (nid == 0xC6183D47) { syscall_code = 0x80002; func_name = "sceUmdActivate"; }
                            else if (nid == 0x6B4A146C) { syscall_code = 0x80003; func_name = "sceUmdGetDriveStat"; }
                            else if (nid == 0x56202973) { syscall_code = 0x80004; func_name = "sceUmdWaitDriveStatWithTimer"; }
                        }

                        if (syscall_code != 0) {
                            mips_write32(cpu, target_stub_vaddr, (syscall_code << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            printf("    -> patched import: %s -> %s (NID: 0x%08X) -> custom syscall: 0x%X\n", lib_name, func_name, nid, syscall_code);
                            patch_count++;
                        } else {
                            bool critical = critical_import_library(lib_name);
                            if (fallback_stubs_count >= MAX_PATCHED_FALLBACKS) {
                                printf("[HLE Linker Error] Fallback table cheia; import %s NID 0x%08X nao pode ser ligado.\n",
                                       lib_name, nid);
                                if (critical) unresolved_critical_imports++;
                                continue;
                            }
                            uint32_t fallback_code = 0x50000 + (uint32_t)fallback_stubs_count;
                            strncpy(fallback_stubs[fallback_stubs_count].lib_name, lib_name,
                                    sizeof(fallback_stubs[fallback_stubs_count].lib_name) - 1);
                            fallback_stubs[fallback_stubs_count].lib_name[
                                sizeof(fallback_stubs[fallback_stubs_count].lib_name) - 1] = '\0';
                            fallback_stubs[fallback_stubs_count].nid = nid;
                            fallback_stubs[fallback_stubs_count].syscall_code = fallback_code;
                            fallback_stubs[fallback_stubs_count].library_version =
                                stub_hdr->version;
                            fallback_stubs_count++;
                            mips_write32(cpu, target_stub_vaddr, (fallback_code << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            printf("    -> patched import (fallback): %s -> NID: 0x%08X -> registered fallback (custom code 0x%X)%s\n",
                                   lib_name, nid, fallback_code,
                                   critical ? " [critical-unimplemented]" : " [non-critical]");
                            patch_count++;
                        }
                        total_stubs++;
                    }
                } else {
                    printf("[HLE Linker Error] Invalid NID/stub table for '%s' (num_funcs=%u, NID=0x%08X, stubs=0x%08X); no memory was patched.\n",
                           lib_name, stub_hdr->num_funcs, nid_table_vaddr, stub_table_vaddr);
                    malformed_import_tables++;
                    if (critical_import_library(lib_name)) unresolved_critical_imports++;
                    /*
                     * Do not continue walking a potentially corrupt header:
                     * its struct_size is the only trusted way to find the next
                     * library, and malformed tables must not cause writes.
                     */
                    break;
                }

                uint32_t step = stub_hdr->struct_size;
                if (step != 5 && step != 6) {
                    printf("[HLE Linker WARN] struct_size incomum (%u) em 0x%08X — possível corrupção/desalinhamento na tabela de stubs.\n",
                           step, curr_stub_addr);
                }
                if (step < 5 || step > 32) step = 5;
                if ((size_t)(stub_btm_addr - curr_stub_addr) < (size_t)step * 4) {
                    printf("[HLE Linker Error] Library stub step crosses import-table boundary at 0x%08X.\n",
                           curr_stub_addr);
                    malformed_import_tables++;
                    break;
                }
                curr_stub_addr += step * 4;
            }
            printf("[HLE Linker] Module '%s' resolved and patched %d / %d imported stubs successfully!\n", 
                   mod_info->modName, patch_count, total_stubs);
            total_patches += patch_count;
        }
    }
    printf("[HLE Linker] Dynamic Linker Phase Completed. Total Patched Stubs across all modules: %d.\n", total_patches);

    free(data);

    if (malformed_import_tables > 0) {
        printf("[Loader Error] Linker rejected %d malformed import table(s); %d critical imports remain explicit runtime fallbacks.\n",
               malformed_import_tables, unresolved_critical_imports);
        return -12;
    }

    if (g_next_module_load_addr != 0) {
        g_next_module_load_addr = (g_binary_segment_end + 0xFFFF) & ~0xFFFF;
        printf("[Loader] Next dynamic module load address advanced to: 0x%08X\n", g_next_module_load_addr);
    }

    return 0;
}

int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point) {
    FILE *file;
    long file_size;
    uint8_t *data;
    size_t read_size;

    if (filepath == NULL || entry_point == NULL) return -1;
    file = fopen(filepath, "rb");
    if (file == NULL) {
        printf("[Loader Error] Failed to open game executable: %s\n", filepath);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        printf("[Loader Error] Failed to determine game executable size: %s\n", filepath);
        return -2;
    }
    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        printf("[Loader Error] Out of memory allocating executable buffer.\n");
        return -2;
    }
    read_size = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size) {
        free(data);
        printf("[Loader Error] Failed to read full executable: %s\n", filepath);
        return -3;
    }
    return load_elf_buffer(cpu, data, read_size, entry_point, 0, false);
}

int load_prx_elf_buffer(MIPS_CPU *cpu, uint8_t *elf_data, size_t elf_size,
                        uint32_t *entry_point, uint32_t *module_start,
                        uint32_t *module_stop, uint32_t *module_gp) {
    if (elf_data == NULL || elf_size < sizeof(Elf32_Ehdr) ||
        entry_point == NULL || module_start == NULL || module_stop == NULL ||
        module_gp == NULL ||
        memcmp(elf_data, "\x7f" "ELF", 4) !=  0) {
        free(elf_data);
        return -6;
    }
    last_module_start = 0;
    last_module_stop = 0;
    last_module_gp = 0;
    extern uint32_t g_next_module_load_addr;
    const uint32_t load_bias = g_next_module_load_addr != 0
        ? g_next_module_load_addr : PSP_USER_LOAD_BASE;
    const int result = load_elf_buffer(cpu, elf_data, elf_size, entry_point,
                                       load_bias, true);
    if (result == 0) {
        *module_start = last_module_start;
        *module_stop = last_module_stop;
        *module_gp = last_module_gp;
    }
    return result;
}
