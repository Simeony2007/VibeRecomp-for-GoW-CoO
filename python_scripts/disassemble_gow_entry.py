#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
God of War Entry Point Disassembler - V1
This script reads the raw EBOOT.BIN file directly, detects the embedded ELF,
resolves the entry point virtual address, and disassembles the first 80 instructions
exactly as they are on disk before any runtime linking or emulator patching.
"""

import os
import sys
import struct

# Register names map
REG_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"
]

def decode_instruction(word, pc):
    opcode = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    shamt = (word >> 6) & 0x1F
    funct = word & 0x3F
    
    imm16 = word & 0xFFFF
    simm16 = imm16 - 0x10000 if imm16 >= 0x8000 else imm16
    target26 = word & 0x03FFFFFF
    
    reg_s = REG_NAMES[rs]
    reg_t = REG_NAMES[rt]
    reg_d = REG_NAMES[rd]

    # Special R-Type (Opcode 0)
    if opcode == 0:
        if word == 0:
            return "nop"
        if funct == 0x00:
            return f"sll {reg_d}, {reg_t}, {shamt}"
        elif funct == 0x02:
            return f"srl {reg_d}, {reg_t}, {shamt}"
        elif funct == 0x03:
            return f"sra {reg_d}, {reg_t}, {shamt}"
        elif funct == 0x08:
            return f"jr {reg_s}"
        elif funct == 0x09:
            return f"jalr {reg_s}" if rd == 31 else f"jalr {reg_d}, {reg_s}"
        elif funct == 0x0C:
            return f"syscall 0x{(word >> 6) & 0xFFFFF:05X}"
        elif funct == 0x0D:
            return "break"
        elif funct == 0x20:
            return f"add {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x21:
            return f"addu {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x22:
            return f"sub {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x23:
            return f"subu {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x24:
            return f"and {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x25:
            return f"or {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x26:
            return f"xor {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x27:
            return f"nor {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x2A:
            return f"slt {reg_d}, {reg_s}, {reg_t}"
        elif funct == 0x2B:
            return f"sltu {reg_d}, {reg_s}, {reg_t}"
        else:
            return f"unsupported r-type funct 0x{funct:02X}"

    # J-Type instructions
    elif opcode == 0x02:
        dest = (pc & 0xF0000000) | (target26 << 2)
        return f"j 0x{dest:08X}"
    elif opcode == 0x03:
        dest = (pc & 0xF0000000) | (target26 << 2)
        return f"jal 0x{dest:08X}"

    # I-Type and load/store
    elif opcode == 0x04:
        dest = pc + 4 + (simm16 << 2)
        return f"beq {reg_s}, {reg_t}, 0x{dest:08X}"
    elif opcode == 0x05:
        dest = pc + 4 + (simm16 << 2)
        return f"bne {reg_s}, {reg_t}, 0x{dest:08X}"
    elif opcode == 0x08:
        return f"addi {reg_t}, {reg_s}, {simm16}"
    elif opcode == 0x09:
        return f"addiu {reg_t}, {reg_s}, {simm16}"
    elif opcode == 0x0A:
        return f"slti {reg_t}, {reg_s}, {simm16}"
    elif opcode == 0x0B:
        return f"sltiu {reg_t}, {reg_s}, {simm16}"
    elif opcode == 0x0C:
        return f"andi {reg_t}, {reg_s}, 0x{imm16:04X}"
    elif opcode == 0x0D:
        return f"ori {reg_t}, {reg_s}, 0x{imm16:04X}"
    elif opcode == 0x0E:
        return f"xori {reg_t}, {reg_s}, 0x{imm16:04X}"
    elif opcode == 0x0F:
        return f"lui {reg_t}, 0x{imm16:04X}"
    elif opcode == 0x20:
        return f"lb {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x21:
        return f"lh {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x23:
        return f"lw {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x24:
        return f"lbu {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x25:
        return f"lhu {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x28:
        return f"sb {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x29:
        return f"sh {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x2B:
        return f"sw {reg_t}, {simm16}({reg_s})"
    elif opcode == 0x2E:
        return f"swc2 {reg_t}, {simm16}({reg_s}) # VFPU"
    elif opcode == 0x32:
        return f"lwc2 {reg_t}, {simm16}({reg_s}) # VFPU"
    elif opcode == 0x14: # BEQL
        dest = pc + 4 + (simm16 << 2)
        return f"beql {reg_s}, {reg_t}, 0x{dest:08X}"
    elif opcode == 0x15: # BNEL
        dest = pc + 4 + (simm16 << 2)
        return f"bnel {reg_s}, {reg_t}, 0x{dest:08X}"
    else:
        return f"unsupported opcode 0x{opcode:02X}"

def main():
    print("======================================================================")
    print("     God of War Pure Binary Entry Point Disassembler V1")
    print("======================================================================")

    # Candidate file paths
    paths = [
        "umd0/PSP_GAME/SYSDIR/EBOOT.BIN",
        "umd0/PSP_GAME/SYSDIR/BOOT.BIN",
        "EBOOT.BIN",
        "BOOT.BIN"
    ]

    filepath = None
    for p in paths:
        if os.path.exists(p):
            filepath = p
            break

    if not filepath:
        print("[FAIL] Game executable file (EBOOT.BIN/BOOT.BIN) not found in current directory tree.")
        print("Please run this script from the main GOW directory where 'umd0/' resides.")
        sys.exit(1)

    print(f"[Init] Opened target binary: '{filepath}' ({os.path.getsize(filepath)} bytes)")

    with open(filepath, "rb") as f:
        data = f.read()

    elf_data = data
    if data.startswith(b"~PSP"):
        elf_offset = data.find(bytes([0x7F, ord('E'), ord('L'), ord('F')]))
        if elf_offset != -1:
            print(f"[Loader] Detected decrypted ~PSP container. Found embedded ELF32 at offset 0x{elf_offset:08X}")
            elf_data = data[elf_offset:]
        else:
            print("[FAIL] Embedded block inside ~PSP is encrypted or is not a valid ELF.")
            sys.exit(2)

    if not elf_data.startswith(bytes([0x7F, ord('E'), ord('L'), ord('F')])):
        print("[FAIL] Target is not a valid MIPS ELF executable.")
        sys.exit(3)

    # Parse MIPS ELF32 headers
    e_entry = struct.unpack("<I", elf_data[24:28])[0]
    e_phoff = struct.unpack("<I", elf_data[28:32])[0]
    e_phnum = struct.unpack("<H", elf_data[44:46])[0]
    
    is_relocatable = e_entry < 0x08000000
    load_bias = 0x08800000 if is_relocatable else 0
    resolved_entry = e_entry + load_bias

    print(f"[Loader] Program Entry Point: 0x{resolved_entry:08X} (load_bias: 0x{load_bias:08X})")

    # Locate PT_LOAD segments to find where resolved_entry maps in file space
    file_offset = None
    for i in range(e_phnum):
        ph_offset = e_phoff + i * 32
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", elf_data[ph_offset:ph_offset+32])
        if p_type == 1: # PT_LOAD
            segment_start_vaddr = p_vaddr + load_bias
            segment_end_vaddr = segment_start_vaddr + p_filesz
            if segment_start_vaddr <= resolved_entry < segment_end_vaddr:
                relative_offset = resolved_entry - segment_start_vaddr
                file_offset = p_offset + relative_offset
                print(f"[Loader] Entry Point VAddr mapped to segment {i} file offset 0x{file_offset:08X}")
                break

    if file_offset is None:
        print("[FAIL] Failed to map program entry point virtual address to program segments.")
        sys.exit(4)

    # Dump and decode instructions
    num_instructions = 80
    print(f"\n--- Disassembling first {num_instructions} instructions at entry point ---")
    print(f"Address     | Hex Word   | Assembly")
    print("----------------------------------------------------------------------")

    for idx in range(num_instructions):
        curr_pc = resolved_entry + idx * 4
        curr_file_offset = file_offset + idx * 4
        if curr_file_offset + 4 > len(elf_data):
            print("[End of Segment]")
            break

        instruction_word = struct.unpack("<I", elf_data[curr_file_offset:curr_file_offset+4])[0]
        assembly = decode_instruction(instruction_word, curr_pc)
        print(f"0x{curr_pc:08X} | 0x{instruction_word:08x} | {assembly}")

    print("----------------------------------------------------------------------")
    print("Execution analysis completed successfully!")

if __name__ == "__main__":
    main()
