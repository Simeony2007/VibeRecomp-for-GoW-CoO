#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
reloc_analyzer-v3.py - Advanced MIPS ELF Relocation Auditor (Fixed Syntax)
==============================================================================
This tool inspects the section headers AND the PT_DYNAMIC segment of a PSP ELF 
binary. If section headers are stripped (common in optimized games), it extracts
relocation tables via the Dynamic Segment, which is how the PSP OS itself
locates relocations in memory!
==============================================================================
"""

import sys
import struct
import os

def analyze_elf(filepath):
    if not os.path.exists(filepath):
        print(f"Error: File '{filepath}' not found.")
        return

    with open(filepath, "rb") as f:
        data = f.read()

    print("==============================================================================")
    print("             RELOC_ANALYZER - PSP GOD OF WAR INTEGRITY TOOL (V3)")
    print("==============================================================================")
    print(f"File Analisado: {filepath}")
    print(f"Tamanho do Arquivo: {len(data)} bytes")

    # Detect ~PSP header (PRX Container)
    elf_offset = 0
    if data.startswith(b"~PSP"):
        print("[Container] Container ~PSP (PRX) detectado.")
        elf_idx = data.find(b"\\x7fELF")
        if elf_idx == -1:
            # Try plain text scan
            elf_idx = data.find(b"\x7fELF")
        if elf_idx != -1:
            elf_offset = elf_idx
            print(f"  -> ELF embutido encontrado no offset do arquivo: 0x{elf_offset:X}")
        else:
            print("  [Erro] Assinatura ELF não encontrada dentro do PRX.")
            return

    elf_data = data[elf_offset:]
    if len(elf_data) < 52 or not elf_data.startswith(b"\x7fELF"):
        print("[Erro] Assinatura ELF inválida ou corrompida.")
        return

    # Unpack ELF Header
    (magic, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, 
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack("<16sHHIIIIIHHHHHH", elf_data[:52])

    print(f"\n[ELF Header]")
    print(f"  Ponto de Entrada original (e_entry): 0x{e_entry:08X}")
    print(f"  Número de Program Headers (Segmentos): {e_phnum}")
    print(f"  Número de Section Headers (Seções): {e_shnum}")
    print(f"  Offset da Tabela de Seções (e_shoff): 0x{e_shoff:08X}")

    load_bias = 0x08800000 if e_entry < 0x04000000 else 0
    print(f"  PSP Load Bias Estimado: 0x{load_bias:08X}")

    # Load Program Headers
    segments = []
    dynamic_segment = None
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        phdr_bytes = elf_data[off : off + e_phentsize]
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", phdr_bytes[:32])
        segments.append({
            "type": p_type,
            "offset": p_offset,
            "vaddr": p_vaddr,
            "paddr": p_paddr,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "flags": p_flags
        })
        if p_type == 1: # PT_LOAD
            print(f"  -> Segmento PT_LOAD {i}: Offset 0x{p_offset:08X} | VAddr 0x{p_vaddr:08X} | Filesz {p_filesz} bytes")
        elif p_type == 2: # PT_DYNAMIC
            dynamic_segment = segments[-1]
            print(f"  -> Segmento PT_DYNAMIC encontrado: Offset 0x{p_offset:08X} | VAddr 0x{p_vaddr:08X} | Filesz {p_filesz} bytes")

    # Memory address mapper
    def get_file_offset(vaddr):
        addr = vaddr & 0x0FFFFFFF
        for seg in segments:
            if seg["type"] == 1: # PT_LOAD
                start = seg["vaddr"] & 0x0FFFFFFF
                end = start + seg["memsz"]
                if addr >= start and addr < end:
                    return seg["offset"] + (addr - start)
        return None

    # Step 1: Analyze Section Headers (Standard relocation tables)
    rel_sections_found = 0
    if e_shnum > 0 and e_shoff != 0:
        print("\n[Section Headers Relocation Scan]")
        # Read section string table
        shstr_off = e_shoff + e_shstrndx * e_shentsize
        shstr_bytes = elf_data[shstr_off : shstr_off + 40]
        _, _, _, _, s_offset, s_size, _, _, _, _ = struct.unpack("<IIIIIIIIII", shstr_bytes)
        shstr_tab = elf_data[s_offset : s_offset + s_size]

        for s in range(e_shnum):
            sect_off = e_shoff + s * e_shentsize
            sect_bytes = elf_data[sect_off : sect_off + 40]
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack("<IIIIIIIIII", sect_bytes)
            
            # Extract section name
            name = ""
            if sh_name < len(shstr_tab):
                chars = []
                idx = sh_name
                while idx < len(shstr_tab) and shstr_tab[idx] != 0:
                    chars.append(chr(shstr_tab[idx]))
                    idx += 1
                name = "".join(chars)

            if sh_type == 9: # SHT_REL
                rel_sections_found += 1
                num_entries = sh_size // sh_entsize if sh_entsize > 0 else sh_size // 8
                print(f"  -> Seção REL '{name}' (ID: {s}): Offset 0x{sh_offset:08X} | Tamanho: {sh_size} bytes | {num_entries} Entradas")
                
                # Sample first relocation types
                if num_entries > 0:
                    sample_size = min(5, num_entries)
                    print("     Amostra de Relocações:")
                    for j in range(sample_size):
                        entry_off = sh_offset + j * 8
                        r_offset, r_info = struct.unpack("<II", elf_data[entry_off : entry_off + 8])
                        r_type = r_info & 0xFF
                        print(f"       [{j:02d}] Target VAddr: 0x{r_offset:08X} | Tipo de Reloc: {r_type} (R_MIPS_{'32' if r_type==2 else '26' if r_type==4 else str(r_type)})")

    # Step 2: Analyze Dynamic Segment (PT_DYNAMIC) - The PSP OS Loader Way!
    dt_rel = 0
    dt_relsz = 0
    dt_relent = 8 # default Elf32_Rel size is 8 bytes

    if dynamic_segment:
        print("\n[PT_DYNAMIC Segment Relocation Scan - The PSP OS Method]")
        dyn_offset = dynamic_segment["offset"]
        dyn_size = dynamic_segment["filesz"]
        
        # Parse dynamic tags (each tag is 8 bytes: 32-bit tag type, 32-bit value)
        idx = 0
        while idx < dyn_size:
            tag_off = dyn_offset + idx
            if tag_off + 8 > len(elf_data):
                break
            d_tag, d_val = struct.unpack("<II", elf_data[tag_off : tag_off + 8])
            if d_tag == 0: # DT_NULL
                break
            
            if d_tag == 17: # DT_REL
                dt_rel = d_val
            elif d_tag == 18: # DT_RELSZ
                dt_relsz = d_val
            elif d_tag == 19: # DT_RELENT
                dt_relent = d_val
            idx += 8

        if dt_rel != 0 and dt_relsz > 0:
            print(f"  -> Tabela de Relocação Dinâmica (DT_REL) encontrada em VAddr: 0x{dt_rel:08X}")
            print(f"  -> Tamanho total (DT_RELSZ): {dt_relsz} bytes")
            print(f"  -> Tamanho de cada entrada (DT_RELENT): {dt_relent} bytes")
            
            # Map dynamic VAddr to file offset
            dt_file_off = get_file_offset(dt_rel)
            if dt_file_off:
                num_dyn_relocs = dt_relsz // dt_relent
                print(f"  -> Total de Entradas de Relocação Dinâmica: {num_dyn_relocs}")
                print(f"  -> File Offset mapeado: 0x{dt_file_off:08X}")
                
                # Sample first dynamic relocations
                if num_dyn_relocs > 0:
                    sample_size = min(5, num_dyn_relocs)
                    print("     Amostra de Relocações Dinâmicas:")
                    for j in range(sample_size):
                        entry_off = dt_file_off + j * dt_relent
                        r_offset, r_info = struct.unpack("<II", elf_data[entry_off : entry_off + 8])
                        r_type = r_info & 0xFF
                        print(f"       [{j:02d}] Target VAddr: 0x{r_offset:08X} | Tipo de Reloc: {r_type} (R_MIPS_{'32' if r_type==2 else '26' if r_type==4 else str(r_type)})")
            else:
                print("  [Aviso] Não foi possível mapear o endereço virtual DT_REL para um offset físico de arquivo.")
        else:
            print("  -> Nenhuma relocação dinâmica (DT_REL) declarada no PT_DYNAMIC.")

    if rel_sections_found == 0 and dt_rel == 0:
        print("\n[ALERTA CRÍTICO]")
        print("Este executável NÃO possui tabelas de relocação em Section Headers nem em PT_DYNAMIC.")
        print("Isso indica que o arquivo foi transformado em um Absolute ELF estático (sem relocabilidade) ou")
        print("está em um formato modificado criptografado.")
    else:
        print("\n==============================================================================")
        print("[SUCESSO] Auditoria de Relocação concluída!")
        print("Use os resultados acima para verificar se as relocações estão em seções ou no PT_DYNAMIC.")
        print("==============================================================================")

if __name__ == "__main__":
    target = "umd0/PSP_GAME/SYSDIR/EBOOT.BIN"
    if len(sys.argv) > 1:
        target = sys.argv[1]
    analyze_elf(target)
