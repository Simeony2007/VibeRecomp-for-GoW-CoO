#!/usr/bin/env python3
"""
Tradutor_PRX.py - Recompilador Estático e Relocador de ELF/PRX para C.
"""

import struct
import sys
import os

# ─── Configurações ─────────────────────────────────────────────────────────────
PRX_DIR   = "PRX_IN"       
OUT_DIR   = "out_prx"      

# ─── Motor MIPS ───────────────────────────────────────────────────────────────
REG = [
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
]

def reg(n): return "cpu->zero" if n == 0 else f"cpu->{REG[n]}"
def imm_s16(imm16): return imm16 if imm16 < 0x8000 else imm16 - 0x10000

class Instr:
    __slots__ = ("addr","raw","op","rs","rt","rd","shamt","funct",
                 "imm","imm_s","target26","is_jump","is_call","has_delay","c_line")

    def __init__(self, addr, raw):
        self.addr       = addr
        self.raw        = raw
        self.op         = (raw >> 26) & 0x3F
        self.rs         = (raw >> 21) & 0x1F
        self.rt         = (raw >> 16) & 0x1F
        self.rd         = (raw >> 11) & 0x1F
        self.shamt      = (raw >>  6) & 0x1F
        self.funct      = raw & 0x3F
        self.imm        = raw & 0xFFFF
        self.imm_s      = imm_s16(self.imm)
        self.target26   = raw & 0x03FFFFFF
        self.is_jump    = False
        self.is_call    = False
        self.has_delay  = False
        self.c_line     = ""

class PRXTranslator:
    def __init__(self, filepath, base_addr):
        self.filepath = filepath
        self.name = os.path.splitext(os.path.basename(filepath))[0].replace("-", "_").replace(".", "_")
        self.image = b""
        self.base_addr = base_addr
        self.code_segments_raw = []
        self.code_segments = []
        self.entry_point = 0

    def parse_elf(self):
        with open(self.filepath, "rb") as f:
            self.image = f.read()

        if self.image[:4] != b"\x7fELF":
            print(f"[-] Ignorando {self.name}: Nao e um arquivo ELF valido.")
            return False

        e_type, e_machine, e_version, self.entry_point, e_phoff = struct.unpack_from("<HHIIII", self.image, 16)[0:5]
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.image, 42)

        # Se o entry point estiver zerado (PRX não relocados), aplicamos a base
        if self.entry_point < 0x08000000:
            self.entry_point += self.base_addr

        print(f"[*] Modulo: {self.name} | Base Virtual: 0x{self.base_addr:08X} | Entry: 0x{self.entry_point:08X}")

        for i in range(e_phnum):
            ph_offset = e_phoff + (i * e_phentsize)
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<IIIIIIII", self.image, ph_offset)
            
            if p_type == 1 and (p_flags & 1): # PT_LOAD e Executável
                self.code_segments_raw.append({"offset": p_offset, "vaddr_raw": p_vaddr, "size": p_filesz})
                self.code_segments.append({"offset": p_offset, "vaddr": p_vaddr + self.base_addr, "size": p_filesz})
        
        return len(self.code_segments) > 0

    def vaddr_to_offset(self, vaddr):
        for seg in self.code_segments_raw:
            start = seg["vaddr_raw"]
            if start <= vaddr < start + seg["size"]:
                return seg["offset"] + (vaddr - start)
        return None

    def apply_elf_relocations(self):
        """ Vare o arquivo em busca da tabela de relocações SHT_REL e injeta os endereços corretos na imagem """
        try:
            e_shoff = struct.unpack_from("<I", self.image, 32)[0]
            e_shentsize = struct.unpack_from("<H", self.image, 46)[0]
            e_shnum = struct.unpack_from("<H", self.image, 48)[0]
        except: return 0

        img = bytearray(self.image)
        relocs = 0

        for i in range(e_shnum):
            sh_offset = e_shoff + i * e_shentsize
            sh_type = struct.unpack_from("<I", img, sh_offset + 4)[0]
            
            if sh_type == 9: # SHT_REL (Tabela de Relocação Oficial ELF)
                sec_off = struct.unpack_from("<I", img, sh_offset + 16)[0]
                sec_size = struct.unpack_from("<I", img, sh_offset + 20)[0]
                
                hi16_ptr = 0
                for r in range(sec_size // 8):
                    r_offset, r_info = struct.unpack_from("<II", img, sec_off + r * 8)
                    r_type = r_info & 0xFF
                    
                    file_off = self.vaddr_to_offset(r_offset)
                    if file_off is None: continue
                    
                    if r_type == 2: # R_MIPS_32
                        val = struct.unpack_from("<I", img, file_off)[0]
                        struct.pack_into("<I", img, file_off, (val + self.base_addr) & 0xFFFFFFFF)
                        relocs += 1
                    elif r_type == 4: # R_MIPS_26
                        val = struct.unpack_from("<I", img, file_off)[0]
                        target = ((val & 0x03FFFFFF) << 2) + self.base_addr
                        val = (val & 0xFC000000) | ((target >> 2) & 0x03FFFFFF)
                        struct.pack_into("<I", img, file_off, val)
                        relocs += 1
                    elif r_type == 5: hi16_ptr = file_off # R_MIPS_HI16
                    elif r_type == 6: # R_MIPS_LO16
                        if hi16_ptr != 0:
                            hi_inst = struct.unpack_from("<I", img, hi16_ptr)[0]
                            lo_inst = struct.unpack_from("<I", img, file_off)[0]
                            
                            hi_val, lo_val = hi_inst & 0xFFFF, lo_inst & 0xFFFF
                            if lo_val >= 0x8000: lo_val -= 0x10000
                            
                            new_val = (hi_val << 16) + lo_val + self.base_addr
                            new_hi = ((new_val >> 16) + ((new_val & 0x8000) >> 15)) & 0xFFFF
                            
                            struct.pack_into("<I", img, hi16_ptr, (hi_inst & 0xFFFF0000) | new_hi)
                            struct.pack_into("<I", img, file_off, (lo_inst & 0xFFFF0000) | (new_val & 0xFFFF))
                            hi16_ptr = 0
                            relocs += 2

        self.image = bytes(img)
        print(f"  [+] Relocações Aplicadas: {relocs}")
        return relocs

    def _read32(self, vaddr):
        for seg in self.code_segments:
            start, end = seg["vaddr"], seg["vaddr"] + seg["size"]
            if start <= vaddr < end:
                file_off = seg["offset"] + (vaddr - start)
                return struct.unpack_from("<I", self.image, file_off)[0]
        return None

    def decode_instr(self, instr: Instr):
        op, rs, rt, rd, sa, fn = instr.op, instr.rs, instr.rt, instr.rd, instr.shamt, instr.funct
        addr, imms = instr.addr, instr.imm_s
        line = f"/* UNKNOWN op=0x{op:02X} fn=0x{fn:02X} */"

        if op == 0x00:
            if fn == 0x08:
                instr.is_jump, instr.has_delay = True, True
                line = f"/* DS */ cpu->pc = cpu->ra; return;" if rs == 31 else f"/* DS */ cpu->pc = {reg(rs)}; return;"
            elif fn == 0x21: line = f"{reg(rd)} = {reg(rs)} + {reg(rt)};"
            elif fn == 0x24: line = f"{reg(rd)} = {reg(rs)} & {reg(rt)};"
            
        elif op == 0x02: # JUMP UNIVERSAL (Agora funciona porque a imagem tem a base)
            dest = ((addr + 4) & 0xF0000000) | (instr.target26 << 2)
            instr.is_jump, instr.has_delay = True, True
            line = f"/* DS */ goto loc_{dest:08X};"

        elif op == 0x03: # JAL UNIVERSAL
            dest = ((addr + 4) & 0xF0000000) | (instr.target26 << 2)
            instr.is_call, instr.has_delay = True, True
            line = f"cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{dest:08X};"

        # INSTRUÇÕES DE STRING E DESALINHAMENTO (Consertam caminhos de arquivos)
        elif op == 0x22: line = f"{reg(rt)} = psp_lwl(mem, {reg(rs)} + {imms}, {reg(rt)});"
        elif op == 0x26: line = f"{reg(rt)} = psp_lwr(mem, {reg(rs)} + {imms}, {reg(rt)});"
        elif op == 0x2A: line = f"psp_swl(mem, {reg(rs)} + {imms}, {reg(rt)});"
        elif op == 0x2E: line = f"psp_swr(mem, {reg(rs)} + {imms}, {reg(rt)});"
        
        elif op == 0x23: line = f"{reg(rt)} = MEM_R32(mem,{reg(rs)}+{imms});"
        elif op == 0x2B: line = f"MEM_W32(mem,{reg(rs)}+{imms},{reg(rt)});"
        elif op == 0x09: line = f"{reg(rt)} = {reg(rs)} + {imms};"
        
        instr.c_line = line

    def scan_and_translate(self):
        print(f"[*] Varrendo codigo de {self.name}...")
        funcs_code = []
        for seg in self.code_segments:
            vaddr, end = seg["vaddr"], seg["vaddr"] + seg["size"]
            instrs = []
            while vaddr < end:
                raw = self._read32(vaddr)
                if raw is not None and raw != 0:
                    ins = Instr(vaddr, raw)
                    self.decode_instr(ins)
                    instrs.append(ins)
                vaddr += 4
            if instrs: funcs_code.append((seg["vaddr"], instrs))
        self.emit_c_code(funcs_code)

    def emit_c_code(self, funcs_code):
        os.makedirs(OUT_DIR, exist_ok=True)
        path = os.path.join(OUT_DIR, f"prx_{self.name}.c")
        
        lines = [
            f"// Gerado automaticamente - PRX: {self.name}",
            '#include "../runtime/cpu.h"',
            '#include "../runtime/memory.h"\n'
        ]

        lines.append(f"void module_start_{self.name}(MIPS_CPU *cpu, uint8_t *mem) {{")
        lines.append(f"  cpu->pc = 0x{self.entry_point:08X};")
        
        for start_addr, instrs in funcs_code:
            for ins in instrs:
                lines.append(f" loc_{ins.addr:08X}:;")
                clean = ins.c_line.replace("/* DS */", "").strip()
                lines.append(f"  {clean}")
        
        lines.append("  return;\n}")

        with open(path, "w") as f: f.write("\n".join(lines))
        print(f"[+] Biblioteca gerada em {path}\n")

if __name__ == "__main__":
    if not os.path.exists(PRX_DIR):
        print(f"Crie a pasta {PRX_DIR} e coloque os .prx dentro.")
        sys.exit(1)

    # Definimos endereços base seguros. O jogo principal usa 0x08800000. 
    # Para as bibliotecas PRX, injetamos a partir de 0x09000000 para não colidir!
    current_prx_base = 0x09000000 

    for filename in os.listdir(PRX_DIR):
        if filename.lower().endswith(".prx") or filename.lower().endswith(".elf"):
            translator = PRXTranslator(os.path.join(PRX_DIR, filename), current_prx_base)
            if translator.parse_elf():
                # Injeta os ponteiros corretos no binário!
                translator.apply_elf_relocations()
                translator.scan_and_translate()
            current_prx_base += 0x00100000 # Avança o endereço para o próximo módulo
    
    print("[!] Recompilação de Bibliotecas Concluida!")