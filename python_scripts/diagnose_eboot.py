#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
diagnose_eboot.py - Script de Diagnóstico Avançado para God of War (PSP)
==============================================================================
Este script realiza uma auditoria estática rigorosa de baixo nível diretamente
no arquivo executável EBOOT.BIN descriptografado do jogo. 

Ele foi desenhado para identificar os pontos críticos de falha do linker HLE,
localizar as instruções exatas que causaram o crash de ponteiro nulo (NULL)
e mapear as seções de relocação dinâmica que precisam ser resolvidas na main.
"""

import sys
import struct
import os

# Padrão binário em Little-Endian do fluxo assembly MIPS que causou o crash:
# Trace 75: PC: 0x2027ECD8, Inst: 0x24C60010 -> addiu $a2, $a2, 16  (Bytes: 10 00 c6 24)
# Trace 76: PC: 0x2027ECDC, Inst: 0x84C70000 -> lh $a3, 0($a2)       (Bytes: 00 00 c7 84)
# Trace 77: PC: 0x2027ECE0, Inst: 0x8CC60004 -> lw $a2, 4($a2)       (Bytes: 04 00 c6 8c)
# Trace 78: PC: 0x2027ECE4, Inst: 0x00C0F809 -> jalr $ra, $a2       (Bytes: 09 f8 c0 00)
# Trace 79: PC: 0x2027ECE8, Inst: 0x00872021 -> addu $a0, $a0, $a3   (Bytes: 21 20 87 00)
CRASH_PATTERN = b"\x10\x00\xc6\x24\x00\x00\xc7\x84\x04\x00\xc6\x8c\x09\xf8\xc0\x00\x21\x20\x87\x00"

def decode_mips_instruction(inst_word, pc):
    """Decodificador básico de instruções MIPS para contextualização do crash."""
    op = (inst_word >> 26) & 0x3F
    rs = (inst_word >> 21) & 0x1F
    rt = (inst_word >> 16) & 0x1F
    rd = (inst_word >> 11) & 0x1F
    shamt = (inst_word >> 6) & 0x1F
    funct = inst_word & 0x3F
    imm = inst_word & 0xFFFF
    
    reg_names = [
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"
    ]
    
    if op == 0:  # R-Type
        if funct == 9:
            return f"0x{pc:08X}: jalr ${reg_names[rs]}, ${reg_names[rd]} (Funct: 9)"
        elif funct == 33:
            return f"0x{pc:08X}: addu ${reg_names[rd]}, ${reg_names[rs]}, ${reg_names[rt]}"
        return f"0x{pc:08X}: SPECIAL (op=0, funct={funct})"
    elif op == 9:  # ADDIU
        # Sign extend immediate
        simm = imm - 0x10000 if imm >= 0x8000 else imm
        return f"0x{pc:08X}: addiu ${reg_names[rt]}, ${reg_names[rs]}, {simm}"
    elif op == 33: # LH
        simm = imm - 0x10000 if imm >= 0x8000 else imm
        return f"0x{pc:08X}: lh ${reg_names[rt]}, {simm}(${reg_names[rs]})"
    elif op == 35: # LW
        simm = imm - 0x10000 if imm >= 0x8000 else imm
        return f"0x{pc:08X}: lw ${reg_names[rt]}, {simm}(${reg_names[rs]})"
    
    return f"0x{pc:08X}: Opcode 0x{inst_word:08X} (op={op})"

def parse_and_diagnose(eboot_path):
    print("======================================================================")
    print("      MIPS Allegrex & PRX Relocation static diagnostic tool")
    print("======================================================================")
    
    if not os.path.exists(eboot_path):
        print(f"[ERRO] O arquivo '{eboot_path}' nao foi localizado.")
        print("Certifique-se de que o caminho esta correto.")
        return
        
    with open(eboot_path, "rb") as f:
        data = f.read()
        
    print(f"[Arquivo] Carregado: {eboot_path} ({len(data)} bytes)")
    
    # 1. Detecção do contêiner ~PSP
    is_prx = False
    modinfo_offset_raw = 0
    elf_data = data
    
    if data.startswith(b"~PSP"):
        is_prx = True
        modinfo_offset_raw = struct.unpack("<I", data[52:56])[0]
        print(f"[Contêiner] ~PSP Detectado! Offset SceModuleInfo bruto: 0x{modinfo_offset_raw:08X}")
        
        elf_offset = data.find(b"\x7fELF")
        if elf_offset != -1:
            print(f"  -> ELF interno localizado no offset físico: 0x{elf_offset:X}")
            elf_data = data[elf_offset:]
        else:
            print("  [ERRO] Assinatura ELF não encontrada dentro do contêiner PRX.")
            return

    # 2. Validação do ELF Header
    if not elf_data.startswith(b"\x7fELF"):
        print("[ERRO] Arquivo não possui cabeçalho ELF válido.")
        return
        
    magic, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack("<16sHHIIIIIHHHHHH", elf_data[:52])
    
    load_bias = 0x08800000 if e_entry < 0x08000000 else 0
    real_entry = e_entry + load_bias
    
    print(f"\n[ELF Header]")
    print(f"  Tipo de Arquivo: {e_type} (1=Relocatable, 2=Executable)")
    print(f"  Ponto de Entrada Original: 0x{e_entry:08X} (Relocado: 0x{real_entry:08X})")
    print(f"  Program Headers: Offset 0x{e_phoff:08X} (Contagem: {e_phnum})")
    print(f"  Section Headers: Offset 0x{e_shoff:08X} (Contagem: {e_shnum})")
    
    # 3. Mapeamento de Segmentos de Memória
    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", elf_data[off:off+32])
        segments.append({
            "index": i,
            "type": p_type,
            "offset": p_offset,
            "vaddr": p_vaddr + load_bias,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "flags": p_flags
        })
        
    print(f"\n[Segmentos de Memoria Mapeados (PT_LOAD)]")
    for seg in segments:
        if seg["type"] == 1:
            flags = []
            if seg["flags"] & 1: flags.append("X")
            if seg["flags"] & 2: flags.append("W")
            if seg["flags"] & 4: flags.append("R")
            print(f"  Seg {seg['index']}: VAddr: 0x{seg['vaddr']:08X} - 0x{seg['vaddr']+seg['memsz']:08X} | File Offset: 0x{seg['offset']:08X} | Size: {seg['filesz']} bytes | Flags: {'|'.join(flags)}")

    # Função interna auxiliar para converter VAddr em Offset do Arquivo
    def vaddr_to_file_offset(vaddr):
        for seg in segments:
            if seg["type"] == 1:
                if vaddr >= seg["vaddr"] and vaddr < seg["vaddr"] + seg["memsz"]:
                    rel_offset = vaddr - seg["vaddr"]
                    if rel_offset < seg["filesz"]:
                        return seg["offset"] + rel_offset
        return None

    # 4. Busca heurística estrita pelo padrão exato de instrução que causou o crash
    print(f"\n[Busca Heurística de Baixo Nível]")
    print("  Escaneando os segmentos físicos em busca do padrão de vtable crash...")
    
    found_matches = []
    pattern_len = len(CRASH_PATTERN)
    
    for seg in segments:
        if seg["type"] == 1: # PT_LOAD
            seg_data = elf_data[seg["offset"] : seg["offset"]+seg["filesz"]]
            offset = 0
            while True:
                idx = seg_data.find(CRASH_PATTERN, offset)
                if idx == -1:
                    break
                
                # Match localizado!
                match_file_offset = seg["offset"] + idx
                match_vaddr = seg["vaddr"] + idx
                found_matches.append((match_vaddr, match_file_offset))
                offset = idx + 4 # Avança na busca

    if not found_matches:
        print("  [INFO] Padrão exato de crash de vtable de GoW não foi localizado nos segmentos.")
        print("  Isso indica que o arquivo testado pode não conter as otimizações específicas ou")
        print("  que as instruções variam ligeiramente na compilação do seu dump.")
    else:
        print(f"  [SUCESSO] Localizado(s) {len(found_matches)} ponto(s) crítico(s) de despacho de vtable:")
        for vaddr, f_offset in found_matches:
            print(f"    -> Endereço Virtual Crítico: 0x{vaddr:08X} (Offset no Arquivo: 0x{f_offset:X})")
            print("       Instruções no ponto de falha:")
            for j in range(5):
                inst_addr = vaddr + (j * 4)
                inst_f_offset = f_offset + (j * 4)
                if inst_f_offset + 4 <= len(elf_data):
                    inst_word = struct.unpack("<I", elf_data[inst_f_offset : inst_f_offset+4])[0]
                    print(f"         {decode_mips_instruction(inst_word, inst_addr)}")

    # 5. Auditoria de Relocação Dinâmica (Essencial para evitar desreferenciamento nulo)
    print("\n[Auditoria de Relocação e Integridade]")
    
    # Busca por tabelas de relocação na Section Table (se existir no dump)
    rel_sections_count = 0
    if e_shoff != 0 and e_shnum > 0:
        # Carrega o string table das seções
        shstr_off = e_shoff + e_shstrndx * e_shentsize
        shstr_bytes = elf_data[shstr_off : shstr_off+e_shentsize]
        if len(shstr_bytes) >= 40:
            _, _, _, _, shstr_file_off, _, _, _, _, _ = struct.unpack("<IIIIIIIIII", shstr_bytes[:40])
            
            for s in range(e_shnum):
                s_off = e_shoff + s * e_shentsize
                s_bytes = elf_data[s_off : s_off+e_shentsize]
                if len(s_bytes) < 40: continue
                sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack("<IIIIIIIIII", s_bytes[:40])
                
                # Lê o nome da seção
                sec_name = ""
                name_offset = shstr_file_off + sh_name
                if name_offset < len(elf_data):
                    sec_name_bytes = []
                    while name_offset < len(elf_data):
                        b = elf_data[name_offset]
                        if b == 0: break
                        sec_name_bytes.append(chr(b))
                        name_offset += 1
                    sec_name = "".join(sec_name_bytes)
                
                # SHT_REL = 9, SHT_RELA = 4
                if sh_type in [4, 9] or ".rel" in sec_name:
                    print(f"  -> Seção de Relocação Detectada: '{sec_name}' | Offset: 0x{sh_offset:08X} | Tamanho: {sh_size} bytes")
                    rel_sections_count += 1
                    
        if rel_sections_count == 0:
            print("  [Alerta] Nenhuma seção de relocação estática (.rel/.rela) foi encontrada na Section Table.")
            print("  Isso confirma que o arquivo EBOOT.BIN é uma build limpa de usermode relocada dinamicamente,")
            print("  o que exige que o emulador resolva dinamicamente os ponteiros em tempo de execução")
            print("  através de linkagem HLE correta de stubs ou mapeamento estrito do Global Pointer ($gp)!")
    else:
        print("  [Alerta] Section Table ausente no arquivo executável (comportamento padrão de binários de produção).")
        print("  O emulador deve contar estritamente com as tabelas de SceModuleInfo carregadas na RAM física.")

    print("\n======================================================================")
    print("                 Fim do Diagnóstico Estático")
    print("======================================================================")

if __name__ == "__main__":
    target = "umd0/PSP_GAME/SYSDIR/EBOOT.BIN"
    if len(sys.argv) > 1:
        target = sys.argv[1]
    parse_and_diagnose(target)
