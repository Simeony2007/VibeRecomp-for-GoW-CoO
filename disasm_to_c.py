#!/usr/bin/env python3
"""
disasm_to_c.py - Traduz código MIPS Allegrex (PSP) para C portável.

Lê o elf_meta.json + elf_meta_image.bin gerados pelo parse_elf.py
(que já têm as relocações aplicadas) e produz:
  - out/funcs/funcs_XXX.c       (arquivos contendo grupos de funções)
  - out/func_table.c            (tabela de ponteiros)
  - out/func_table.h            (declarações)

Uso: python3 disasm_to_c.py
"""

import struct
import sys
import os
import json
import re

# ─── Configurações ─────────────────────────────────────────────────────────────
META_JSON      = "elf_meta.json"
IMAGE_BIN      = "elf_meta_image.bin"
OUT_DIR        = "out"
FUNCS_DIR      = os.path.join(OUT_DIR, "funcs")
MAX_FUNC_LINES = 3000

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
    if n == 0:
        return "cpu->zero"  # O "buraco negro" de leitura!
    return f"cpu->{REG[n]}"

def imm_s16(imm16):
    return imm16 if imm16 < 0x8000 else imm16 - 0x10000

def raw_hex(raw):
    return f"{raw:08X}"

# ─── Instrução decodificada ────────────────────────────────────────────────────
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

# ─── Tradutor ──────────────────────────────────────────────────────────────────
class MIPStoC:
    def __init__(self):
        if not os.path.exists(META_JSON):
            print(f"ERRO: {META_JSON} não encontrado!")
            print("Rode primeiro: python3 parse_elf.py EBOOT.BIN")
            sys.exit(1)
        if not os.path.exists(IMAGE_BIN):
            print(f"ERRO: {IMAGE_BIN} não encontrado!")
            print("Rode primeiro: python3 parse_elf.py EBOOT.BIN")
            sys.exit(1)

        with open(META_JSON) as f:
            self._meta = json.load(f)
        with open(IMAGE_BIN, "rb") as f:
            self._image = f.read()

        self._entry       = self._meta["entry"]
        self._load_base   = self._meta["load_base"]
        self._code_segs   = self._meta["code_segments"]
        self._stubs       = {s["vaddr"]: s["module"]
                             for s in self._meta.get("syscall_stubs", [])}
        self._func_starts = set()

        os.makedirs(FUNCS_DIR, exist_ok=True)

    # ── Leitura da imagem relocada ─────────────────────────────────────────
    def _read32(self, vaddr):
        for seg in self._code_segs:
            start = seg["vaddr"]
            end   = start + seg["size"]
            if start <= vaddr < end:
                file_off = seg["offset"] + (vaddr - start)
                # Proteção contra leitura inválida
                if file_off < 0 or file_off + 4 > len(self._image):
                    return None
                return struct.unpack_from("<I", self._image, file_off)[0]
        return None

    def _in_code(self, vaddr):
        for seg in self._code_segs:
            if seg["vaddr"] <= vaddr < seg["vaddr"] + seg["size"]:
                return True
        return False

    # ── Passe 1: descobre inícios de funções ──────────────────────────────
    def _discover_functions(self):
        print("[disasm] Passe 1: descobrindo funções...")
        self._func_starts.add(self._entry)

        for vaddr in self._stubs:
            self._func_starts.add(vaddr)

        for seg in self._code_segs:
            vaddr = seg["vaddr"]
            end   = vaddr + seg["size"]
            while vaddr < end:
                raw = self._read32(vaddr)
                if raw is None:
                    vaddr += 4
                    continue

                op    = (raw >> 26) & 0x3F
                rt    = (raw >> 16) & 0x1F
                rs    = (raw >> 21) & 0x1F
                funct = raw & 0x3F
                imm   = raw & 0xFFFF
                imms  = imm if imm < 0x8000 else imm - 0x10000

                # ── JAL (Direct call)
                if op == 0x03:
                    dest = self._load_base + ((raw & 0x03FFFFFF) << 2)
                    if self._in_code(dest):
                        self._func_starts.add(dest)

                # ── J (Direct jump)
                elif op == 0x02:
                    dest = self._load_base + ((raw & 0x03FFFFFF) << 2)
                    if self._in_code(dest):
                        self._func_starts.add(dest)

                # ── Branch instructions (BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ, etc)
                elif op in (0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
                    offset = vaddr + 4 + (imms << 2)
                    if self._in_code(offset):
                        self._func_starts.add(offset)

                # ── Prólogo clássico: addiu $sp, $sp, -N (potencial início de função)
                if op == 0x09:
                    if rs == 29 and rt == 29 and imm >= 0x8000:
                        self._func_starts.add(vaddr)

                vaddr += 4

        print(f"[disasm] {len(self._func_starts)} funções encontradas.")

    # ── Decoder de instrução ───────────────────────────────────────────────
    def _decode(self, instr: Instr):
        op   = instr.op
        rs   = instr.rs
        rt   = instr.rt
        rd   = instr.rd
        sa   = instr.shamt
        fn   = instr.funct
        imm  = instr.imm
        imms = instr.imm_s
        addr = instr.addr
        raw  = instr.raw

        line = ""

        if op == 0x00:   # SPECIAL
            if   fn == 0x00:
                if raw == 0: line = "/* nop */"
                else: line = f"{reg(rd)} = (uint32_t){reg(rt)} << {sa};"        # SLL
            elif fn == 0x02: line = f"{reg(rd)} = (uint32_t){reg(rt)} >> {sa};"  # SRL
            elif fn == 0x03: line = f"{reg(rd)} = (int32_t){reg(rt)} >> {sa};"   # SRA
            elif fn == 0x04: line = f"{reg(rd)} = (uint32_t){reg(rt)} << ({reg(rs)} & 31);"  # SLLV
            elif fn == 0x06: line = f"{reg(rd)} = (uint32_t){reg(rt)} >> ({reg(rs)} & 31);"  # SRLV
            elif fn == 0x07: line = f"{reg(rd)} = (int32_t){reg(rt)} >> ({reg(rs)} & 31);"   # SRAV
            elif fn == 0x08:   # JR
                instr.is_jump  = True
                instr.has_delay = True
                if rs == 31: line = f"/* DS */ return;"
                else:        line = f"/* DS */ CPU_JR(cpu, {reg(rs)});"
            elif fn == 0x09:   # JALR
                instr.is_call  = True
                instr.has_delay = True
                line = f"/* DS */ {reg(rd)} = 0x{addr+8:08X}u; CPU_JALR(cpu, {reg(rs)});"
            elif fn == 0x0C:   # SYSCALL
                code = (raw >> 6) & 0xFFFFF
                line = f"CPU_SYSCALL(cpu, 0x{code:05X});"
            elif fn == 0x0F: line = "/* sync */"
            elif fn == 0x10: line = f"{reg(rd)} = cpu->hi;"   # MFHI
            elif fn == 0x11: line = f"cpu->hi = {reg(rs)};"   # MTHI
            elif fn == 0x12: line = f"{reg(rd)} = cpu->lo;"   # MFLO
            elif fn == 0x13: line = f"cpu->lo = {reg(rs)};"   # MTLO
            elif fn == 0x18:
                line = (f"{{ int64_t _m = (int64_t)(int32_t){reg(rs)} * (int64_t)(int32_t){reg(rt)}; "
                        f"cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}")  # MULT
            elif fn == 0x19:
                line = (f"{{ uint64_t _m = (uint64_t){reg(rs)} * (uint64_t){reg(rt)}; "
                        f"cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}")  # MULTU
            elif fn == 0x1A:
                line = (f"if ({reg(rt)}) {{ cpu->lo=(uint32_t)((int32_t){reg(rs)}/(int32_t){reg(rt)}); "
                        f"cpu->hi=(uint32_t)((int32_t){reg(rs)}%(int32_t){reg(rt)}); }}")  # DIV
            elif fn == 0x1B:
                line = (f"if ({reg(rt)}) {{ cpu->lo={reg(rs)}/{reg(rt)}; "
                        f"cpu->hi={reg(rs)}%{reg(rt)}; }}")  # DIVU
            elif fn == 0x20: line = f"{reg(rd)} = (uint32_t)((int32_t){reg(rs)}+(int32_t){reg(rt)});"  # ADD
            elif fn == 0x21: line = f"{reg(rd)} = {reg(rs)} + {reg(rt)};"   # ADDU
            elif fn == 0x22: line = f"{reg(rd)} = (uint32_t)((int32_t){reg(rs)}-(int32_t){reg(rt)});"  # SUB
            elif fn == 0x23: line = f"{reg(rd)} = {reg(rs)} - {reg(rt)};"   # SUBU
            elif fn == 0x24: line = f"{reg(rd)} = {reg(rs)} & {reg(rt)};"   # AND
            elif fn == 0x25: line = f"{reg(rd)} = {reg(rs)} | {reg(rt)};"   # OR
            elif fn == 0x26: line = f"{reg(rd)} = {reg(rs)} ^ {reg(rt)};"   # XOR
            elif fn == 0x27: line = f"{reg(rd)} = ~({reg(rs)} | {reg(rt)});"  # NOR
            elif fn == 0x2A: line = f"{reg(rd)} = ((int32_t){reg(rs)} < (int32_t){reg(rt)}) ? 1 : 0;"  # SLT
            elif fn == 0x2B: line = f"{reg(rd)} = ({reg(rs)} < {reg(rt)}) ? 1 : 0;"  # SLTU
            elif fn == 0x16: line = f"{reg(rd)} = __builtin_clz({reg(rs)});"  # CLZ
            elif fn == 0x17: line = f"{reg(rd)} = __builtin_ctz({reg(rs)});"  # CLO (aproximado)
            elif fn == 0x28: line = f"{reg(rd)} = ({reg(rs)} == 0) ? {reg(rt)} : {reg(rd)};"  # MOVZ
            elif fn == 0x29: line = f"{reg(rd)} = ({reg(rs)} != 0) ? {reg(rt)} : {reg(rd)};"  # MOVN
            elif fn == 0x2C: line = f"{{ int64_t _m=(int64_t)(int32_t){reg(rs)}*(int64_t)(int32_t){reg(rt)}; {reg(rd)}=(uint32_t)(_m>>32); }}"  # MADD (simplificado)

        elif op == 0x01:  # REGIMM
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            if   rt == 0x00: line = f"if ((int32_t){reg(rs)} < 0) {{ /* DS */ goto loc_{offset:08X}; }}"   # BLTZ
            elif rt == 0x01: line = f"if ((int32_t){reg(rs)} >= 0) {{ /* DS */ goto loc_{offset:08X}; }}"  # BGEZ
            elif rt == 0x10: # BLTZAL
                instr.is_call = True
                line = f"if ((int32_t){reg(rs)} < 0) {{ cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{offset:08X}; }}"
            elif rt == 0x11: # BGEZAL
                instr.is_call = True
                line = f"if ((int32_t){reg(rs)} >= 0) {{ cpu->ra=0x{addr+8:08X}u; /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x02:  # J
            dest = self._load_base + (instr.target26 << 2)
            instr.is_jump   = True
            instr.has_delay = True
            if dest in self._func_starts:
                line = f"/* DS */ func_{dest:08X}(cpu, mem); return;"
            else:
                line = f"/* DS */ goto loc_{dest:08X};"

        elif op == 0x03:  # JAL
            dest = self._load_base + (instr.target26 << 2)
            instr.is_call   = True
            instr.has_delay = True
            line = f"cpu->ra=0x{addr+8:08X}u; /* DS */ func_{dest:08X}(cpu, mem);"

        elif op == 0x04:  # BEQ
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            if rs == 0 and rt == 0:
                line = f"/* DS */ goto loc_{offset:08X};"
            else:
                if offset in self._func_starts:
                    line = f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ func_{offset:08X}(cpu, mem); return; }}"
                else:
                    line = f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ goto loc_{offset:08X}; }}"
        elif op == 0x14:  # BEQL (Branch on Equal Likely)
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            if rs == 0 and rt == 0:
                line = f"/* DS likely */ goto loc_{offset:08X};"
            else:
                if offset in self._func_starts:
                    line = f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ func_{offset:08X}(cpu, mem); return; }}"
                else:
                    line = f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x05:  # BNE
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            line = f"if ({reg(rs)} != {reg(rt)}) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x06:  # BLEZ
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            line = f"if ((int32_t){reg(rs)} <= 0) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x07:  # BGTZ
            offset = addr + 4 + (imms << 2)
            instr.is_branch = True
            instr.has_delay = True
            line = f"if ((int32_t){reg(rs)} > 0) {{ /* DS */ goto loc_{offset:08X}; }}"

        elif op == 0x08: line = f"{reg(rt)} = (uint32_t)((int32_t){reg(rs)}+{imms});"  # ADDI
        elif op == 0x09: line = f"{reg(rt)} = {reg(rs)} + (uint32_t){imms & 0xFFFFFFFF}u;"  # ADDIU
        elif op == 0x0A: line = f"{reg(rt)} = ((int32_t){reg(rs)} < {imms}) ? 1 : 0;"  # SLTI
        elif op == 0x0B: line = f"{reg(rt)} = ({reg(rs)} < {imm}u) ? 1 : 0;"           # SLTIU
        elif op == 0x0C: line = f"{reg(rt)} = {reg(rs)} & 0x{imm:04X}u;"               # ANDI
        elif op == 0x0D: line = f"{reg(rt)} = {reg(rs)} | 0x{imm:04X}u;"               # ORI
        elif op == 0x0E: line = f"{reg(rt)} = {reg(rs)} ^ 0x{imm:04X}u;"               # XORI
        elif op == 0x0F: line = f"{reg(rt)} = 0x{imm:04X}u << 16;"                     # LUI

        elif op == 0x20: line = f"{reg(rt)} = (uint32_t)(int8_t)MEM_R8(mem,{reg(rs)}+{imms});"    # LB
        elif op == 0x21: line = f"{reg(rt)} = (uint32_t)(int16_t)MEM_R16(mem,{reg(rs)}+{imms});"  # LH
        elif op == 0x22: line = f"/* LWL {reg(rt)}, {imms}({reg(rs)}) - stub */"
        elif op == 0x23: line = f"{reg(rt)} = MEM_R32(mem,{reg(rs)}+{imms});"                     # LW
        elif op == 0x24: line = f"{reg(rt)} = (uint32_t)MEM_R8(mem,{reg(rs)}+{imms});"            # LBU
        elif op == 0x25: line = f"{reg(rt)} = (uint32_t)MEM_R16(mem,{reg(rs)}+{imms});"           # LHU
        elif op == 0x26: line = f"/* LWR {reg(rt)}, {imms}({reg(rs)}) - stub */"

        elif op == 0x28: line = f"MEM_W8(mem,{reg(rs)}+{imms},(uint8_t){reg(rt)});"    # SB
        elif op == 0x29: line = f"MEM_W16(mem,{reg(rs)}+{imms},(uint16_t){reg(rt)});"  # SH
        elif op == 0x2A: line = f"/* SWL {reg(rt)}, {imms}({reg(rs)}) - stub */"
        elif op == 0x2B: line = f"MEM_W32(mem,{reg(rs)}+{imms},{reg(rt)});"            # SW
        elif op == 0x2E: line = f"/* SWR {reg(rt)}, {imms}({reg(rs)}) - stub */"

        elif op in (0x11, 0x12, 0x13, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
                    0x31, 0x35, 0x39, 0x3D):
            line = f"/* FPU/VFPU 0x{raw:08X} @ 0x{addr:08X} - stub */"

        else:
            line = f"/* UNKNOWN op=0x{op:02X} 0x{raw:08X} @ 0x{addr:08X} */"

        instr.c_line = line

    # ── Coleta instruções de uma função ────────────────────────────────────
    def _collect_instrs(self, start_addr):
        addr = start_addr
        next_funcs = sorted(a for a in self._func_starts if a > start_addr)
        stop = next_funcs[0] if next_funcs else start_addr + MAX_FUNC_LINES * 4
        stop = min(stop, start_addr + MAX_FUNC_LINES * 4)

        instrs = []
        while addr < stop:
            raw = self._read32(addr)
            if raw is None:
                break
            instr = Instr(addr, raw)
            self._decode(instr)
            instrs.append(instr)
            addr += 4
        return instrs

    # ── Gera o corpo estruturado e processa Delay Slots ─────────────────────
    def _format_function_body(self, start_addr, instrs):
        branch_targets = set()
        for instr in instrs:
            if instr.is_branch or instr.is_jump:
                m = re.search(r'goto loc_([0-9A-Fa-f]{8})', instr.c_line)
                if m:
                    target = int(m.group(1), 16)
                    if any(i.addr == target for i in instrs):
                        branch_targets.add(target)

        lines = []
        skip_next = False
        instr_addrs = {i.addr for i in instrs}

        for i, instr in enumerate(instrs):
            addr = instr.addr

            if skip_next:
                lines.append(f"  /* 0x{addr:08X}: delay slot (emitido acima) */")
                skip_next = False
                continue

            if addr in branch_targets or addr == start_addr:
                lines.append(f" loc_{addr:08X}:;")

            ds_line = "/* nop */"
            if instr.has_delay:
                if i + 1 < len(instrs):
                    ds_instr = instrs[i + 1]
                    ds_raw_instr = Instr(ds_instr.addr, ds_instr.raw)
                    self._decode(ds_raw_instr)
                    ds_line = ds_raw_instr.c_line if ds_raw_instr.c_line else "/* nop */"
                    skip_next = True

            line = instr.c_line or ""

            # Transforma goto para fora da função em chamadas reais e return
            m = re.search(r'goto loc_([0-9A-Fa-f]{8})', line)
            if m:
                target = int(m.group(1), 16)
                inside = target in instr_addrs
                if not inside:
                    line = re.sub(
                        r'goto loc_([0-9A-Fa-f]{8});?',
                        lambda m: f'func_{m.group(1).upper()}(cpu, mem); return;',
                        line
                    )

            # Injeção confiável do Delay Slot
            if "/* DS */" in line:
                branch_line = line.replace("/* DS */", ds_line, 1)
            else:
                branch_line = line

            branch_line = branch_line.strip()

            # Descartar linhas de código inválido, dados ou chamadas para funções inexistentes
            is_invalid = "UNKNOWN" in branch_line or addr > 0x0A000000

            # 1. Identifica se a linha tem referências a funções ou labels
            func_refs = re.findall(r'func_([0-9A-Fa-f]{8})', branch_line)
            loc_refs = re.findall(r'loc_([0-9A-Fa-f]{8})', branch_line)
            
            # 2. Verifica se as funções chamadas realmente foram descobertas no Passe 1
            if not is_invalid:
                for f_addr_hex in func_refs:
                    f_addr = int(f_addr_hex, 16)
                    if f_addr not in self._func_starts:
                        is_invalid = True
                        break

            # 3. Verifica se as labels de destino existem DENTRO desta função específica
            if not is_invalid:
                for l_addr_hex in loc_refs:
                    l_addr = int(l_addr_hex, 16)
                    if l_addr not in instr_addrs:
                        is_invalid = True
                        break

            # 4. Se for inválido, comenta a linha inteira para o GCC não tentar compilar
            if is_invalid:
                clean_line = branch_line.replace("/*", "").replace("*/", "").strip()
                branch_line = f"/* Skip invalid/data @ 0x{addr:08X}: {clean_line} */"

            # Sanitização robusta: garantir sintaxe C correta
            # 1. Garantir que 'return' sempre tem ';'
            branch_line = re.sub(r'\breturn\b(?!\s*;)', 'return;', branch_line)
            
            # 2. Remover `;` duplicados
            branch_line = re.sub(r';\s*;', ';', branch_line)
            
            # 3. Garantir `;` antes de `}`
            branch_line = re.sub(r'([^;{}\s])\s*}', r'\1; }', branch_line)
            
            # 4. Limpar espaços extras
            branch_line = re.sub(r'\s+', ' ', branch_line)

            lines.append(f"  {branch_line}  /* 0x{raw_hex(instr.raw)} */")

        return lines

    # ── Agrupa funções em arquivos em lote ─────────────────────────────────
    def _emit_group(self, group_id, funcs):
        path = os.path.join(FUNCS_DIR, f"funcs_{group_id:03d}.c")

        lines = []
        lines.append('#include "../../runtime/cpu.h"')
        lines.append('#include "../../runtime/memory.h"')
        lines.append('#include "../func_table.h"\n')

        for start_addr, instrs in funcs:
            fname = f"func_{start_addr:08X}"
            lines.append(f"\n/* ===== {fname} ===== */")
            lines.append(f"void {fname}(MIPS_CPU *cpu, uint8_t *mem) {{")
            
            # Incorpora as instruções processadas de _format_function_body
            body_lines = self._format_function_body(start_addr, instrs)
            lines.extend(body_lines)

            lines.append("}")

        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    # ── Gera tabela de funções (Sólido) ────────────────────────────────────
    def _emit_table(self, func_entries):
        # ── HEADER (.h)
        h = [
            "#pragma once",
            '#include "runtime/cpu.h"',
            "",
            "typedef void (*PSPFunc)(MIPS_CPU *cpu, uint8_t *mem);",
            "",
            "extern uint32_t psp_func_addrs[];"
        ]

        for addr, name in func_entries:
            h.append(f"void {name}(MIPS_CPU *cpu, uint8_t *mem);")

        h += [
            "",
            "extern PSPFunc psp_func_table[];",
            f"#define PSP_FUNC_COUNT {len(func_entries)}"
        ]

        with open(os.path.join(OUT_DIR, "func_table.h"), "w") as f:
            f.write("\n".join(h))

        # ── SOURCE (.c)
        c = [
            '#include "func_table.h"',
            "",
            "PSPFunc psp_func_table[] = {"
        ]

        for addr, name in func_entries:
            c.append(f"    {name},")

        c.append("};")
        c.append("")

        c.append("uint32_t psp_func_addrs[] = {")
        for addr, name in func_entries:
            c.append(f"    0x{addr:08X},")
        c.append("};")

        with open(os.path.join(OUT_DIR, "func_table.c"), "w") as f:
            f.write("\n".join(c))

    # ── Pipeline principal (Ordem Corrigida) ───────────────────────────────
    def run(self):
        # 1. Executar descoberta primeiro
        self._discover_functions()

        group_size = 100
        group = []
        group_id = 0
        
        # 2. Tuplas em vez de array solto garantem que as tabelas extraiam os bytes corretos
        func_entries = []

        for i, start in enumerate(sorted(self._func_starts)):
            instrs = self._collect_instrs(start)
            if not instrs:
                continue

            group.append((start, instrs))
            name = f"func_{start:08X}"
            func_entries.append((start, name))

            if len(group) >= group_size:
                self._emit_group(group_id, group)
                group = []
                group_id += 1

        if group:
            self._emit_group(group_id, group)

        # 3. Processar tabela por último
        self._emit_table(func_entries)

if __name__ == "__main__":
    t = MIPStoC()
    t.run()