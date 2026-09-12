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

static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias, uint32_t mod_addr);
static void resolve_module_info_section_headers(MIPS_CPU *cpu, SceModuleInfo *mod, Elf32_Shdr *sh_table, int sh_num, const char *shstrtab, uint32_t load_bias);
int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point);

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

static inline uint32_t resolve_stub_address(MIPS_CPU *cpu, uint32_t stub_val, uint32_t mod_addr, uint32_t load_bias) {
    if (stub_val == 0) return 0;
    if (stub_val >= 0x08800000 && stub_val < 0x0C000000) {
        return stub_val;
    }

    // Candidate 1: relative byte offset
    uint32_t cand1 = stub_val + load_bias;
    SceLibraryStubHeader *hdr1 = (SceLibraryStubHeader *)mips_get_ptr(cpu, cand1);
    if (hdr1 && hdr1->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr1->lib_name_ptr, load_bias);
        const char *name = (const char *)mips_get_ptr(cpu, name_vaddr);
        if (name && name[0] >= 32 && name[0] < 127) {
            return cand1;
        }
    }

    // Candidate 2: word offset relative to the SceModuleInfo structure address
    uint32_t cand2 = mod_addr + (stub_val * 4);
    SceLibraryStubHeader *hdr2 = (SceLibraryStubHeader *)mips_get_ptr(cpu, cand2);
    if (hdr2 && hdr2->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr2->lib_name_ptr, load_bias);
        const char *name = (const char *)mips_get_ptr(cpu, name_vaddr);
        if (name && name[0] >= 32 && name[0] < 127) {
            return cand2;
        }
    }

    // Candidate 3: byte offset relative to SceModuleInfo
    uint32_t cand3 = mod_addr + stub_val;
    SceLibraryStubHeader *hdr3 = (SceLibraryStubHeader *)mips_get_ptr(cpu, cand3);
    if (hdr3 && hdr3->lib_name_ptr != 0) {
        uint32_t name_vaddr = relocate_addr(hdr3->lib_name_ptr, load_bias);
        const char *name = (const char *)mips_get_ptr(cpu, name_vaddr);
        if (name && name[0] >= 32 && name[0] < 127) {
            return cand3;
        }
    }

    return cand1;
}

static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias, uint32_t mod_addr) {
    if (!mod) { printf("[HLE Linker DEBUG] is_valid_module_info: mod == NULL (endereco 0x%08X nao mapeado)\n", mod_addr); return 0; }

    // NOVO: dump bruto dos 64 primeiros bytes da estrutura (hex + ASCII),
    // pra conferirmos visualmente onde os campos de verdade estao, em vez
    // de continuar assumindo o layout classico de 52 bytes (que pode estar
    // desalinhado nessa build - a secao real tem 64 bytes, 12 a mais que o
    // struct que a gente usa).
    {
        const uint8_t *raw = (const uint8_t *)mod;
        printf("[HLE Linker DEBUG] Dump bruto de SceModuleInfo em 0x%08X (64 bytes):\n", mod_addr);
        for (int row = 0; row < 4; row++) {
            printf("  +0x%02X:", row * 16);
            for (int col = 0; col < 16; col++) {
                printf(" %02X", raw[row * 16 + col]);
            }
            printf("  |");
            for (int col = 0; col < 16; col++) {
                uint8_t b = raw[row * 16 + col];
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
        SceLibraryStubHeader *stub_hdr = (SceLibraryStubHeader *)mips_get_ptr(cpu, stub_top);
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

        const char *lib_name = (const char *)mips_get_ptr(cpu, lib_name_vaddr);
        if (!lib_name) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): mips_get_ptr(lib_name_vaddr=0x%08X) retornou NULL\n",
                   name_len, mod->modName, lib_name_vaddr);
            return 0;
        }

        if (lib_name[0] < 32 || lib_name[0] >= 127) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): lib_name[0]=0x%02X em 0x%08X nao e ASCII imprimivel\n",
                   name_len, mod->modName, (unsigned char)lib_name[0], lib_name_vaddr);
            return 0;
        }
        int lib_name_len = 0;
        while (lib_name_len < 128) {
            char c = lib_name[lib_name_len];
            if (c == '\0') break;
            if (c < 32 || c >= 127) {
                printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): lib_name[%d]=0x%02X nao e ASCII imprimivel (nome ate aqui: '%.*s')\n",
                       name_len, mod->modName, lib_name_len, (unsigned char)c, lib_name_len, lib_name);
                return 0;
            }
            lib_name_len++;
        }
        if (lib_name_len == 0) {
            printf("[HLE Linker DEBUG] is_valid_module_info REJEITADO ('%.*s'): lib_name vazio em 0x%08X\n",
                   name_len, mod->modName, lib_name_vaddr);
            return 0;
        }
    }

    printf("[HLE Linker DEBUG] is_valid_module_info: '%.*s' VALIDADO com sucesso.\n", name_len, mod->modName);
    return 1;
}

static void resolve_module_info_section_headers(MIPS_CPU *cpu, SceModuleInfo *mod, Elf32_Shdr *sh_table, int sh_num, const char *shstrtab, uint32_t load_bias) {
    (void)cpu;
    if (!mod) return;

    uint32_t lib_stub_addr = 0;
    uint32_t lib_stub_size = 0;

    if (shstrtab != NULL) {
        for (int i = 0; i < sh_num; i++) {
            const char *sect_name = shstrtab + sh_table[i].sh_name;
            if (strcmp(sect_name, ".lib.stub") == 0) {
                lib_stub_addr = sh_table[i].sh_addr + load_bias;
                lib_stub_size = sh_table[i].sh_size;
                break;
            }
        }
    }

    if (lib_stub_addr != 0 && (mod->stub_top >= (uint32_t)sh_num || strcmp(mod->modName, "s") == 0)) {
        printf("[HLE Linker ACTIVE CORRUPTION DETECTOR] Tentando reescrever mod->stub_top/btm para o modulo '%s'!\n", mod->modName);
        printf("  -> GP lido do modulo: 0x%08X, stub_top atual (sera reescrito): 0x%08X, stub_btm atual: 0x%08X\n", mod->gp_value, mod->stub_top, mod->stub_btm);
        printf("  -> Novo stub_top: 0x%08X, Novo stub_btm: 0x%08X\n", lib_stub_addr, lib_stub_addr + lib_stub_size);
        printf("[HLE Linker] Overriding stale indices for module '%s' using real ELF '.lib.stub' section: VAddr 0x%08X (Size: %u bytes)\n",
               mod->modName, lib_stub_addr, lib_stub_size);
        mod->stub_top = lib_stub_addr;
        mod->stub_btm = lib_stub_addr + lib_stub_size;
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
    (void)elf_size;

    uint32_t modinfo_offset1 = 0;
    uint32_t modinfo_offset2 = 0;

    if (size >= 4 && memcmp(data, "~PSP", 4) == 0) {
        printf("[Loader] Detected ~PSP (PRX) executable container.\n");
        modinfo_offset1 = *(uint32_t*)&data[52];

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
            uint16_t comp_type = *(uint16_t*)&data[6];
            if ((comp_type & 0xF00) != 0x300) {
                printf("[Loader Error] PRX file is encrypted or compressed (Type 0x%03X).\n", comp_type & 0xF00);
                free(data);
                return -4;
            }

            uint32_t boot_entry = *(uint32_t*)&data[48];
            printf("[Loader] Mapping flat program space directly to standard base address 0x08804000.\n");

            void *dest = mips_get_ptr(cpu, 0x08804000);
            if (!dest) {
                printf("[Loader Error] Target physical memory at 0x08804000 is unavailable.\n");
                free(data);
                return -5;
            }
            memcpy(dest, data + 0x150, size - 0x150);
            *entry_point = boot_entry + 0x08804000;
            free(data);
            return 0;
        }
    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        printf("[Loader Error] Game binary does not have a valid ELF magic number.\n");
        free(data);
        return -6;
    }

    extern uint32_t g_next_module_load_addr;
    uint32_t load_bias = 0;
    if (ehdr->e_entry < 0x08000000) {
        if (g_next_module_load_addr != 0) {
            load_bias = g_next_module_load_addr;
            printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying dynamic load bias: 0x%08X\n", ehdr->e_entry, load_bias);
        } else {
            load_bias = 0x08800000;
            printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying PSP user RAM load bias: 0x%08X\n", ehdr->e_entry, load_bias);
        }
    }

    *entry_point = ehdr->e_entry + load_bias;
    printf("[Loader] Successfully parsed MIPS ELF Header. Entry Point: 0x%08X\n", *entry_point);

    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    int loaded_count = 0;
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
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD
            uint32_t target_vaddr = phdr->p_vaddr + load_bias;
            printf("  -> Loading PT_LOAD segment %d: VADDR 0x%08X, File Offset: 0x%08X, File Size: %u bytes, Memory Size: %u bytes\n",
            loaded_count, target_vaddr, phdr->p_offset, phdr->p_filesz, phdr->p_memsz);

            if (loaded_count == 0) {
                seg0_file_offset = phdr->p_offset;
                seg0_target_vaddr = target_vaddr;
            }

            void *dest = mips_get_ptr(cpu, target_vaddr);
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
            if (seg_end > g_binary_segment_end) {
                g_binary_segment_end = seg_end;
            }

            loaded_count++;
        }
    }

    printf("[Loader] Successfully loaded %d executable segments into emulated memory spaces.\n", loaded_count);

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
            uint32_t file_word = *(uint32_t *)(elf_data + seg0_file_offset + off);
            printf("  offset=0x%06X | arquivo=%08X | RAM=%08X | %s\n",
                off, file_word, ram_word ? *ram_word : 0xDEADDEAD,
                (ram_word && file_word == *ram_word) ? "OK" : "DIVERGIU");
        }
    }

    const char *shstrtab = NULL;
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        if (ehdr->e_shstrndx < ehdr->e_shnum) {
            shstrtab = (const char *)(elf_data + sh_table[ehdr->e_shstrndx].sh_offset);
        }
    }

    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *dbg_sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
        printf("[DEBUG SECTIONS] Total: %d\n", ehdr->e_shnum);
        for (int s = 0; s < ehdr->e_shnum; s++) {
            const char *sname = (shstrtab != NULL) ? (shstrtab + dbg_sh_table[s].sh_name) : "???";
            printf("  [%2d] name='%s' type=0x%X offset=0x%X size=%u link=%u info=%u entsize=%u\n",
                   s, sname, dbg_sh_table[s].sh_type, dbg_sh_table[s].sh_offset,
                   dbg_sh_table[s].sh_size, dbg_sh_table[s].sh_link, dbg_sh_table[s].sh_info,
                   dbg_sh_table[s].sh_entsize);
        }
    }

    mips_apply_relocations(cpu, elf_data, load_bias, entry_point);

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
            const char *sname = shstrtab + sh_table[s].sh_name;
            if (strcmp(sname, ".rodata.sceModuleInfo") == 0) {
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
                        if (offset >= phdr->p_offset && offset < phdr->p_offset + phdr->p_filesz) {
                            uint32_t relative_offset = offset - phdr->p_offset;
                            uint32_t file_addr = phdr->p_vaddr + relative_offset + load_bias;
                            SceModuleInfo *fmod = (SceModuleInfo *)mips_get_ptr(cpu, file_addr);
                            if (fmod && is_valid_module_info(cpu, fmod, load_bias, file_addr)) {
                                printf("[HLE Linker] Resolving section headers for valid module '%s' at 0x%08X...", fmod->modName, file_addr);
                                resolve_module_info_section_headers(cpu, fmod, sh_table, sh_num, shstrtab, load_bias);
                            }
                        }
                    }
                }
            }
        }
    }

    uint32_t resolved_addrs[32];
    int resolved_count = 0;

    uint32_t offsets[3] = { modinfo_offset1, modinfo_offset2, modinfo_offset3 };
    for (int o = 0; o < 3; o++) {
        uint32_t offset = offsets[o];
        if (offset != 0) {
            for (int i = 0; i < ehdr->e_phnum; i++) {
                Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
                if (phdr->p_type == 1) {
                    if (offset >= phdr->p_offset && offset < phdr->p_offset + phdr->p_filesz) {
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

            printf("[HLE Linker] >>> Patching Module %d/%d: '%s' (Attributes: 0x%04X, GP: 0x%08X) <<<\n", 
                   m + 1, resolved_count, mod_info->modName, mod_info->modAttribute, current_gp);
            printf("[HLE Linker DEBUG] Module '%s' raw fields: gp_value=0x%08X, ent_top=0x%08X, ent_btm=0x%08X, stub_top=0x%08X, stub_btm=0x%08X\n",
                   mod_info->modName, mod_info->gp_value, mod_info->ent_top, mod_info->ent_btm, mod_info->stub_top, mod_info->stub_btm);

            uint32_t stub_top_addr = resolve_stub_address(cpu, mod_info->stub_top, mod_addr, load_bias);
            uint32_t stub_btm_addr = resolve_stub_address(cpu, mod_info->stub_btm, mod_addr, load_bias);
            printf("  -> Stub table range: 0x%08X - 0x%08X\n", stub_top_addr, stub_btm_addr);

            uint32_t curr_stub_addr = stub_top_addr;
            int patch_count = 0;
            int total_stubs = 0;

            while (curr_stub_addr < stub_btm_addr) {
                SceLibraryStubHeader *stub_hdr = (SceLibraryStubHeader *)mips_get_ptr(cpu, curr_stub_addr);
                if (stub_hdr == NULL || stub_hdr->lib_name_ptr == 0) {
                    break;
                }

                uint32_t lib_name_vaddr = relocate_addr(stub_hdr->lib_name_ptr, load_bias);
                char lib_name_buf[64];
                if (!safe_read_cstring(cpu, lib_name_vaddr, lib_name_buf, sizeof(lib_name_buf))) {
                    curr_stub_addr += 4;
                    continue;
                }
                const char *lib_name = lib_name_buf;

                uint32_t nid_table_vaddr = relocate_addr(stub_hdr->nid_table_ptr, load_bias);
                uint32_t *nid_table = (uint32_t *)mips_get_ptr(cpu, nid_table_vaddr);

                uint32_t stub_table_vaddr = relocate_addr(stub_hdr->stub_table_ptr, load_bias);

                if (nid_table != NULL && stub_table_vaddr != 0 &&
                    stub_hdr->num_funcs > 0 && stub_hdr->num_funcs < 512) {

                    uint32_t num_stubs_to_patch = stub_hdr->num_funcs;
                    if (num_stubs_to_patch > 1024) {
                        num_stubs_to_patch = 1024;
                    }

                    for (uint32_t j = 0; j < num_stubs_to_patch; j++) {
                        uint32_t nid = nid_table[j];
                        uint32_t target_stub_vaddr = stub_table_vaddr + j * 8;

                        if (target_stub_vaddr < 0x08800000 || target_stub_vaddr >= g_binary_segment_end) {
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
                            else if (nid == 0x810C4BC3) { syscall_code = 0x11114; func_name = "sceIoClose"; }
                            else if (nid == 0x54F5FB11) { syscall_code = 0x11115; func_name = "sceIoDevctl"; }
                            else if (nid == 0x63632449) { syscall_code = 0x11118; func_name = "sceIoIoctl"; }
                            else if (nid == 0xE95A012B) { syscall_code = 0x11119; func_name = "sceIoIoctlAsync"; }
                            else if (nid == 0xE23EEC33) { syscall_code = 0x1111A; func_name = "sceIoWaitAsync"; }
                            else if (nid == 0x3251EA56) { syscall_code = 0x1111B; func_name = "sceIoPollAsync"; }
                            else if (nid == 0x55F4717D) { syscall_code = 0x1111E; func_name = "sceIoGetstat"; }
                            else if (nid == 0xB29DDF9C) { syscall_code = 0x1111F; func_name = "sceIoDopen"; }
                            else if (nid == 0xE3EB004C) { syscall_code = 0x11120; func_name = "sceIoDread"; }
                            else if (nid == 0xEB092469) { syscall_code = 0x11121; func_name = "sceIoDclose"; }
                        } else if (strcmp(lib_name, "ThreadManForUser") == 0) {
                            if (nid == 0xD6D016D7 || nid == 0x3F53A0F3 || nid == 0x3F53E640) { syscall_code = 0x22221; func_name = "sceKernelCreateSema"; }
                            else if (nid == 0x4E3A1105) { syscall_code = 0x22222; func_name = "sceKernelWaitSema"; }
                            else if (nid == 0xCEADEB85 || nid == 0xCEADEB47) { syscall_code = 0x22223; func_name = "sceKernelSignalSema"; }
                            else if (nid == 0x2C1184E6 || nid == 0x9ACE131E) { syscall_code = 0x22224; func_name = "sceKernelDelayThread"; }
                            else if (nid == 0xF6414A71 || nid == 0xD6DA4BA1 || nid == 0x446D8DE6) { syscall_code = 0x22225; func_name = "sceKernelCreateThread"; }
                            else if (nid == 0xF475845D) { syscall_code = 0x22226; func_name = "sceKernelStartThread"; }
                            else if (nid == 0xAA73C935 || nid == 0x8011F9B0) { syscall_code = 0x22227; func_name = "sceKernelExitThread"; }
                            else if (nid == 0x68DA9E36) { syscall_code = 0x22228; func_name = "sceKernelCheckCallback"; }
                            else if (nid == 0xEDBA5844) { syscall_code = 0x22229; func_name = "sceKernelDeleteCallback"; }
                        } else if (strcmp(lib_name, "SysMemUserForUser") == 0) {
                            // FIX: 0x237DBD4F e 0x9D9A5BA1 estavam trocados.
                            // Confirmado em duas fontes independentes (STUB
                            // table original do ps2dev forum + lista de
                            // syscalls do FW 4.50): 0x237DBD4F e' o NID real
                            // de sceKernelAllocPartitionMemory (nao
                            // TotalFreeMemSize), e 0x9D9A5BA1 e' o NID real
                            // de sceKernelGetBlockHeadAddr (nao
                            // AllocPartitionMemory). Isso fazia o jogo
                            // chamar Alloc de verdade e cair no handler de
                            // TotalFreeMemSize (que ignora os 4 argumentos e
                            // so devolve 24MB fixo), e depois chamar
                            // GetBlockHeadAddr(block_id) e cair no handler
                            // de Alloc (que le 4 argumentos que o jogo nunca
                            // preparou) - exatamente o bug do
                            // "part=size=25165824, name='block'".
                            if (nid == 0x342061E5) { syscall_code = 0x90001; func_name = "sceKernelTotalFreeMemSize"; }
                            else if (nid == 0xA291F107 || nid == 0xF77D77CB) { syscall_code = 0x90002; func_name = "sceKernelMaxFreeMemSize"; }
                            else if (nid == 0x237DBD4F) { syscall_code = 0x90003; func_name = "sceKernelAllocPartitionMemory"; }
                            else if (nid == 0x9D9A5BA1 || nid == 0x13A5ABEF) { syscall_code = 0x90004; func_name = "sceKernelGetBlockHeadAddr"; }
                            else if (nid == 0xB6D61D02) { syscall_code = 0x90005; func_name = "sceKernelFreePartitionMemory"; }
                        } else if (strcmp(lib_name, "sceGe_Driver") == 0 || strcmp(lib_name, "sceGe") == 0 || strcmp(lib_name, "sceGe_user") == 0) {
                            if (nid == 0xAB49E76A || nid == 0xAB49E7EC) { syscall_code = 0x33331; func_name = "sceGeListEnqueue"; }
                            else if (nid == 0x03444EB4 || nid == 0x034C113E) { syscall_code = 0x33332; func_name = "sceGeListSync"; }
                            else if (nid == 0xE47E40E4) { syscall_code = 0x33333; func_name = "sceGeEdramGetAddr"; }
                            else if (nid == 0x1F6752AD) { syscall_code = 0x33334; func_name = "sceGeEdramGetSize"; }
                            else if (nid == 0xB287BD61) { syscall_code = 0x33335; func_name = "sceGeDrawSync"; }
                        } else if (strcmp(lib_name, "sceDisplay") == 0) {
                            if (nid == 0x0E20F177) { syscall_code = 0x60001; func_name = "sceDisplaySetMode"; }
                            else if (nid == 0x289D82FE) { syscall_code = 0x60002; func_name = "sceDisplaySetFrameBuf"; }
                            else if (nid == 0x984C27E7) { syscall_code = 0x60003; func_name = "sceDisplayWaitVblankStart"; }
                            else if (nid == 0x9C6EAAD7) { syscall_code = 0x60004; func_name = "sceDisplayGetVblankCounter"; }
                            else if (nid == 0xDBA6C4C4) { syscall_code = 0x60005; func_name = "sceDisplayWaitVblankStartCB"; }
                        } else if (strcmp(lib_name, "sceCtrl") == 0) {
                            if (nid == 0x1F4011E6 || nid == 0x1F803938) { syscall_code = 0x70001; func_name = "sceCtrlReadBufferPositive"; }
                            else if (nid == 0x3A622550) { syscall_code = 0x70002; func_name = "sceCtrlPeekBufferPositive"; }
                            else if (nid == 0x6A2774F3) { syscall_code = 0x70003; func_name = "sceCtrlSetSamplingCycle"; }
                            else if (nid == 0xA7144800) { syscall_code = 0x70004; func_name = "sceCtrlSetSamplingMode"; }
                        } else if (strcmp(lib_name, "sceAudio") == 0) {
                            if (nid == 0x5EC81C55) { syscall_code = 0x40001; func_name = "sceAudioChReserve"; }
                            else if (nid == 0x13F592BC) { syscall_code = 0x40002; func_name = "sceAudioChRelease"; }
                            else if (nid == 0x14074D7E || nid == 0x08DF58A5) { syscall_code = 0x40003; func_name = "sceAudioOutputPanned"; }
                        } else if (strcmp(lib_name, "sceUmdUser") == 0) {
                            if (nid == 0x46EBB729) { syscall_code = 0x80001; func_name = "sceUmdCheckMedium"; }
                            else if (nid == 0xC6183D47) { syscall_code = 0x80002; func_name = "sceUmdGetDriveStatus"; }
                            else if (nid == 0x56202973) { syscall_code = 0x80003; func_name = "sceUmdWaitDriveStat"; }
                        }

                        if (syscall_code != 0) {
                            mips_write32(cpu, target_stub_vaddr, (syscall_code << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            printf("    -> patched import: %s -> %s (NID: 0x%08X) -> custom syscall: 0x%X\n", lib_name, func_name, nid, syscall_code);
                            patch_count++;
                        } else {
                            uint32_t fallback_code = 0x50000 + fallback_stubs_count;
                            if (fallback_stubs_count < MAX_PATCHED_FALLBACKS) {
                                strncpy(fallback_stubs[fallback_stubs_count].lib_name, lib_name, 31);
                                fallback_stubs[fallback_stubs_count].nid = nid;
                                fallback_stubs[fallback_stubs_count].syscall_code = fallback_code;
                                fallback_stubs_count++;
                            }
                            mips_write32(cpu, target_stub_vaddr, (fallback_code << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x00000000);
                            printf("    -> patched import (fallback): %s -> NID: 0x%08X -> fallback success (custom code 0x%X)\n", lib_name, nid, fallback_code);
                            patch_count++;
                        }
                        total_stubs++;
                    }
                } else if (nid_table != NULL) {
                    printf("[HLE Linker WARN] num_funcs suspeito (%u) para módulo com stub '%s' — struct_size provavelmente desalinhado. Ignorando entrada.\n",
                           stub_hdr->num_funcs, lib_name);
                }

                uint32_t step = stub_hdr->struct_size;
                if (step != 5 && step != 6) {
                    printf("[HLE Linker WARN] struct_size incomum (%u) em 0x%08X — possível corrupção/desalinhamento na tabela de stubs.\n",
                           step, curr_stub_addr);
                }
                if (step < 5 || step > 32) step = 5;
                curr_stub_addr += step * 4;
            }
            printf("[HLE Linker] Module '%s' resolved and patched %d / %d imported stubs successfully!\n", 
                   mod_info->modName, patch_count, total_stubs);
            total_patches += patch_count;
        }
    }
    printf("[HLE Linker] Dynamic Linker Phase Completed. Total Patched Stubs across all modules: %d.\n", total_patches);

    free(data);

    if (g_next_module_load_addr != 0) {
        g_next_module_load_addr = (g_binary_segment_end + 0xFFFF) & ~0xFFFF;
        printf("[Loader] Next dynamic module load address advanced to: 0x%08X\n", g_next_module_load_addr);
    }

    return 0;
}
