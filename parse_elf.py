#!/usr/bin/env python3
"""
parse_elf.py - Lê o header ELF/PRX do EBOOT.BIN descriptado do PSP.
Suporta PRX relocável (ET_SCE_PRX) aplicando endereço base e relocações.

Uso: python3 parse_elf.py EBOOT.BIN
"""

import struct
import sys
import os
import json

# ─── Constantes ELF ───────────────────────────────────────────────────────────
ELF_MAGIC    = b'\x7fELF'
ET_EXEC      = 2
ET_SCE_EXEC  = 0xFE00
ET_SCE_PRX   = 0xFFA0
EM_MIPS      = 8
PT_LOAD      = 1
SHT_REL      = 9

# Endereço base padrão do PSP para módulos de usuário
PSP_LOAD_BASE = 0x08800000

# Tipos de relocação MIPS
R_MIPS_NONE = 0
R_MIPS_16   = 1
R_MIPS_32   = 2
R_MIPS_26   = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6


def r16(data, off):
    return struct.unpack_from("<H", data, off)[0]

def r32(data, off):
    return struct.unpack_from("<I", data, off)[0]

def w32(data, off, val):
    struct.pack_into("<I", data, off, val & 0xFFFFFFFF)


class ELFSection:
    def __init__(self, name, sh_type, addr, offset, size, link, info, data):
        self.name   = name
        self.type   = sh_type
        self.addr   = addr
        self.offset = offset
        self.size   = size
        self.link   = link
        self.info   = info
        self.data   = data

    def __repr__(self):
        return (f"ELFSection(name={self.name!r}, addr=0x{self.addr:08X}, "
                f"size=0x{self.size:X})")


class ELFSegment:
    def __init__(self, p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags, data):
        self.p_type   = p_type
        self.p_offset = p_offset
        self.p_vaddr  = p_vaddr
        self.p_filesz = p_filesz
        self.p_memsz  = p_memsz
        self.p_flags  = p_flags
        self.data     = bytearray(data)

    def is_executable(self):
        return bool(self.p_flags & 0x1)

    def __repr__(self):
        flags = ""
        if self.p_flags & 0x4: flags += "R"
        if self.p_flags & 0x2: flags += "W"
        if self.p_flags & 0x1: flags += "X"
        return (f"ELFSegment(vaddr=0x{self.p_vaddr:08X}, "
                f"memsz=0x{self.p_memsz:X}, flags={flags})")


class PSPElf:
    def __init__(self, path, load_base=PSP_LOAD_BASE):
        self.path      = path
        self.load_base = load_base
        self.sections  = []
        self.segments  = []
        self.entry_raw = 0
        self.entry     = 0
        self.e_type    = 0
        self._raw      = None
        self._relocated_image = None

        self._parse()

        if self.e_type == ET_SCE_PRX:
            print(f"[parse_elf] PRX relocável detectado. Base: 0x{load_base:08X}")
            self._apply_relocations()
        else:
            self._relocated_image = bytearray(self._raw)

        self.entry = self.entry_raw + (self.load_base if self.e_type == ET_SCE_PRX else 0)
        print(f"[parse_elf] Entry point final: 0x{self.entry:08X}")

    def _parse(self):
        with open(self.path, "rb") as f:
            self._raw = f.read()
        raw = self._raw

        if raw[:4] != ELF_MAGIC:
            raise ValueError(
                f"Magic ELF inválido: {raw[:4].hex()}\n"
                "O EBOOT.BIN precisa estar descriptado. Use o PRXDecrypter."
            )
        if raw[4] != 1:
            raise ValueError("Só ELF 32-bit suportado.")
        if raw[5] != 1:
            raise ValueError("Só little-endian suportado.")

        self.e_type    = r16(raw, 0x10)
        e_machine      = r16(raw, 0x12)
        self.entry_raw = r32(raw, 0x18)
        e_phoff        = r32(raw, 0x1C)
        e_shoff        = r32(raw, 0x20)
        e_phentsize    = r16(raw, 0x2A)
        e_phnum        = r16(raw, 0x2C)
        e_shentsize    = r16(raw, 0x2E)
        e_shnum        = r16(raw, 0x30)
        e_shstrndx     = r16(raw, 0x32)

        if e_machine != EM_MIPS:
            raise ValueError(f"Não é MIPS: {e_machine:#x}")

        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type   = r32(raw, off + 0x00)
            p_offset = r32(raw, off + 0x04)
            p_vaddr  = r32(raw, off + 0x08)
            p_filesz = r32(raw, off + 0x10)
            p_memsz  = r32(raw, off + 0x14)
            p_flags  = r32(raw, off + 0x18)
            self.segments.append(
                ELFSegment(p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags,
                           raw[p_offset : p_offset + p_filesz])
            )

        strtab = b""
        if e_shoff and e_shnum and e_shstrndx < e_shnum:
            st  = e_shoff + e_shstrndx * e_shentsize
            sto = r32(raw, st + 0x10)
            sts = r32(raw, st + 0x14)
            strtab = raw[sto : sto + sts]

        for i in range(e_shnum):
            off     = e_shoff + i * e_shentsize
            sh_name = r32(raw, off + 0x00)
            sh_type = r32(raw, off + 0x04)
            sh_addr = r32(raw, off + 0x0C)
            sh_off  = r32(raw, off + 0x10)
            sh_size = r32(raw, off + 0x14)
            sh_link = r32(raw, off + 0x18)
            sh_info = r32(raw, off + 0x1C)
            name = ""
            if strtab and sh_name < len(strtab):
                end  = strtab.index(b'\x00', sh_name)
                name = strtab[sh_name:end].decode("ascii", errors="replace")
            data = raw[sh_off : sh_off + sh_size] if sh_size else b""
            self.sections.append(
                ELFSection(name, sh_type, sh_addr, sh_off, sh_size, sh_link, sh_info, data)
            )

    def _apply_relocations(self):
        base  = self.load_base
        image = bytearray(self._raw)
        rel_count    = 0
        hi16_pending = []

        for sec in self.sections:
            if sec.type != SHT_REL or not sec.name.startswith(".rel"):
                continue
            data = sec.data
            for i in range(len(data) // 8):
                r_offset = r32(data, i*8)
                r_info   = r32(data, i*8 + 4)
                r_type   = r_info & 0xFF

                if r_offset + 4 > len(image):
                    continue
                original = r32(image, r_offset)

                if r_type == R_MIPS_NONE:
                    pass

                elif r_type == R_MIPS_32:
                    w32(image, r_offset, original + base)
                    rel_count += 1

                elif r_type == R_MIPS_26:
                    target = (original & 0x03FFFFFF) + (base >> 2)
                    w32(image, r_offset, (original & 0xFC000000) | (target & 0x03FFFFFF))
                    rel_count += 1

                elif r_type == R_MIPS_HI16:
                    hi16_pending.append((r_offset, original))

                elif r_type == R_MIPS_LO16:
                    lo   = original & 0xFFFF
                    lo_s = lo if lo < 0x8000 else lo - 0x10000
                    for (hi_off, hi_orig) in hi16_pending:
                        hi_val  = (hi_orig & 0xFFFF) << 16
                        full    = hi_val + lo_s + base
                        new_hi  = (full + 0x8000) >> 16
                        w32(image, hi_off, (hi_orig & 0xFFFF0000) | (new_hi & 0xFFFF))
                        rel_count += 1
                    new_lo = (lo_s + base) & 0xFFFF
                    w32(image, r_offset, (original & 0xFFFF0000) | new_lo)
                    hi16_pending.clear()
                    rel_count += 1

                elif r_type == R_MIPS_16:
                    val = (original & 0xFFFF) + (base & 0xFFFF)
                    w32(image, r_offset, (original & 0xFFFF0000) | (val & 0xFFFF))
                    rel_count += 1

        self._relocated_image = image
        print(f"[parse_elf] {rel_count} relocações aplicadas.")

        # Atualiza segmentos com dados relocados e vaddr correto
        for seg in self.segments:
            if seg.p_type == PT_LOAD:
                seg.data    = bytearray(image[seg.p_offset : seg.p_offset + seg.p_filesz])
                seg.p_vaddr += base

    def code_segments(self):
        return [s for s in self.segments if s.p_type == PT_LOAD and s.is_executable()]

    def data_segments(self):
        return [s for s in self.segments if s.p_type == PT_LOAD and not s.is_executable()]

    def summary(self):
        etype_str = {
            ET_EXEC:     "ET_EXEC (executável absoluto)",
            ET_SCE_EXEC: "ET_SCE_EXEC (PRX executável Sony)",
            ET_SCE_PRX:  "ET_SCE_PRX (PRX relocável Sony)",
        }.get(self.e_type, f"desconhecido (0x{self.e_type:04X})")

        print("=" * 60)
        print(f"  Arquivo   : {self.path}")
        print(f"  Tipo      : {etype_str}")
        print(f"  Base      : 0x{self.load_base:08X}")
        print(f"  Entry raw : 0x{self.entry_raw:08X}")
        print(f"  Entry real: 0x{self.entry:08X}  <- use este no loader.c")
        print(f"  Segmentos (apos relocacao):")
        for s in self.segments:
            if s.p_type == PT_LOAD:
                print(f"    {s}")
        print(f"  Modulos de syscall detectados:")
        for sec in self.sections:
            if sec.name.startswith(".sceStub.text."):
                mod = sec.name[len(".sceStub.text."):]
                print(f"    {mod}  ({sec.size // 8} funcoes importadas)")
        print("=" * 60)

    def export_json(self, out_path):
        code_segs = [{"vaddr": s.p_vaddr, "offset": s.p_offset,
                      "size": s.p_filesz, "memsz": s.p_memsz}
                     for s in self.code_segments()]
        data_segs = [{"vaddr": s.p_vaddr, "offset": s.p_offset,
                      "size": s.p_filesz, "memsz": s.p_memsz}
                     for s in self.data_segments()]
        stubs = [{"module": sec.name[len(".sceStub.text."):],
                  "vaddr":  sec.addr + (self.load_base if self.e_type == ET_SCE_PRX else 0),
                  "size":   sec.size}
                 for sec in self.sections if sec.name.startswith(".sceStub.text.")]

        meta = {
            "entry"         : self.entry,
            "load_base"     : self.load_base,
            "e_type"        : self.e_type,
            "is_prx"        : self.e_type == ET_SCE_PRX,
            "code_segments" : code_segs,
            "data_segments" : data_segs,
            "syscall_stubs" : stubs,
        }
        with open(out_path, "w") as f:
            json.dump(meta, f, indent=2)
        print(f"[parse_elf] JSON exportado: {out_path}")

        # Salva imagem com relocacoes ja aplicadas (usada pelo disasm_to_c.py)
        img_path = out_path.replace(".json", "_image.bin")
        with open(img_path, "wb") as f:
            f.write(self._relocated_image)
        print(f"[parse_elf] Imagem relocada salva: {img_path}  ({len(self._relocated_image):,} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Uso: python3 parse_elf.py <EBOOT.BIN> [base_hex]")
        print("  base_hex: endereco base opcional (padrao: 0x08800000)")
        sys.exit(1)
    base = PSP_LOAD_BASE
    if len(sys.argv) >= 3:
        base = int(sys.argv[2], 16)
    elf = PSPElf(sys.argv[1], load_base=base)
    elf.summary()
    elf.export_json("elf_meta.json")
