#!/usr/bin/env python3
"""
parse_elf.py - Analisador avançado de ELF/PRX para PSP
Extração 100% nativa de NIDs, Stubs, Globais e Construtores (.ctors).
"""

import struct
import sys
import json

# ─── Constantes ELF/PSP ───────────────────────────────────────────────────────
ELF_MAGIC    = b'\x7fELF'
ET_SCE_PRX   = 0xFFA0
PT_PRXINFO   = 0x70000001
SHT_REL      = 9
PSP_LOAD_BASE = 0x08800000

# Tipos de relocação MIPS
R_MIPS_32   = 2
R_MIPS_26   = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6

KNOWN_NIDS = {
    0x446D8DE6: "sceKernelCreateThread",
    0xF475845D: "sceKernelStartThread",
    0x106D5659: "sceIoOpen",
    0x810C4CE3: "sceIoClose",
    0x6A638D83: "sceIoRead",
    0x42EC0328: "sceIoWrite",
    0x977DE386: "sceKernelLoadModule",
    0x50F0C1EC: "sceKernelStartModule",
    0xCEE345D4: "sceKernelDelayThread",
    0xF6427665: "sceKernelSetCompiledSdkVersion",
    0xE81CAF8F: "sceKernelLoadModule",
    0x04B7766E: "sceKernelStartModule",
    0x68DA9E36: "sceKernelDelayThread",
    0x0282A7BA: "sceKernelSetCompiledSdkVersion370",
    0x28B6489C: "sceKernelDeleteThread",
    0x278C0DF5: "sceKernelWaitThreadEnd",
    0x092968F4: "sceKernelExitThread",
    0xAA73C935: "sceKernelExitGame",
    0x342061E5: "sceKernelSetCompiledSdkVersion",
    0xF77D77CB: "sceKernelSetCompiledSdkVersion_2",
    0xEBD177D6: "sceKernelDelayThreadCB",
    0xDFA8BAF8: "sceKernelSysClock2USec",
    0xEDBA5844: "sceKernelUSec2SysClock",
    0x82BC5777: "sceKernelCreateSema",
    0x293B45B8: "sceKernelGetThreadId",
    0x71BC9871: "sceKernelChangeThreadPriority",
    0x46EBB729: "sceKernelWaitSema",
    0x4AC57943: "sceKernelRegisterExitCallback",
    0x82826F70: "sceKernelSleepThreadCB",
}

def r16(data, off): return struct.unpack_from("<H", data, off)[0]
def r32(data, off): return struct.unpack_from("<I", data, off)[0]
def w32(data, off, val): struct.pack_into("<I", data, off, val & 0xFFFFFFFF)

class ELFSection:
    def __init__(self, name, sh_type, addr, offset, size, data):
        self.name, self.type, self.addr = name, sh_type, addr
        self.offset, self.size, self.data = offset, size, data

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
        self.entry, self.e_type, self.gp_value = 0, 0, 0
        self.hle_imports = []

        self._parse()
        if self.e_type == ET_SCE_PRX:
            self._apply_relocations()

        self._extract_hle_data()

    def _parse(self):
        with open(self.path, "rb") as f: self._raw = f.read()
        raw = self._raw
        if raw[:4] != ELF_MAGIC: raise ValueError("EBOOT.BIN inválido ou criptografado.")

        self.e_type = r16(raw, 0x10)
        entry_raw = r32(raw, 0x18)
        self.entry = entry_raw + (self.load_base if self.e_type == ET_SCE_PRX else 0)

        e_phoff, e_shoff = r32(raw, 0x1C), r32(raw, 0x20)
        e_phnum, e_phentsize = r16(raw, 0x2C), r16(raw, 0x2A)
        e_shnum, e_shentsize, e_shstrndx = r16(raw, 0x30), r16(raw, 0x2E), r16(raw, 0x32)

        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            self.segments.append(ELFSegment(
                r32(raw, off), r32(raw, off+4), r32(raw, off+8),
                r32(raw, off+16), r32(raw, off+20), r32(raw, off+24),
                raw[r32(raw, off+4):r32(raw, off+4)+r32(raw, off+16)]
            ))

        strtab_off = r32(raw, e_shoff + e_shstrndx * e_shentsize + 0x10)
        strtab_sz  = r32(raw, e_shoff + e_shstrndx * e_shentsize + 0x14)
        strtab = raw[strtab_off:strtab_off+strtab_sz]

        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            name = strtab[r32(raw, off):].split(b'\x00')[0].decode("ascii", errors="replace")
            self.sections.append(ELFSection(
                name, r32(raw, off+4), r32(raw, off+12),
                r32(raw, off+16), r32(raw, off+20),
                raw[r32(raw, off+16):r32(raw, off+16)+r32(raw, off+20)]
            ))

    def _addr_to_offset(self, vaddr):
        """
        Converte vaddr (já com ou sem base) para offset no arquivo bruto.
        Tenta primeiro com o vaddr como está, depois subtrai load_base
        para cobrir tanto ELFs linkados em 0x08800000 quanto PRXs com
        offsets internos a partir de 0.
        """
        for seg in self.segments:
            if seg.p_vaddr <= vaddr < seg.p_vaddr + seg.p_memsz:
                return seg.p_offset + (vaddr - seg.p_vaddr)
        # Fallback: tenta sem a base (para endereços já relocados)
        vaddr_no_base = vaddr - self.load_base
        if vaddr_no_base >= 0:
            for seg in self.segments:
                if seg.p_vaddr <= vaddr_no_base < seg.p_vaddr + seg.p_memsz:
                    return seg.p_offset + (vaddr_no_base - seg.p_vaddr)
        return 0

    def _extract_hle_data(self):
        print("[parse_elf] Extraindo NIDs nativos via sceModuleInfo...")
        raw = self._raw
        modinfo_vaddr = 0

        # 1. Tenta por PT_PRXINFO padrão
        for seg in self.segments:
            if seg.p_type == PT_PRXINFO:
                modinfo_vaddr = seg.p_vaddr
                break

        # 2. Tenta pelo nome da seção
        if modinfo_vaddr == 0:
            for sec in self.sections:
                if ".rodata.sceModuleInfo" in sec.name:
                    modinfo_vaddr = sec.addr
                    break

        # 3. ====== HEURÍSTICA DEFINITIVA PARA PRX ======
        if modinfo_vaddr == 0 and len(self.segments) > 0:
            print("  [AVISO] Tabela sceModuleInfo não encontrada. Iniciando varredura heurística...")
            seg0 = self.segments[0]
            max_offset = seg0.p_filesz
            
            # Varre o segmento procurando a assinatura da struct sceModuleInfo
            for i in range(0, max_offset - 0x34, 4):
                gp_raw  = r32(seg0.data, i + 0x20)
                exp_top = r32(seg0.data, i + 0x24)
                exp_end = r32(seg0.data, i + 0x28)
                imp_top = r32(seg0.data, i + 0x2C)
                imp_end = r32(seg0.data, i + 0x30)

                # CRÍTICO: No PRX, esses ponteiros são OFFSETS RELATIVOS (não endereços virtuais baseados em 0x08800000).
                # Portanto, verificamos se eles apontam validamente para dentro do próprio arquivo.
                if (0 < imp_top < imp_end <= max_offset) and (exp_top == 0 or exp_top <= exp_end <= max_offset) and (0 < gp_raw < max_offset):
                    
                    # Valida o nome do módulo (primeiros 28 bytes: ascii visível seguido de nulls)
                    chunk = seg0.data[i:i+28]
                    if chunk[0] != 0:
                        try:
                            parts = chunk.split(b'\x00', 1)
                            if len(parts) == 2 and all(b == 0 for b in parts[1]):
                                name_str = parts[0].decode('ascii')
                                if name_str.isprintable():
                                    modinfo_vaddr = seg0.p_vaddr + i
                                    print(f"  [+] sceModuleInfo encontrada via heurística no offset 0x{i:08X} (Nome: '{name_str}')")
                                    break
                        except Exception:
                            pass
        # ===============================================

        if modinfo_vaddr == 0:
            print("  [ERRO] Tabela sceModuleInfo não encontrada mesmo após varredura!")
            return

        offset = self._addr_to_offset(modinfo_vaddr)
        if offset == 0:
            print(f"  [ERRO] Não foi possível mapear sceModuleInfo vaddr=0x{modinfo_vaddr:08X}")
            return

        # Agora aplicamos a base (load_base) aos offsets encontrados para ter o valor real na RAM
        gp_raw = r32(raw, offset + 0x20)
        if self.e_type == ET_SCE_PRX:
            if gp_raw < self.load_base:
                self.gp_value = gp_raw + self.load_base
            else:
                self.gp_value = gp_raw
        else:
            self.gp_value = gp_raw

        imp_top = r32(raw, offset + 0x2C)
        imp_end = r32(raw, offset + 0x30)

        if self.e_type == ET_SCE_PRX:
            if imp_top < self.load_base: imp_top += self.load_base
            if imp_end < self.load_base: imp_end += self.load_base

        print(f"  [+] GP Value Final: 0x{self.gp_value:08X} (raw=0x{gp_raw:08X})")
        print(f"  [+] Import Table: 0x{imp_top:08X} -> 0x{imp_end:08X}")

        curr_imp = imp_top
        while curr_imp < imp_end:
            imp_off = self._addr_to_offset(curr_imp)
            if imp_off == 0: break

            name_ptr    = r32(raw, imp_off)
            func_count  = r16(raw, imp_off + 0x0A)
            nid_tbl_ptr = r32(raw, imp_off + 0x0C)
            stub_ptr    = r32(raw, imp_off + 0x10)

            if self.e_type == ET_SCE_PRX:
                if name_ptr and name_ptr < self.load_base:    name_ptr    += self.load_base
                if nid_tbl_ptr < self.load_base:              nid_tbl_ptr += self.load_base
                if stub_ptr    < self.load_base:              stub_ptr    += self.load_base

            mod_name = "syslib"
            if name_ptr:
                name_off = self._addr_to_offset(name_ptr)
                if name_off:
                    mod_name = raw[name_off:].split(b'\x00')[0].decode("ascii", errors="replace")

            for i in range(func_count):
                nid_off = self._addr_to_offset(nid_tbl_ptr + i*4)
                if nid_off == 0: continue

                nid = r32(raw, nid_off)
                stub_addr = stub_ptr + i*8

                func_name = KNOWN_NIDS.get(nid, f"nid_0x{nid:08X}")
                self.hle_imports.append({
                    "stub_addr": f"0x{stub_addr:08X}",
                    "nid":       f"0x{nid:08X}",
                    "name":      func_name,
                    "module":    mod_name
                })

            curr_imp += 20

        print(f"[parse_elf] ✅ {len(self.hle_imports)} NIDs resolvidos!")

    def _apply_relocations(self):
        base, image = self.load_base, bytearray(self._raw)
        rel_count, hi16_pending = 0, []

        ctors_vaddr, ctors_size = 0, 0
        for sec in self.sections:
            if sec.name == ".ctors":
                ctors_vaddr = sec.addr
                ctors_size  = sec.size
                print(f"[parse_elf] Secao .ctors encontrada: vaddr 0x{ctors_vaddr:08X}, size {ctors_size}")
                break

        for sec in self.sections:
            if sec.type != SHT_REL: continue
            data = sec.data
            for i in range(len(data) // 8):
                off, info = r32(data, i*8), r32(data, i*8+4)
                r_type = info & 0xFF
                if off + 4 > len(image): continue
                orig = r32(image, off)
                if   r_type == R_MIPS_32:
                    w32(image, off, orig + base); rel_count += 1
                elif r_type == R_MIPS_26:
                    target = (orig & 0x03FFFFFF) + (base >> 2)
                    w32(image, off, (orig & 0xFC000000) | (target & 0x03FFFFFF)); rel_count += 1
                elif r_type == R_MIPS_HI16:
                    hi16_pending.append((off, orig))
                elif r_type == R_MIPS_LO16:
                    lo_s = (orig & 0xFFFF) if (orig & 0x8000) == 0 else (orig & 0xFFFF) - 0x10000
                    for h_off, h_orig in hi16_pending:
                        full = ((h_orig & 0xFFFF) << 16) + lo_s + base
                        w32(image, h_off, (h_orig & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
                        rel_count += 1
                    w32(image, off, (orig & 0xFFFF0000) | ((lo_s + base) & 0xFFFF))
                    hi16_pending.clear(); rel_count += 1

        if ctors_vaddr != 0:
            ctors_off = self._addr_to_offset(ctors_vaddr)
            for i in range(0, ctors_size, 4):
                ptr_off  = ctors_off + i
                orig_ptr = r32(image, ptr_off)
                if orig_ptr != 0 and orig_ptr != 0xFFFFFFFF and orig_ptr < base:
                    w32(image, ptr_off, orig_ptr + base)
                    rel_count += 1
            print(f"[parse_elf] Construtores globais realocados para a RAM.")

        self._relocated_image = image
        print(f"[parse_elf] {rel_count} relocações aplicadas no total.")

    def export_json(self, out_path):
        ctors_addr, ctors_size = 0, 0
        for sec in self.sections:
            if sec.name == ".ctors":
                ctors_addr = sec.addr + (self.load_base if self.e_type == ET_SCE_PRX else 0)
                ctors_size = sec.size
                break

        meta = {
            "entry":      self.entry,
            "load_base":  self.load_base,
            "gp_value":   self.gp_value,   # ← já está com base somada corretamente
            "ctors_addr": ctors_addr,
            "ctors_size": ctors_size,
            "code_segments": [
                {
                    "vaddr":  s.p_vaddr + (self.load_base if self.e_type == ET_SCE_PRX else 0),
                    "offset": s.p_offset,
                    "size":   s.p_filesz
                }
                for s in self.segments if s.is_executable()
            ],
            "hle_imports": self.hle_imports
        }

        with open(out_path, "w") as f:
            json.dump(meta, f, indent=2)

        img_path = out_path.replace(".json", "_image.bin")
        with open(img_path, "wb") as f:
            f.write(getattr(self, "_relocated_image", self._raw))

        print(f"[parse_elf] Metadados exportados para {out_path}")
        print(f"[parse_elf] GP final gravado no JSON: 0x{self.gp_value:08X}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Uso: {sys.argv[0]} <EBOOT.BIN>")
        sys.exit(1)
    elf = PSPElf(sys.argv[1])
    elf.export_json("elf_meta.json")