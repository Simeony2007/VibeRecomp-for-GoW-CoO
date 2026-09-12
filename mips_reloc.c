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
            return true;
        }
    }
    out[out_size - 1] = 0;
    return true;
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

int mips_apply_relocations(MIPS_CPU *cpu, uint8_t *elf_data, uint32_t load_bias, uint32_t *entry_point) {
    (void)entry_point;
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
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

    uint32_t seg_vaddr[16] = {0};
    int max_segs = ehdr->e_phnum < 16 ? ehdr->e_phnum : 16;
    for (int p = 0; p < max_segs; p++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + p * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD
            seg_vaddr[p] = phdr->p_vaddr + load_bias;
        }
    }

    for (int i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *sh = &sh_table[i];
        if (sh->sh_type == 9 || sh->sh_type == 0x700000A0) { // SHT_REL / SHT_PRXRELOC
            int count = sh->sh_size / 8;
            Elf32_Rel *rels = (Elf32_Rel *)(elf_data + sh->sh_offset);

            // NOVO: fila de R_MIPS_HI16 pendentes (algoritmo real do ABI MIPS,
            // usado pelo PPSSPP). Cada HI16 so e' aplicado quando aparece o
            // PROXIMO R_MIPS_LO16 na tabela - nao precisa ser adjacente, pode
            // ter qualquer outra relocacao (R_MIPS_26 de um jal, R_MIPS_32 de
            // um dado, etc) no meio, que e exatamente o padrao que gerava os
            // 667 warnings de "LO16 IMEDIATO nao encontrado" no log do GoW.
            // Resetada a cada secao, porque HI16/LO16 nunca cruzam secoes.
            #define MAX_PENDING_HI16 256
            typedef struct {
                uint32_t *ptr;
                uint32_t hi_inst;
                uint32_t relocate_to;
                uint32_t target_vaddr; // so pra log de diagnostico
            } PendingHi16;
            PendingHi16 pending_hi16[MAX_PENDING_HI16];
            int pending_hi16_count = 0;

            for (int r = 0; r < count; r++) {
                uint32_t r_offset = rels[r].r_offset;
                uint32_t r_info = rels[r].r_info;
                // FIX: o tipo ocupa o byte inteiro (bits 0-7), nao so os 4 bits baixos.
                // Com 0x0F qualquer tipo >= 16 seria mascarado incorretamente.
                uint32_t r_type = r_info & 0xFF;
                uint32_t readwrite_idx = (r_info >> 8) & 0xFF;
                uint32_t relative_idx = (r_info >> 16) & 0xFF;

                uint32_t patch_base = (readwrite_idx < (uint32_t)max_segs) ? seg_vaddr[readwrite_idx] : load_bias;
                uint32_t target_vaddr = r_offset + patch_base;
                uint32_t relocateTo = (relative_idx < (uint32_t)max_segs) ? seg_vaddr[relative_idx] : load_bias;

                if (r_type < 32) type_histogram[r_type]++;

                uint32_t *ptr = (uint32_t *)mips_get_ptr(cpu, target_vaddr);
                if (ptr) {
                    if (r_type == 2) { // R_MIPS_32
                        *ptr += relocateTo;
                        reloc_count++;
                    } else if (r_type == 3) { // R_MIPS_REL32
                        // FIX: R_MIPS_REL32 NAO e o mesmo que R_MIPS_26. Antes estava
                        // sendo tratado como salto de 26 bits, corrompendo palavras de
                        // dados/eh_frame que usam esse tipo. Semanticamente equivale a
                        // somar o deslocamento de base igual ao R_MIPS_32.
                        *ptr += relocateTo;
                        reloc_count++;
                    } else if (r_type == 4) { // R_MIPS_26
                        uint32_t inst = *ptr;
                        uint32_t target = (inst & 0x03FFFFFF) + (relocateTo >> 2);
                        *ptr = (inst & 0xFC000000) | (target & 0x03FFFFFF);
                        reloc_count++;
                    } else if (r_type == 5) { // R_MIPS_HI16
                        uint32_t hi_inst = *ptr;
                        // NOVO: nao tenta mais casar aqui. So enfileira - o
                        // patch de verdade acontece quando o proximo LO16
                        // (de qualquer simbolo/instrucao) aparecer na tabela,
                        // conforme o algoritmo padrao HI16/LO16 do MIPS ABI.
                        if (pending_hi16_count < MAX_PENDING_HI16) {
                            pending_hi16[pending_hi16_count].ptr = ptr;
                            pending_hi16[pending_hi16_count].hi_inst = hi_inst;
                            pending_hi16[pending_hi16_count].relocate_to = relocateTo;
                            pending_hi16[pending_hi16_count].target_vaddr = target_vaddr;
                            pending_hi16_count++;
                        } else if (unhandled_count < 20) {
                            printf("[HLE Reloc WARNING] Fila de R_MIPS_HI16 pendentes cheia (>%d) em 0x%08X (secao %d, entry %d) - relocacao descartada.\n",
                                   MAX_PENDING_HI16, target_vaddr, i, r);
                        }
                    } else if (r_type == 6) { // R_MIPS_LO16
                        uint32_t lo_inst = *ptr;
                        int32_t full_val = (int32_t)((lo_inst & 0xFFFF) + relocateTo);
                        *ptr = (lo_inst & 0xFFFF0000) | (full_val & 0xFFFF);
                        reloc_count++;

                        // NOVO: drena a fila de HI16 pendentes usando a parte
                        // baixa (16 bits) DESTA instrucao LO16. Isso e' o que
                        // realmente fecha o par, independente de quantas
                        // outras relocacoes (nao-HI16/LO16) apareceram entre
                        // o lui e o addiu/lw na tabela.
                        int16_t lo_part = (int16_t)(lo_inst & 0xFFFF);
                        if (pending_hi16_count > 1 && unhandled_count < 20) {
                            printf("[HLE Reloc DEBUG] LO16 em 0x%08X fechando %d R_MIPS_HI16 pendente(s) de uma vez (secao %d, entry %d) - confira se faz sentido pra esse trecho de codigo.\n",
                                   target_vaddr, pending_hi16_count, i, r);
                        }
                        for (int q = 0; q < pending_hi16_count; q++) {
                            uint32_t hi_inst_q = pending_hi16[q].hi_inst;
                            int32_t full_val_q = (int32_t)(((hi_inst_q & 0xFFFF) << 16) + lo_part + pending_hi16[q].relocate_to);
                            uint16_t new_hi = (uint16_t)((full_val_q - (int16_t)(full_val_q & 0xFFFF)) >> 16);
                            *(pending_hi16[q].ptr) = (hi_inst_q & 0xFFFF0000) | new_hi;
                            reloc_count++;
                        }
                        pending_hi16_count = 0;
                    } else {
                        // Tipo de relocacao nao implementado (ex: R_MIPS_GPREL16,
                        // R_MIPS_GOT16, R_MIPS_CALL16, R_MIPS_GPREL32...).
                        // A palavra fica exatamente como estava no arquivo - se ela
                        // deveria conter um ponteiro de funcao/dado, ele fica 0 ou com
                        // o valor de link-time errado, o que costuma se manifestar como
                        // "entry_pc == load_bias" (ponteiro nulo relocado por acidente)
                        // quando esse endereco depois e usado como funcao/thread entry.
                        unhandled_count++;
                        if (unhandled_count <= 20) {
                            printf("[HLE Reloc WARNING] Tipo de relocacao NAO IMPLEMENTADO: %u (%s) em vaddr 0x%08X (secao %d, entry %d/%d) - palavra deixada sem modificacao (raw=0x%08X).\n",
                                   r_type, mips_reloc_type_name(r_type), target_vaddr, i, r, count, *ptr);
                        } else if (unhandled_count == 21) {
                            printf("[HLE Reloc WARNING] ... mais relocacoes de tipo nao implementado serao contadas silenciosamente (veja o histograma no final).\n");
                        }
                    }
                } else {
                    unmapped_count++;
                    if (unmapped_count <= 20) {
                        printf("[HLE Reloc WARNING] target_vaddr 0x%08X fora da memoria mapeada para relocacao tipo %u (%s) (secao %d, entry %d).\n",
                               target_vaddr, r_type, mips_reloc_type_name(r_type), i, r);
                    }
                }

            }
            // NOVO: qualquer HI16 que sobrou na fila ao final da secao nunca
            // achou NENHUM R_MIPS_LO16 depois dele - diferente do warning
            // antigo (que so cobria "nao achou o imediato"), este so dispara
            // se realmente não existir NENHUM LO16 restante pra fechar o par.
            if (pending_hi16_count > 0) {
                printf("[HLE Reloc WARNING] %d R_MIPS_HI16 pendente(s) na secao %d nunca encontraram NENHUM R_MIPS_LO16 - resultado incorreto (ficam com valor pre-relocacao).\n",
                    pending_hi16_count, i);
            }
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

    return 0;
}
