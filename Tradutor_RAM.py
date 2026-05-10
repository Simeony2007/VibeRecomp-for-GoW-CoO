#!/usr/bin/env python3
import struct
import sys
import os

# Configurações de Ambiente
IN_FILE = "ram_dump.bin"
OUT_DIR = "out/funcs"
BASE_ADDR = 0x08800000
CODE_SIZE = 0x0034B3C0 
LINES_PER_FILE = 1000  # Evita erro 'Killed' do GCC no R36S

REG = ["zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
       "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"]

def reg(n): return "0" if n == 0 else f"cpu->{REG[n]}"
def reg_dest(n): return f"cpu->{REG[n]}"
def imm_s16(imm16): return imm16 if imm16 < 0x8000 else imm16 - 0x10000

class Instr:
    __slots__ = ("addr","raw","op","rs","rt","rd","shamt","funct","imm","imm_s","target26","c_line")
    def __init__(self, addr, raw):
        self.addr, self.raw = addr, raw
        self.op, self.rs, self.rt, self.rd = (raw >> 26) & 0x3F, (raw >> 21) & 0x1F, (raw >> 16) & 0x1F, (raw >> 11) & 0x1F
        self.shamt, self.funct, self.imm = (raw >> 6) & 0x1F, raw & 0x3F, raw & 0xFFFF
        self.imm_s, self.target26 = imm_s16(self.imm), raw & 0x03FFFFFF
        self.c_line = ""

def decode_instr(instr):
    op, rs, rt, rd, sa, fn = instr.op, instr.rs, instr.rt, instr.rd, instr.shamt, instr.funct
    addr, imms, raw = instr.addr, instr.imm_s, instr.raw
    
    # Ignora escritas no registrador ZERO (Reduz código e warnings)
    dest_reg = -1
    if op == 0x00: dest_reg = rd
    elif op in [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25]: dest_reg = rt
    if dest_reg == 0:
        instr.c_line = "/* nop */"
        return

    line = f"/* UNKNOWN 0x{raw:08X} */"

    # Lógica baseada no disasm_to_c.py
    if op == 0x00:
        if   fn == 0x00: line = "/* nop */" if raw == 0 else f"{reg_dest(rd)} = (uint32_t){reg(rt)} << {sa};"
        elif fn == 0x02: line = f"{reg_dest(rd)} = (uint32_t){reg(rt)} >> {sa};"
        elif fn == 0x03: line = f"{reg_dest(rd)} = (int32_t){reg(rt)} >> {sa};"
        elif fn == 0x04: line = f"{reg_dest(rd)} = (uint32_t){reg(rt)} << ({reg(rs)} & 31);"
        elif fn == 0x06: line = f"{reg_dest(rd)} = (uint32_t){reg(rt)} >> ({reg(rs)} & 31);"
        elif fn == 0x07: line = f"{reg_dest(rd)} = (int32_t){reg(rt)} >> ({reg(rs)} & 31);"
        elif fn == 0x08: line = f"/* DS */ cpu->pc = {reg(rs)}; return;"
        elif fn == 0x09: line = f"/* DS */ {reg_dest(rd)} = 0x{addr+8:08X}u; cpu->pc = {reg(rs)}; return;"
        elif fn == 0x0C: code = (raw >> 6) & 0xFFFFF; line = f"psp_syscall_mips(cpu, mem, 0x{code:05X}); cpu->pc = 0x{addr+4:08X}u; return;"
        elif fn == 0x10: line = f"{reg_dest(rd)} = cpu->hi;"
        elif fn == 0x11: line = f"cpu->hi = {reg(rs)};"
        elif fn == 0x12: line = f"{reg_dest(rd)} = cpu->lo;"
        elif fn == 0x13: line = f"cpu->lo = {reg(rs)};"
        elif fn == 0x18: line = f"{{ int64_t _m = (int64_t)(int32_t){reg(rs)} * (int64_t)(int32_t){reg(rt)}; cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}"
        elif fn == 0x19: line = f"{{ uint64_t _m = (uint64_t){reg(rs)} * (uint64_t){reg(rt)}; cpu->lo=(uint32_t)_m; cpu->hi=(uint32_t)(_m>>32); }}"
        elif fn == 0x1A: line = f"if ({reg(rt)}) {{ cpu->lo=(uint32_t)((int32_t){reg(rs)}/(int32_t){reg(rt)}); cpu->hi=(uint32_t)((int32_t){reg(rs)}%(int32_t){reg(rt)}); }}"
        elif fn == 0x1B: line = f"if ({reg(rt)}) {{ cpu->lo={reg(rs)}/{reg(rt)}; cpu->hi={reg(rs)}%{reg(rt)}; }}"
        elif fn == 0x21: line = f"{reg_dest(rd)} = {reg(rs)} + {reg(rt)};"
        elif fn == 0x23: line = f"{reg_dest(rd)} = {reg(rs)} - {reg(rt)};"
        elif fn == 0x24: line = f"{reg_dest(rd)} = {reg(rs)} & {reg(rt)};"
        elif fn == 0x25: line = f"{reg_dest(rd)} = {reg(rs)} | {reg(rt)};"
        elif fn == 0x26: line = f"{reg_dest(rd)} = {reg(rs)} ^ {reg(rt)};"
        elif fn == 0x27: line = f"{reg_dest(rd)} = ~({reg(rs)} | {reg(rt)});"
        elif fn == 0x2A: line = f"{reg_dest(rd)} = ((int32_t){reg(rs)} < (int32_t){reg(rt)}) ? 1 : 0;"
        elif fn == 0x2B: line = f"{reg_dest(rd)} = ({reg(rs)} < {reg(rt)}) ? 1 : 0;"

    elif op == 0x01:
        offset = addr + 4 + (imms << 2)
        cond = "< 0" if rt in (0x00, 0x02) else ">= 0"
        link = f"cpu->ra=0x{addr+8:08X}u; " if rt in (0x10, 0x11, 0x12, 0x13) else ""
        line = f"if ((int32_t){reg(rs)} {cond}) {{ {link}/* DS */ cpu->pc = 0x{offset:08X}u; return; }}"

    elif op in (0x02, 0x03):
        dest = (addr & 0xF0000000) | (instr.target26 << 2)
        link = f"cpu->ra = 0x{addr+8:08X}u; " if op == 0x03 else ""
        line = f"{link}/* DS */ cpu->pc = 0x{dest:08X}u; return;"

    elif op in (0x04, 0x14):
        offset = addr + 4 + (imms << 2)
        line = f"/* DS */ cpu->pc = 0x{offset:08X}u; return;" if rs == 0 and rt == 0 else f"if ({reg(rs)} == {reg(rt)}) {{ /* DS */ cpu->pc = 0x{offset:08X}u; return; }}"
    elif op in (0x05, 0x15):
        offset = addr + 4 + (imms << 2)
        line = f"if ({reg(rs)} != {reg(rt)}) {{ /* DS */ cpu->pc = 0x{offset:08X}u; return; }}"
    elif op in (0x06, 0x16):
        offset = addr + 4 + (imms << 2)
        line = f"if ((int32_t){reg(rs)} <= 0) {{ /* DS */ cpu->pc = 0x{offset:08X}u; return; }}"
    elif op in (0x07, 0x17):
        offset = addr + 4 + (imms << 2)
        line = f"if ((int32_t){reg(rs)} > 0) {{ /* DS */ cpu->pc = 0x{offset:08X}u; return; }}"

    elif op == 0x08: line = f"{reg_dest(rt)} = (uint32_t)((int32_t){reg(rs)} + {imms});"
    elif op == 0x09: line = f"{reg_dest(rt)} = {reg(rs)} + {imms};"
    elif op == 0x0A: line = f"{reg_dest(rt)} = ((int32_t){reg(rs)} < {imms}) ? 1 : 0;"
    elif op == 0x0B: line = f"{reg_dest(rt)} = ({reg(rs)} < (uint32_t){imms}) ? 1 : 0;"
    elif op == 0x0C: line = f"{reg_dest(rt)} = {reg(rs)} & 0x{instr.imm:04X}u;"
    elif op == 0x0D: line = f"{reg_dest(rt)} = {reg(rs)} | 0x{instr.imm:04X}u;"
    elif op == 0x0E: line = f"{reg_dest(rt)} = {reg(rs)} ^ 0x{instr.imm:04X}u;"
    elif op == 0x0F: line = f"{reg_dest(rt)} = 0x{instr.imm:04X}u << 16;"

    elif op == 0x20: line = f"{reg_dest(rt)} = (uint32_t)(int8_t)MEM_R8(mem,{reg(rs)}+{imms});"
    elif op == 0x21: line = f"{reg_dest(rt)} = (uint32_t)(int16_t)MEM_R16(mem,{reg(rs)}+{imms});"
    elif op == 0x22: line = f"{reg_dest(rt)} = psp_lwl(mem, {reg(rs)} + {imms}, {reg(rt)});"
    elif op == 0x23: line = f"{reg_dest(rt)} = MEM_R32(mem,{reg(rs)}+{imms});"
    elif op == 0x24: line = f"{reg_dest(rt)} = (uint32_t)MEM_R8(mem,{reg(rs)}+{imms});"
    elif op == 0x25: line = f"{reg_dest(rt)} = (uint32_t)MEM_R16(mem,{reg(rs)}+{imms});"
    elif op == 0x26: line = f"{reg_dest(rt)} = psp_lwr(mem, {reg(rs)} + {imms}, {reg(rt)});"
    elif op == 0x28: line = f"MEM_W8(mem,{reg(rs)}+{imms},(uint8_t){reg(rt)});"
    elif op == 0x29: line = f"MEM_W16(mem,{reg(rs)}+{imms},(uint16_t){reg(rt)});"
    elif op == 0x2A: line = f"psp_swl(mem, {reg(rs)} + {imms}, {reg(rt)});"
    elif op == 0x2B: line = f"MEM_W32(mem,{reg(rs)}+{imms},{reg(rt)});"
    elif op == 0x2E: line = f"psp_swr(mem, {reg(rs)} + {imms}, {reg(rt)});"
    
    instr.c_line = line

print("[*] Iniciando Tradutor RAM Dump Integrado...")
os.makedirs(OUT_DIR, exist_ok=True)
with open(IN_FILE, "rb") as f: ram_data = f.read()

all_instrs = []
vaddr = BASE_ADDR
for offset in range(0, CODE_SIZE, 4):
    raw = struct.unpack_from("<I", ram_data, offset)[0]
    ins = Instr(vaddr, raw); decode_instr(ins); all_instrs.append(ins); vaddr += 4

file_count, current_lines = 0, []

def flush_file():
    global file_count, current_lines
    if not current_lines: return
    path = os.path.join(OUT_DIR, f"funcs_{file_count:03d}.c")
    with open(path, "w") as fw:
        fw.write('#include "../../runtime/cpu.h"\\n#include "../../runtime/memory.h"\\n')
        fw.write('extern void psp_syscall_mips(MIPS_CPU *cpu, uint8_t *mem, uint32_t code);\\n\\n')
        fw.write(f"void run_block_{file_count:03d}(MIPS_CPU *cpu, uint8_t *mem) {{\\n  switch(cpu->pc) {{\\n")
        for addr, _ in current_lines: fw.write(f"    case 0x{addr:08X}: goto loc_{addr:08X};\\n")
        fw.write("  }\\n")
        for addr, line in current_lines: fw.write(f" loc_{addr:08X}:;\\n  {line}\\n")
        last_addr = current_lines[-1][0]
        fw.write(f"  cpu->pc = 0x{last_addr + 4:08X}u;\\n  return;\\n}}\\n")
    print(f" [+] Gerado {path}"); current_lines = []; file_count += 1

for i in range(len(all_instrs)):
    ins = all_instrs[i]; line = ins.c_line
    if "/* DS */" in line:
        ds_line = "/* nop */"
        if i + 1 < len(all_instrs):
            ds_line = all_instrs[i+1].c_line.replace("/* DS */", "").strip()
        line = line.replace("/* DS */", ds_line)
    current_lines.append((ins.addr, line))
    if len(current_lines) >= LINES_PER_FILE: flush_file()

flush_file()

# No final do arquivo (Geração do Dispatcher):
disp_path = "runtime/dispatcher.c"
with open(disp_path, "w") as f:
    f.write('#include "cpu.h"\n#include <stdio.h>\n\n')
    for i in range(file_count): 
        f.write(f'void run_block_{i:03d}(MIPS_CPU *cpu, uint8_t *mem);\n')
    f.write('\nvoid dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t pc) {\n')
    f.write('    uint32_t old_pc = cpu->pc;\n')
    for i in range(file_count): 
        f.write(f'    run_block_{i:03d}(cpu, mem); if (cpu->pc != old_pc) return;\n')
    f.write('\n    cpu->running = 0;\n}\n')