#!/usr/bin/env python3
"""
parse_elf.py - Analisador avançado de ELF/PRX para PSP com extração de NIDs para HLE.
"""

import struct
import sys
import os
import json

# ─── Constantes ELF/PSP ───────────────────────────────────────────────────────
ELF_MAGIC    = b'\x7fELF'
ET_SCE_PRX   = 0xFFA0
EM_MIPS      = 8
PT_LOAD      = 1
SHT_REL      = 9
PSP_LOAD_BASE = 0x08800000

# Tipos de relocação MIPS
R_MIPS_32   = 2
R_MIPS_26   = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6

def r16(data, off): return struct.unpack_from("<H", data, off)[0]
def r32(data, off): return struct.unpack_from("<I", data, off)[0]
def w32(data, off, val): struct.pack_into("<I", data, off, val & 0xFFFFFFFF)

class ELFSection:
    def __init__(self, name, sh_type, addr, offset, size, link, info, data):
        self.name, self.type, self.addr = name, sh_type, addr
        self.offset, self.size, self.link = offset, size, link
        self.info, self.data = info, data

class ELFSegment:
    def __init__(self, p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags, data):
        self.p_type, self.p_offset, self.p_vaddr = p_type, p_offset, p_vaddr
        self.p_filesz, self.p_memsz, self.p_flags = p_filesz, p_memsz, p_flags
        self.data = bytearray(data)
    def is_executable(self): return bool(self.p_flags & 0x1)

class PSPElf:
    def __init__(self, path, load_base=PSP_LOAD_BASE):
        self.path, self.load_base = path, load_base
        self.sections, self.segments = [], []
        self.entry_raw, self.entry, self.e_type = 0, 0, 0
        self.hle_imports = [] # Tabela: {vaddr, nid, module}
        
        self._parse()
        if self.e_type == ET_SCE_PRX:
            self._apply_relocations()
            self._extract_hle_data() # Nova função para automatizar Syscalls
        else:
            self._relocated_image = bytearray(self._raw)

        self.entry = self.entry_raw + (self.load_base if self.e_type == ET_SCE_PRX else 0)

    def _parse(self):
        with open(self.path, "rb") as f: self._raw = f.read()
        raw = self._raw
        if raw[:4] != ELF_MAGIC: raise ValueError("EBOOT.BIN inválido ou criptografado.")
        
        self.e_type = r16(raw, 0x10)
        self.entry_raw = r32(raw, 0x18)
        e_phoff, e_shoff = r32(raw, 0x1C), r32(raw, 0x20)
        e_phnum, e_phentsize = r16(raw, 0x2C), r16(raw, 0x2A)
        e_shnum, e_shentsize, e_shstrndx = r16(raw, 0x30), r16(raw, 0x2E), r16(raw, 0x32)

        # Parse Segmentos
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, p_offset, p_vaddr = r32(raw, off), r32(raw, off+4), r32(raw, off+8)
            p_filesz, p_memsz, p_flags = r32(raw, off+16), r32(raw, off+20), r32(raw, off+24)
            self.segments.append(ELFSegment(p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags, raw[p_offset:p_offset+p_filesz]))

        # Parse Seções
        strtab_off = r32(raw, e_shoff + e_shstrndx * e_shentsize + 0x10)
        strtab_sz = r32(raw, e_shoff + e_shstrndx * e_shentsize + 0x14)
        strtab = raw[strtab_off:strtab_off+strtab_sz]

        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            sh_name_idx = r32(raw, off)
            name = strtab[sh_name_idx:].split(b'\x00')[0].decode("ascii", errors="replace")
            sh_type, sh_addr, sh_f_off, sh_size = r32(raw, off+4), r32(raw, off+12), r32(raw, off+16), r32(raw, off+20)
            sh_link, sh_info = r32(raw, off+24), r32(raw, off+28)
            self.sections.append(ELFSection(name, sh_type, sh_addr, sh_f_off, sh_size, sh_link, sh_info, raw[sh_f_off:sh_f_off+sh_size]))

    def _extract_hle_data(self):
        """ 
        Modo Brute-Force: Varre todo o código executável caçando instruções 'syscall'.
        Ignora NIDs e tabelas oficiais (ideal para jogos ofuscados como GOW).
        """
        print("[parse_elf] Modo Brute-Force ativado: Caçando syscalls no código...")
        syscall_codes = {}
        syscall_count = 0

        for seg in self.segments:
            if seg.is_executable():
                data = seg.data
                # Lê de 4 em 4 bytes (tamanho de uma instrução MIPS)
                for i in range(0, len(data) - 3, 4):
                    inst = struct.unpack("<I", data[i:i+4])[0]
                    
                    # Máscara MIPS exata: Opcode == 0 (primeiros 6 bits) e Funct == 0x0C (últimos 6 bits)
                    if (inst & 0xFC00003F) == 0x0000000C:
                        # Extrai os 20 bits de código do meio da instrução
                        code = (inst >> 6) & 0xFFFFF
                        
                        if code not in syscall_codes:
                            syscall_codes[code] = 0
                        syscall_codes[code] += 1
                        syscall_count += 1

        # Ordena das mais chamadas para as menos chamadas
        sorted_syscalls = sorted(syscall_codes.items(), key=lambda x: x[1], reverse=True)

        self.hle_imports = []
        for code, freq in sorted_syscalls:
            self.hle_imports.append({
                "syscall_code": f"0x{code:05X}",
                "frequency": freq,
                "nid": "0x????????", # Não sabemos o nome, mas temos o código!
                "module": "GOW_STATIC"
            })
            
        print(f"[parse_elf] ✅ {syscall_count} instruções SYSCALL encontradas ({len(syscall_codes)} códigos únicos)")

    def _get_string_at(self, vaddr):
        off = self._addr_to_offset(vaddr)
        return self._raw[off:].split(b'\x00')[0].decode("ascii", errors="replace")

    def _addr_to_offset(self, vaddr):
        for seg in self.segments:
            if seg.p_vaddr <= vaddr < seg.p_vaddr + seg.p_memsz:
                return seg.p_offset + (vaddr - seg.p_vaddr)
        return 0

    def _apply_relocations(self):
        base, image = self.load_base, bytearray(self._raw)
        rel_count, hi16_pending = 0, []
        for sec in self.sections:
            if sec.type != SHT_REL: continue
            data = sec.data
            for i in range(len(data) // 8):
                off, info = r32(data, i*8), r32(data, i*8+4)
                r_type = info & 0xFF
                if off + 4 > len(image): continue
                orig = r32(image, off)
                if r_type == R_MIPS_32: w32(image, off, orig + base); rel_count += 1
                elif r_type == R_MIPS_26:
                    target = (orig & 0x03FFFFFF) + (base >> 2)
                    w32(image, off, (orig & 0xFC000000) | (target & 0x03FFFFFF)); rel_count += 1
                elif r_type == R_MIPS_HI16: hi16_pending.append((off, orig))
                elif r_type == R_MIPS_LO16:
                    lo_s = (orig & 0xFFFF) if (orig & 0x8000) == 0 else (orig & 0xFFFF) - 0x10000
                    for h_off, h_orig in hi16_pending:
                        full = ((h_orig & 0xFFFF) << 16) + lo_s + base
                        w32(image, h_off, (h_orig & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF)); rel_count += 1
                    w32(image, off, (orig & 0xFFFF0000) | ((lo_s + base) & 0xFFFF)); hi16_pending.clear(); rel_count += 1
        self._relocated_image = image
        print(f"[parse_elf] {rel_count} relocações aplicadas.")

    def export_json(self, out_path):
        meta = {
            "entry"         : self.entry,
            "load_base"     : self.load_base,
            "code_segments" : [{"vaddr": s.p_vaddr + self.load_base, "offset": s.p_offset, "size": s.p_filesz} for s in self.segments if s.is_executable()],
            "hle_imports"   : self.hle_imports # A chave de ouro para os syscalls
        }
        with open(out_path, "w") as f: json.dump(meta, f, indent=2)
        with open(out_path.replace(".json", "_image.bin"), "wb") as f: f.write(self._relocated_image)
        print(f"[parse_elf] Metadados e Imagem exportados.")

if __name__ == "__main__":
    if len(sys.argv) < 2: sys.exit(1)
    elf = PSPElf(sys.argv[1])
    elf.export_json("elf_meta.json")