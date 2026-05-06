#!/usr/bin/env python3
"""
disasm_to_c.py - Traduz código MIPS Allegrex (PSP) para C portável, com injeção HLE.
"""

import struct
import sys
import os
import json
import re
import gzip
from concurrent.futures import ThreadPoolExecutor

# ─── Configurações ─────────────────────────────────────────────────────────────
META_JSON      = "elf_meta.json"
IMAGE_BIN      = "elf_meta_image.bin"
PPMAP_FILE     = "gow.ppmap"  # <<< COLOQUE O NOME DO SEU ARQUIVO PPMAP ORIGINAL AQUI!
OUT_DIR        = "out"
FUNCS_DIR      = os.path.join(OUT_DIR, "funcs")
MAX_FUNC_LINES = 8000

# ─── Nomes dos registradores MIPS ─────────────────────────────────────────────
REG = [
    "zero","at","v0","v1",
    "a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1",
    "gp","sp","fp","ra"
]

def reg(n):
    if n == 0: return "cpu->zero"
    return f"cpu->{REG[n]}"

def imm_s16(imm16):
    return imm16 if imm16 < 0x8000 else imm16 - 0x10000

def raw_hex(raw):
    return f"{raw:08X}"

class Instr:
    __slots__ = ("addr","raw","op","rs","rt","rd","shamt","funct",
                 "imm","imm_s","target26","is_branch","is_jump",
                 "is_call","has_delay","c_line")

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
        self.is_branch  = False
        self.is_jump    = False
        self.is_call    = False
        self.has_delay  = False
        self.c_line     = ""

class MIPStoC:
    def __init__(self):
        if not os.path.exists(META_JSON) or not os.path.exists(IMAGE_BIN):
            print("ERRO: Arquivos meta não encontrados.")
            sys.exit(1)

        with open(META_JSON) as f:
            self._meta = json.load(f)
        with open(IMAGE_BIN, "rb") as f:
            self._image = f.read()

        self._entry       = self._meta["entry"]
        self._load_base   = self._meta["load_base"]
        self._code_segs   = self._meta["code_segments"]
        self._stubs       = {s["vaddr"]: s["module"] for s in self._meta.get("syscall_stubs", [])}
        self._func_starts = set()
        
        self.syscall_map  = {}
        self._load_symbols()

        if os.path.isdir(FUNCS_DIR):
            for filename in os.listdir(FUNCS_DIR):
                if filename.endswith('.c') or filename.endswith('.h'):
                    os.unlink(os.path.join(FUNCS_DIR, filename))
        os.makedirs(FUNCS_DIR, exist_ok=True)
        for filename in ('func_table.c', 'func_table.h'):
            path = os.path.join(OUT_DIR, filename)
            if os.path.exists(path):
                os.unlink(path)

    def _load_symbols(self):
        # 1. A NOSSA LISTA VIP (Engenharia Reversa Manual)
        # Endereços já ajustados para o PRX original
        self.syscall_map[0x08B01DF4] = "sceKernelSetCompiledSdkVersion"
        self.syscall_map[0x08B01D64] = "sceKernelCreateThread"
        self.syscall_map[0x08B01D14] = "sceKernelStartThread"
        self.syscall_map[0x08B01ED4] = "sceIoDread"
        
        count = 4 # Já começamos com as 4 vitais
        
        SYMBOL_FILE = "gow.sym" 
        if not os.path.exists(SYMBOL_FILE):
            print(f"[AVISO] {SYMBOL_FILE} nao encontrado! Usando apenas Lista VIP.")
            return
            
        print(f"[disasm] Carregando simbolos de {SYMBOL_FILE}...")
        
        with open(SYMBOL_FILE, "rt", encoding="utf-8", errors="ignore") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 2:
                    try:
                        addr = int(parts[0], 16)
                        raw_name = parts[-1].split(',')[0] 
                        addr -= 0x4000
                        
                        # 2. LIMPEZA AGRESSIVA: Remove todos os 'z' e '_' do começo do nome
                        name = raw_name.lstrip("z_")
                        
                        if name.startswith("sce") or "Thread" in name or "Module" in name:
                            # Se não for um dos VIPs que já mapeamos, adiciona!
                            if addr not in self.syscall_map:
                                self.syscall_map[addr] = name
                                count += 1
                    except (ValueError, IndexError):
                        continue
                        
        print(f"[disasm] {count} Syscalls mapeadas com sucesso!")


    def _read32(self, vaddr):
        for seg in self._code_segs:
            start = seg["vaddr"]
            end   = start + seg["size"]
            if start <= vaddr < end:
                file_off = seg["offset"] + (vaddr - start)
                if file_off < 0 or file_off + 4 > len(self._image): return None
                return struct.unpack_from("<I", self._image, file_off)[0]
        return None

    def _in_code(self, vaddr):
        for seg in self._code_segs:
            if seg["vaddr"] <= vaddr < seg["vaddr"] + seg["size"]: return True
        return False

    def _discover_functions(self):
        print("[disasm] Passe 1: descobrindo funções...")
        self._func_starts.add(self._entry)
        for vaddr in self._stubs: self._func_starts.add(vaddr)
        
        # Adiciona também todas as funções mapeadas pelo PPMAP
        for vaddr in self.syscall_map.keys():
            self._func_starts.add(vaddr)

        for seg in self._code_segs:
            vaddr = seg["vaddr"]
            end   = vaddr + seg["size"]
            while vaddr < end:
                raw = self._read32(vaddr)
                if raw is None:
                    vaddr += 4; continue

                op, rt, rs, imm = (raw >> 26) & 0x3F, (raw >> 16) & 0x1F, (raw >> 21) & 0x1F, raw & 0xFFFF
                imms = imm if imm < 0x8000 else imm - 0x10000

                if op in (0x02, 0x03):
                    dest = self._load_base | ((raw & 0x03FFFFFF) << 2)
                    if self._in_code(dest): self._func_starts.add(dest)
                elif op in (0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
                    offset = vaddr + 4 + (imms << 2)
                    if self._in_code(offset): self._func_starts.add(offset)
                elif op == 0x09 and rs == 29 and rt == 29 and imm >= 0x8000:
                    self._func_starts.add(vaddr)

                vaddr += 4
        print(f"[disasm] {len(self._func_starts)} funções encontradas.")

    def _decode(self, instr: Instr):
        op, rs, rt, rd, sa, fn = instr.op, instr.rs, instr.rt, instr.rd, instr.shamt, instr.funct
        imm, imms, addr, raw = instr.imm, instr.imm_s, instr.addr, instr.raw
        line = ""

        if op == 0x00:
            if   fn == 0x00: line = "/* nop */" if raw == 0 else f"{reg(rd)} = (uint32_t){reg(rt)} << {sa};"
            elif fn == 0x02: line = f"{reg(rd)} = (uint32_t){reg(rt)} >> {sa};"
            elif fn == 0x03: line = f"{reg(rd)} = (int32_t){reg(rt)} >> {sa};"
            elif fn == 0x04: line = f"{reg(rd)} = (uint32_t){reg(rt)} << ({reg(rs)} & 31);"
            elif fn == 0x06: line = f"{reg(rd)} = (uint32_t){reg(rt)} >> ({reg(rs)} & 31);"
            elif fn == 0x07: line = f"{reg(rd)} = (int32_t){reg(rt)} >> ({reg(rs)} & 31);"
            elif fn == 0x08:
                instr.is_jump, instr.has_delay = True, True
                line = f"/* DS */ cpu->pc = cpu->ra; return;" if rs == 31 else f"/* DS */ cpu->pc = {reg(rs)}; return;"
            elif fn == 0x09:
                instr.is_call, instr.has_delay = True, True
                line = f"/* DS */ {reg(rd)} = 0x{addr+8:08X}u; cpu->pc = {reg(rs)}; return;"
            elif fn == 0x0C: code = (raw >> 6) & 0xFFFFF; line = f"CPU_SYSCALL(cpu, 0x{code:05X});"
            elif fn == 0x0F: line = "/* sync */"
            elif fn == 0x10: line = f"{reg(rd)} = cpu->hi;"
            elif fn == 0x11: line = f"cpu->hi = {reg(rs)};"
            elif fn == 0x12: line = f"{reg(rd)} = cpu->lo;"
            elif fn == 0x13: line = f"cpu->lo = {reg(rs)};"
            elif fn == 0x18: line = f"{{ int64_t _m = (int64_t)(int32_t){reg(rs)} * (int64_t)(int32_t){reg(rt)}; cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}"
            elif fn == 0x19: line = f"{{ uint64_t _m = (uint64_t){reg(rs)} * (uint64_t){reg(rt)}; cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}"
            elif fn == 0x1A: line = f"if ({reg(rt)}) {{ cpu->lo=(uint32_t)((int32_t){reg(rs)}/(int32_t){reg(rt)}); cpu->hi=(uint32_t)((int32_t){reg(rs)}%(int32_t){reg(rt)}); }}"
            elif fn == 0x1B: line = f"if ({reg(rt)}) {{ cpu->lo={reg(rs)}/{reg(rt)}; cpu->hi={reg(rs)}%{reg(rt)}; }}"
            elif fn == 0x20: line = f"{reg(rd)} = (uint32_t)((int32_t){reg(rs)}+(int32_t){reg(rt)});"
            elif fn == 0x21: line = f"{reg(rd)} = {reg(rs)} + {reg(rt)};"
            elif fn == 0x22: line = f"{reg(rd)} = (uint32_t)((int32_t){reg(rs)}-(int32_t){reg(rt)});"
            elif fn == 0x23: line = f"{reg(rd)} = {reg(rs)} - {reg(rt)};"
            elif fn == 0x24: line = f"{reg(rd)} = {reg(rs)} & {reg(rt)};"
            elif fn == 0x25: line = f"{reg(rd)} = {reg(rs)} | {reg(rt)};"
            elif fn == 0x26: line = f"{reg(rd)} = {reg(rs)} ^ {reg(rt)};"
            elif fn == 0x27: line = f"{reg(rd)} = ~({reg(rs)} | {reg(rt)});"
            elif fn == 0x2A: line = f"{reg(rd)} = ((int32_t){reg(rs)} < (int32_t){reg(rt)}) ? 1 : 0;"
            elif fn == 0x2B: line = f"{reg(rd)} = ({reg(rs)} < {reg(rt)}) ? 1 : 0;"
            elif fn == 0x16: line = f"{reg(rd)} = __builtin_clz({reg(rs)});"
            elif fn == 0x17: line = f"{reg(rd)} = __builtin_ctz({reg(rs)});"
            elif fn == 0x28: line = f"{reg(rd)} = ({reg(rs)} == 0) ? {reg(rt)} : {reg(rd)};"
            elif fn == 0x29: line = f"{reg(rd)} = ({reg(rs)} != 0) ? {reg(rt)} : {reg(rd)};"
            elif fn == 0x2C: line = f"{{ int64_t _m=(int64_t)(int32_t){reg(rs)}*(int64_t)(int32_t){reg(rt)}; {reg(rd)}=(uint32_t)(_m>>32); }}"

        elif op == 0x01:
            offset = addr + 4 + (imms << 2)
            instr.is_branch, instr.has_delay = True, True
            if   rt in (0x00, 0x02): line = f"if ((int32_t){reg(rs)} < 0) {{ /* DS */ goto loc_{offset:08X}; }}"
            elif rt in (0x01, 0x03): line = f"if ((int32_t){reg(rs)} >= 0) {{ /* DS */ goto loc_{offset:08X}; }}"
            elif rt in (0x10, 0x12): 
                instr.is_call = True
                line = f"if ((int32_t){reg(rs)} < 0) {{ cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{offset:08X}; }}"
            elif rt in (0x11, 0x13): 
                instr.is_call = True
                line = f"if ((int32_t){reg(rs)} >= 0) {{ cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x02:
            dest = self._load_base | (instr.target26 << 2)
            instr.is_jump, instr.has_delay = True, True
            line = f"/* DS */ goto loc_{dest:08X};"

        elif op == 0x03:
            dest = self._load_base | (instr.target26 << 2)
            instr.is_call, instr.has_delay = True, True
            line = f"cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{dest:08X};"

        elif op in (0x04, 0x14):
            offset = addr + 4 + (imms << 2)
            instr.is_branch, instr.has_delay = True, True
            line = f"/* DS */ goto loc_{offset:08X};" if rs == 0 and rt == 0 else f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op in (0x05, 0x15):
            offset = addr + 4 + (imms << 2)
            instr.is_branch, instr.has_delay = True, True
            line = f"if ({reg(rs)} != {reg(rt)}) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op in (0x06, 0x16):
            offset = addr + 4 + (imms << 2)
            instr.is_branch, instr.has_delay = True, True
            line = f"if ((int32_t){reg(rs)} <= 0) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op in (0x07, 0x17):
            offset = addr + 4 + (imms << 2)
            instr.is_branch, instr.has_delay = True, True
            line = f"if ((int32_t){reg(rs)} > 0) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x08: line = f"{reg(rt)} = (uint32_t)((int32_t){reg(rs)} - {-imms});" if imms < 0 else f"{reg(rt)} = (uint32_t)((int32_t){reg(rs)} + {imms});"
        elif op == 0x09: line = f"{reg(rt)} = {reg(rs)} - {-imms};" if imms < 0 else f"{reg(rt)} = {reg(rs)} + {imms};"
        elif op == 0x0A: line = f"{reg(rt)} = ((int32_t){reg(rs)} < {imms}) ? 1 : 0;"
        elif op == 0x0B: line = f"{reg(rt)} = ({reg(rs)} < (uint32_t){imms}) ? 1 : 0;"
        elif op == 0x0C: line = f"{reg(rt)} = {reg(rs)} & 0x{imm:04X}u;"
        elif op == 0x0D: line = f"{reg(rt)} = {reg(rs)} | 0x{imm:04X}u;"
        elif op == 0x0E: line = f"{reg(rt)} = {reg(rs)} ^ 0x{imm:04X}u;"
        elif op == 0x0F: line = f"{reg(rt)} = 0x{imm:04X}u << 16;"

        elif op == 0x20: line = f"{reg(rt)} = (uint32_t)(int8_t)MEM_R8(mem,{reg(rs)}+{imms});"
        elif op == 0x21: line = f"{reg(rt)} = (uint32_t)(int16_t)MEM_R16(mem,{reg(rs)}+{imms});"
        elif op == 0x22: line = f"/* LWL stub */"
        elif op == 0x23: line = f"{reg(rt)} = MEM_R32(mem,{reg(rs)}+{imms});"
        elif op == 0x24: line = f"{reg(rt)} = (uint32_t)MEM_R8(mem,{reg(rs)}+{imms});"
        elif op == 0x25: line = f"{reg(rt)} = (uint32_t)MEM_R16(mem,{reg(rs)}+{imms});"
        elif op == 0x26: line = f"/* LWR stub */"

        elif op == 0x28: line = f"MEM_W8(mem,{reg(rs)}+{imms},(uint8_t){reg(rt)});"
        elif op == 0x29: line = f"MEM_W16(mem,{reg(rs)}+{imms},(uint16_t){reg(rt)});"
        elif op == 0x2A: line = f"/* SWL stub */"
        elif op == 0x2B: line = f"MEM_W32(mem,{reg(rs)}+{imms},{reg(rt)});"
        elif op == 0x2E: line = f"/* SWR stub */"
        elif op in (0x11, 0x12, 0x13, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x31, 0x35, 0x39, 0x3D): line = f"/* FPU stub */"
        else: line = f"/* UNKNOWN op=0x{op:02X} */"

        instr.c_line = line

    def _collect_instrs(self, start_addr):
        addr = start_addr
        next_funcs = sorted(a for a in self._func_starts if a > start_addr)
        stop = next_funcs[0] if next_funcs else start_addr + MAX_FUNC_LINES * 4

        instrs = []
        while addr < stop:
            raw = self._read32(addr)
            if raw is None: break
            instr = Instr(addr, raw)
            self._decode(instr)
            instrs.append(instr)
            addr += 4
        return instrs

    def _format_function_body(self, start_addr, instrs):
        # 🔥 A MÁGICA DA INJEÇÃO HLE 🔥
        if start_addr in self.syscall_map:
            name = self.syscall_map[start_addr]
            lines = [
                f"  // [HLE] Injeção Direta de Syscall: {name}",
                f"  cpu->pc = cpu->ra; // Default: volta pra quem chamou",
                f'  psp_syscall(cpu, mem, "{name}"); // Chama a Syscall (pode sobrescrever o PC)',
                f"  return;"
            ]
            return lines, []

        lines, instr_addrs, emitted_addrs = [], {i.addr for i in instrs}, []

        for i, instr in enumerate(instrs):
            addr = instr.addr
            emitted_addrs.append(addr)
            lines.append(f" loc_{addr:08X}:;")

            ds_line = "/* nop */"
            if instr.has_delay:
                if i + 1 < len(instrs):
                    ds_instr = instrs[i + 1]
                    ds_raw = Instr(ds_instr.addr, ds_instr.raw)
                    self._decode(ds_raw)
                    ds_line = ds_raw.c_line or "/* nop */"
                else:
                    raw_ds = self._read32(addr + 4)
                    if raw_ds is not None:
                        ds_raw = Instr(addr + 4, raw_ds)
                        self._decode(ds_raw)
                        ds_line = ds_raw.c_line or "/* nop */"

            line = instr.c_line or ""
            is_likely = instr.op in (0x14, 0x15, 0x16, 0x17) or (instr.op == 0x01 and instr.rt in (0x02, 0x03, 0x12, 0x13))

            if "/* DS */" in line:
                branch_line = line.replace("/* DS */", ds_line, 1)
                if is_likely: branch_line += f" else {{ goto loc_{addr+8:08X}; }}"
            else:
                branch_line = line

            def sanitize_goto(match):
                tgt = int(match.group(1), 16)
                return match.group(0) if tgt in instr_addrs else f"cpu->pc = 0x{tgt:08X}u; return;"

            branch_line = re.sub(r'goto loc_([0-9A-Fa-f]{8});?', sanitize_goto, branch_line)
            is_invalid = "UNKNOWN" in branch_line or addr > 0x0A000000

            if is_invalid:
                clean_line = branch_line.replace("/*", "").replace("*/", "").strip()
                branch_line = f"/* Skip invalid/data @ 0x{addr:08X}: {clean_line} */"

            branch_line = re.sub(r'\breturn\b(?!\s*;)', 'return;', branch_line)
            branch_line = re.sub(r';\s*;', ';', branch_line)
            branch_line = re.sub(r'([^;{}\s])\s*}', r'\1; }', branch_line)
            branch_line = re.sub(r'\s+', ' ', branch_line)

            lines.append(f"  {branch_line}  /* 0x{raw_hex(instr.raw)} */")
        return lines, emitted_addrs

    def _emit_group(self, args):
        group_id, funcs = args
        path = os.path.join(FUNCS_DIR, f"funcs_{group_id:03d}.c")
        lines = ['#include "../../runtime/cpu.h"', '#include "../../runtime/memory.h"', '#include "../func_table.h"\n']

        for start_addr, instrs in funcs:
            fname = f"func_{start_addr:08X}"
            lines.append(f"\n/* ===== {fname} ===== */\nvoid {fname}(MIPS_CPU *cpu, uint8_t *mem) {{")
            
            body_lines, emitted_addrs = self._format_function_body(start_addr, instrs)

            if emitted_addrs:
                lines.append("  switch(cpu->pc) {")
                for ea in emitted_addrs: lines.append(f"    case 0x{ea:08X}: goto loc_{ea:08X};")
                lines.append("  }")

            lines.extend(body_lines)

            if body_lines and not body_lines[-1].strip().endswith("return;"):
                last_addr = instrs[-1].addr
                lines.append(f"  cpu->pc = 0x{last_addr + 4:08X}u; // Avanca o PC de forma absoluta")
                lines.append("  return;")

            lines.append("}")

        with open(path, "w", encoding="utf-8") as f: f.write("\n".join(lines))
        print(f"[Thread] Arquivo funcs_{group_id:03d}.c gerado com sucesso.")

    def _emit_table(self, func_entries):
        h = [
            "#pragma once", 
            '#include "runtime/cpu.h"', 
            "", 
            "typedef void (*PSPFunc)(MIPS_CPU *cpu, uint8_t *mem);", 
            "", 
            "extern uint32_t psp_func_addrs[];",
            "// Assinatura global da Syscall (Injetada automaticamente)",
            "void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, const char *name);"
        ]
        unique_func_names = sorted(list(set(name for _, name in func_entries)))
        h.extend([f"void {name}(MIPS_CPU *cpu, uint8_t *mem);" for name in unique_func_names])
        h.extend(["", "extern PSPFunc psp_func_table[];", f"#define PSP_FUNC_COUNT {len(func_entries)}"])
        
        with open(os.path.join(OUT_DIR, "func_table.h"), "w") as f: f.write("\n".join(h))

        c = ['#include "func_table.h"', "", "PSPFunc psp_func_table[] = {"]
        c.extend([f"    {name}," for _, name in func_entries])
        c.extend(["};", "", "uint32_t psp_func_addrs[] = {"])
        c.extend([f"    0x{addr:08X}," for addr, _ in func_entries])
        c.append("};")
        
        with open(os.path.join(OUT_DIR, "func_table.c"), "w") as f: f.write("\n".join(c))

    def run(self):
        self._discover_functions()
        group_size = 100
        groups, current_group, func_entries = [], [], []

        for i, start in enumerate(sorted(self._func_starts)):
            instrs = self._collect_instrs(start)
            if not instrs: continue
            current_group.append((start, instrs))
            name = f"func_{start:08X}"
            
            for ins in instrs:
                func_entries.append((ins.addr, name))

            if len(current_group) >= group_size:
                groups.append((len(groups), current_group))
                current_group = []

        if current_group: groups.append((len(groups), current_group))

        print(f"[disasm] Gerando arquivos C usando {os.cpu_count()} threads...")
        with ThreadPoolExecutor() as executor: executor.map(self._emit_group, groups)

        self._emit_table(func_entries)
        print("[disasm] Processo finalizado com Mapeamento Total!")

if __name__ == "__main__":
    MIPStoC().run()