#include "mips_reloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

bool safe_read_cstring(MIPS_CPU *cpu, uint32_t vaddr, char *out, size_t out_size) {
    if (vaddr == 0 || out_size == 0) return false;
    for (size_t i = 0; i < out_size - 1; i++) {
        uint8_t *ptr = (uint8_t *)mips_get_ptr(cpu, vaddr + (uint32_t)i);
        if (!ptr) {
            out[i] = 0;
            return i > 0;
        }
        uint8_t byte = *ptr;
        out[i] = (char)byte;
        if (byte == 0) return true;
        if (byte < 32 || byte >= 127) {
            out[i] = 0;
            return false;
        }
    }
    out[out_size - 1] = 0;
    return false;
}

// Nomes legiveis dos tipos de relocacao MIPS (ABI padrao) - usado so pro log de diagnostico
static const char *mips_reloc_type_name(uint32_t type) {
    switch (type) {
        case 0:  return "R_MIPS_NONE";
        case 1:  return "R_MIPS_16";
        case 2:  return "R_MIPS_32";
        case 3:  return "R_MIPS_REL32";
        case 4:  return "R_MIPS_26";
        case 5:  return "R_MIPS_HI16";
        case 6:  return "R_MIPS_LO16";
        case 7:  return "R_MIPS_GPREL16";
        case 8:  return "R_MIPS_LITERAL";
        case 9:  return "R_MIPS_GOT16";
        case 10: return "R_MIPS_PC16";
        case 11: return "R_MIPS_CALL16";
        case 12: return "R_MIPS_GPREL32";
        default: return "UNKNOWN";
    }
}

extern uint32_t g_binary_segment_start;
extern uint32_t g_binary_segment_end;

static bool file_range_valid(size_t file_size, uint32_t offset, size_t size) {
    return (uint64_t)offset <= file_size &&
           (uint64_t)size <= (uint64_t)file_size - offset;
}

static bool loaded_range_valid(uint32_t addr, size_t size) {
    uint64_t end = (uint64_t)addr + size;
    return g_binary_segment_end > g_binary_segment_start &&
           addr >= g_binary_segment_start &&
           end <= g_binary_segment_end;
}

int mips_apply_relocations(MIPS_CPU *cpu, uint8_t *elf_data, size_t elf_size,
                           uint32_t load_bias, uint32_t *entry_point) {
    (void)entry_point;
    if (cpu == NULL || elf_data == NULL || elf_size < sizeof(Elf32_Ehdr)) {
        printf("[HLE Reloc Error] ELF buffer is missing or truncated.\n");
        return -1;
    }
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    if (ehdr->e_phentsize < sizeof(Elf32_Phdr) ||
        !file_range_valid(elf_size, ehdr->e_phoff,
                          (size_t)ehdr->e_phnum * ehdr->e_phentsize)) {
        printf("[HLE Reloc Error] Program-header table is outside the ELF buffer.\n");
        return -1;
    }
    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        printf("[HLE Reloc] ELF has no section headers; no relocations to apply.\n");
        return 0;
    }
    if (ehdr->e_shentsize != sizeof(Elf32_Shdr) ||
        !file_range_valid(elf_size, ehdr->e_shoff,
                          (size_t)ehdr->e_shnum * ehdr->e_shentsize)) {
        printf("[HLE Reloc Error] No Section Headers found for relocation.\n");
        return -1;
    }
    
    printf("[HLE Reloc PPSSPP] Starting PPSSPP-compliant 2-pass segment relocations (%d sections, %d phdr)...\n",
           ehdr->e_shnum, ehdr->e_phnum);
    
    Elf32_Shdr *sh_table = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
    int reloc_count = 0;
    int unhandled_count = 0;
    int unmapped_count = 0;
    uint32_t type_histogram[32] = {0};

    uint32_t *seg_vaddr = calloc(ehdr->e_phnum ? ehdr->e_phnum : 1,
                                 sizeof(*seg_vaddr));
    bool *seg_loaded = calloc(ehdr->e_phnum ? ehdr->e_phnum : 1,
                              sizeof(*seg_loaded));
    if (seg_vaddr == NULL || seg_loaded == NULL) {
        printf("[HLE Reloc Error] Out of memory for segment relocation bases.\n");
        free(seg_vaddr);
        free(seg_loaded);
        return -1;
    }
    uint16_t max_segs = ehdr->e_phnum;
    for (int p = 0; p < max_segs; p++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + p * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD
            uint64_t segment_addr = (uint64_t)phdr->p_vaddr + load_bias;
            if (segment_addr <= UINT32_MAX) {
                seg_vaddr[p] = (uint32_t)segment_addr;
                seg_loaded[p] = true;
            }
        }
    }

    for (int i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *sh = &sh_table[i];
        if (sh->sh_type == 9 || sh->sh_type == 0x700000A0) { // SHT_REL / SHT_PRXRELOC
            if (!file_range_valid(elf_size, sh->sh_offset, sh->sh_size) ||
                (sh->sh_size % sizeof(Elf32_Rel)) != 0 ||
                (sh->sh_entsize != 0 && sh->sh_entsize != sizeof(Elf32_Rel))) {
                printf("[HLE Reloc Error] Relocation section %d is truncated or malformed.\n", i);
                free(seg_vaddr);
                free(seg_loaded);
                return -1;
            }
            int count = sh->sh_size / 8;
            Elf32_Rel *rels = (Elf32_Rel *)(elf_data + sh->sh_offset);

            uint32_t *reloc_ops = calloc(count ? (size_t)count : 1,
                                         sizeof(*reloc_ops));
            uint32_t *reloc_targets = calloc(count ? (size_t)count : 1,
                                             sizeof(*reloc_targets));
            bool *reloc_valid = calloc(count ? (size_t)count : 1,
                                       sizeof(*reloc_valid));
            if (reloc_ops == NULL || reloc_targets == NULL ||
                reloc_valid == NULL) {
                printf("[HLE Reloc Error] Out of memory snapshotting relocation section %d.\n",
                       i);
                free(reloc_ops);
                free(reloc_targets);
                free(reloc_valid);
                free(seg_vaddr);
                free(seg_loaded);
                return -1;
            }

            for (int r = 0; r < count; r++) {
                uint32_t readwrite_idx = (rels[r].r_info >> 8) & 0xFF;
                if (readwrite_idx >= max_segs || !seg_loaded[readwrite_idx]) {
                    continue;
                }
                uint64_t target64 = (uint64_t)rels[r].r_offset +
                                    seg_vaddr[readwrite_idx];
                if (target64 > UINT32_MAX ||
                    !loaded_range_valid((uint32_t)target64, sizeof(uint32_t))) {
                    continue;
                }
                uint32_t *target_ptr = (uint32_t *)mips_get_ptr_range(
                    cpu, (uint32_t)target64, sizeof(uint32_t));
                if (target_ptr != NULL) {
                    reloc_ops[r] = *target_ptr;
                    reloc_targets[r] = (uint32_t)target64;
                    reloc_valid[r] = true;
                }
            }

            for (int r = 0; r < count; r++) {
                uint32_t r_info = rels[r].r_info;
                uint32_t r_type = r_info & 0x0F;
                uint32_t readwrite_idx = (r_info >> 8) & 0xFF;
                uint32_t relative_idx = (r_info >> 16) & 0xFF;

                if (readwrite_idx >= max_segs || !seg_loaded[readwrite_idx] ||
                    !reloc_valid[r]) {
                    unmapped_count++;
                    continue;
                }
                uint32_t target_vaddr = reloc_targets[r];
                uint32_t relocateTo =
                    relative_idx < max_segs && seg_loaded[relative_idx]
                        ? seg_vaddr[relative_idx] : 0;

                if (r_type < 32) type_histogram[r_type]++;

                uint32_t *ptr = (uint32_t *)mips_get_ptr_range(
                    cpu, target_vaddr, sizeof(uint32_t));
                uint32_t op = reloc_ops[r];
                if (r_type == 0) { // R_MIPS_NONE
                    continue;
                } else if (r_type == 1) { // R_MIPS_16
                    *ptr = (op & 0xFFFF0000) |
                           (((op & 0xFFFF) + relocateTo) & 0xFFFF);
                    reloc_count++;
                } else if (r_type == 2) { // R_MIPS_32
                    *ptr = op + relocateTo;
                    reloc_count++;
                } else if (r_type == 3) { // R_MIPS_REL32
                    *ptr = op + relocateTo;
                    reloc_count++;
                } else if (r_type == 4) { // R_MIPS_26
                    uint32_t target = (op & 0x03FFFFFF) + (relocateTo >> 2);
                    *ptr = (op & 0xFC000000) | (target & 0x03FFFFFF);
                    reloc_count++;
                } else if (r_type == 5) { // R_MIPS_HI16
                    bool found_lo = false;
                    for (int t = r + 1; t < count; t++) {
                        uint32_t lo_type = rels[t].r_info & 0x0F;
                        if (lo_type == 5) continue;
                        if (lo_type != 1 && lo_type != 6) continue;
                        if (!reloc_valid[t]) continue;

                        int32_t low = (int16_t)(reloc_ops[t] & 0xFFFF);
                        uint32_t full = ((op & 0xFFFF) << 16) +
                                        (uint32_t)low + relocateTo;
                        uint16_t high =
                            (uint16_t)((full - (int16_t)full) >> 16);
                        *ptr = (op & 0xFFFF0000) | high;
                        reloc_count++;
                        found_lo = true;
                        break;
                    }
                    if (!found_lo) {
                        unhandled_count++;
                        if (unhandled_count <= 20) {
                            printf("[HLE Reloc WARNING] R_MIPS_HI16 em 0x%08X sem par LO16/16 valido na secao %d (entry %d/%d); instrucao preservada.\n",
                                   target_vaddr, i, r, count);
                        }
                    }
                } else if (r_type == 6) { // R_MIPS_LO16
                    *ptr = (op & 0xFFFF0000) |
                           (((op & 0xFFFF) + relocateTo) & 0xFFFF);
                    reloc_count++;
                } else if (r_type == 7) { // R_MIPS_GPREL16
                    continue;
                } else {
                    unhandled_count++;
                    if (unhandled_count <= 20) {
                        printf("[HLE Reloc WARNING] Tipo de relocacao nao suportado: %u (%s) em 0x%08X (secao %d, entry %d/%d, raw=0x%08X).\n",
                               r_type, mips_reloc_type_name(r_type),
                               target_vaddr, i, r, count, op);
                    } else if (unhandled_count == 21) {
                        printf("[HLE Reloc WARNING] Relocacoes nao suportadas adicionais serao contabilizadas no resumo.\n");
                    }
                }
            }
            free(reloc_ops);
            free(reloc_targets);
            free(reloc_valid);
        }
    }
    
    printf("[HLE Reloc PPSSPP] Segment-based relocations completed! Applied %d relocations in RAM!\n", reloc_count);
    if (unhandled_count > 0 || unmapped_count > 0) {
        printf("[HLE Reloc WARNING] %d relocation(s) com tipo nao suportado e %d com endereco fora do mapa NAO foram aplicadas!\n",
               unhandled_count, unmapped_count);
    }

    // NOVO: imprime o histograma de tipos encontrados. Antes esse array era
    // preenchido mas nunca exibido, entao nao dava pra saber quais tipos de
    // relocacao o binario realmente usa (essencial para diagnosticar buracos
    // de cobertura, tipo GOT16/CALL16/GPREL16).
    printf("[HLE Reloc PPSSPP] Histograma de tipos de relocacao encontrados:\n");
    for (int t = 0; t < 32; t++) {
        if (type_histogram[t] > 0) {
            printf("    type %2d (%-16s): %u ocorrencia(s)\n", t, mips_reloc_type_name(t), type_histogram[t]);
        }
    }

    free(seg_vaddr);
    free(seg_loaded);
    return 0;
}
