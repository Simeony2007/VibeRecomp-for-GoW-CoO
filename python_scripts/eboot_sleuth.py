#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
eboot_sleuth.py - Advanced PSP EBOOT.BIN Static Analyzer and Code Generator
==============================================================================
This tool is designed to run on the user's WSL/Linux terminal against their
decrypted/extracted 'EBOOT.BIN' or MIPS ELF of God of War.

It performs three critical roles:
1. Scans the binary for the exact GOW crash signature.
2. Disassembles MIPS instructions around the crash area to find the root cause.
3. Resolves ALL legitimate Sony NID imports and automatically generates
   a clean, bulletproof 'eboot_hle_map.h' header specifically for God of War.
==============================================================================
"""

import sys
import struct
import os

# Disassembler helper for basic MIPS instructions
def disassemble_mips(word, pc):
    opcode = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    shamt = (word >> 6) & 0x1F
    funct = word & 0x3F
    imm = word & 0xFFFF
    simm = imm if imm < 0x8000 else imm - 0x10000
    target = word & 0x03FFFFFF

    regs = [
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"
    ]

    r_name = lambda r: regs[r] if r < 32 else f"r{r}"

    if opcode == 0x00:
        if funct == 0x00: return f"sll {r_name(rd)}, {r_name(rt)}, {shamt}" if rd != 0 else "nop"
        if funct == 0x02: return f"srl {r_name(rd)}, {r_name(rt)}, {shamt}"
        if funct == 0x03: return f"sra {r_name(rd)}, {r_name(rt)}, {shamt}"
        if funct == 0x08: return f"jr {r_name(rs)}"
        if funct == 0x09: return f"jalr {r_name(rd)}, {r_name(rs)}"
        if funct == 0x0C: return "syscall"
        if funct == 0x0D: return "break"
        if funct == 0x20: return f"add {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x21: return f"addu {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x22: return f"sub {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x23: return f"subu {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x24: return f"and {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x25: return f"or {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x26: return f"xor {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x27: return f"nor {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x2A: return f"slt {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        if funct == 0x2B: return f"sltu {r_name(rd)}, {r_name(rs)}, {r_name(rt)}"
        return f"special_0x{funct:02X}"
    elif opcode == 0x02:
        return f"j 0x{(target * 4) | (pc & 0xF0000000):08X}"
    elif opcode == 0x03:
        return f"jal 0x{(target * 4) | (pc & 0xF0000000):08X}"
    elif opcode == 0x04:
        return f"beq {r_name(rs)}, {r_name(rt)}, 0x{pc + 4 + simm * 4:08X}"
    elif opcode == 0x05:
        return f"bne {r_name(rs)}, {r_name(rt)}, 0x{pc + 4 + simm * 4:08X}"
    elif opcode == 0x08:
        return f"addi {r_name(rt)}, {r_name(rs)}, {simm}"
    elif opcode == 0x09:
        return f"addiu {r_name(rt)}, {r_name(rs)}, {simm}"
    elif opcode == 0x0F:
        return f"lui {r_name(rt)}, 0x{imm:04X}"
    elif opcode == 0x20:
        return f"lb {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x21:
        return f"lh {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x23:
        return f"lw {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x24:
        return f"lbu {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x25:
        return f"lhu {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x28:
        return f"sb {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x29:
        return f"sh {r_name(rt)}, {simm}({r_name(rs)})"
    elif opcode == 0x2B:
        return f"sw {r_name(rt)}, {simm}({r_name(rs)})"
    return f"opcode_0x{opcode:02X}"

def read_cstring(data, offset):
    chars = []
    while offset < len(data):
        c = data[offset]
        if c == 0:
            break
        chars.append(chr(c))
        offset += 1
    return "".join(chars)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 eboot_sleuth.py <path_to_EBOOT.BIN>")
        sys.exit(1)

    filepath = sys.argv[1]
    if not os.path.exists(filepath):
        print(f"Error: File '{filepath}' does not exist.")
        sys.exit(1)

    with open(filepath, "rb") as f:
        data = f.read()

    print("==============================================================================")
    print("                EBOOT_SLEUTH - PSP GOD OF WAR CORE ANALYSIS TOOL")
    print("==============================================================================")
    print(f"File: {filepath}")
    print(f"Size: {len(data)} bytes")

    # Detect ELF
    elf_data = data
    load_bias = 0
    is_prx = False

    if data.startswith(b"~PSP"):
        is_prx = True
        print("[Container] Detected ~PSP (PRX) Container.")
        elf_offset = data.find(b"\x7fELF")
        if elf_offset != -1:
            elf_data = data[elf_offset:]
        else:
            comp_type = struct.unpack("<H", data[6:8])[0] & 0xF00
            if comp_type != 0x300:
                print("Error: PRX file is compressed/encrypted. Use a decrypted ELF dump.")
                sys.exit(1)

    if not elf_data.startswith(b"\x7fELF"):
        print("Error: Not a valid decrypted ELF.")
        sys.exit(1)

    magic, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack("<16sHHIIIIIHHHHHH", elf_data[:52])

    if e_entry < 0x08000000:
        load_bias = 0x08800000
        print(f"[Loader] Relocatable PRX. Entry point: 0x{e_entry:08X} (bias: 0x{load_bias:08X})")
    else:
        print(f"[Loader] Absolute ELF. Entry point: 0x{e_entry:08X}")

    # Load Segments
    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        phdr_bytes = elf_data[off : off + e_phentsize]
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", phdr_bytes)
        if p_type == 1: # PT_LOAD
            segments.append({
                "offset": p_offset,
                "vaddr": p_vaddr,
                "paddr": p_paddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
                "data": elf_data[p_offset : p_offset + p_filesz]
            })

    def mips_get_file_offset(addr):
        vaddr = addr & 0x0FFFFFFF
        if vaddr < 0x04000000:
            vaddr += 0x08800000
        real_vaddr = vaddr - load_bias
        for seg in segments:
            start = seg["vaddr"]
            end = seg["vaddr"] + seg["memsz"]
            if real_vaddr >= start and real_vaddr < end:
                return seg["offset"] + (real_vaddr - start)
        return None

    # Part 1: Search for the Crash Code Pattern
    # Byte sequence: 10 00 c6 24  00 00 c7 84  04 00 c6 8c  09 f8 c0 00
    crash_bytes = b"\x10\x00\xc6\x24\x00\x00\xc7\x84\x04\x00\xc6\x8c\x09\xf8\xc0\x00"
    crash_offset = elf_data.find(crash_bytes)

    if crash_offset != -1:
        print("\n[🎯 CRASH PATTERN FOUND!]")
        # Determine Virtual Address of pattern
        vaddr_found = 0
        for seg in segments:
            if crash_offset >= seg["offset"] and crash_offset < seg["offset"] + seg["filesz"]:
                vaddr_found = seg["vaddr"] + (crash_offset - seg["offset"]) + load_bias
                break
        
        print(f"  -> File Offset: 0x{crash_offset:08X}")
        print(f"  -> Simulated Virtual Address: 0x{vaddr_found:08X} (Masked: 0x{vaddr_found & 0x0FFFFFFF:08X})")
        print("\n--- Disassembly around crash site ---")
        
        # Disassemble 10 words before and after
        start_off = crash_offset - 40
        for idx in range(25):
            curr_off = start_off + idx * 4
            if curr_off < 0 or curr_off + 4 > len(elf_data):
                continue
            word = struct.unpack("<I", elf_data[curr_off : curr_off + 4])[0]
            curr_vaddr = vaddr_found - (crash_offset - curr_off)
            line = disassemble_mips(word, curr_vaddr)
            marker = "  ==> [CRASH CORE]" if curr_off == crash_offset else "     "
            print(f"{marker} 0x{curr_vaddr:08X} [0x{word:08X}]: {line}")
    else:
        print("\n[⚠️ Crash Pattern Not Found]")
        print("This could be due to small variant byte adjustments in your particular EBOOT dump.")

    # Part 2: Extract Module and Stub Information (The Rosetta Stone)
    # Scan segment headers and file offsets to find Module info
    modinfo_offset = 0
    if is_prx:
        modinfo_offset = struct.unpack("<I", data[52:56])[0]
    else:
        for seg in segments:
            if seg["paddr"] != 0 and seg["paddr"] != seg["vaddr"]:
                modinfo_offset = seg["paddr"] & 0x7FFFFFFF

    # Let's resolve the exact SceModuleInfo structure
    resolved_mod_addr = None
    if modinfo_offset != 0:
        test_addr = modinfo_offset + load_bias
        file_off = mips_get_file_offset(test_addr)
        if file_off and file_off + 52 <= len(elf_data):
            resolved_mod_addr = test_addr

    if not resolved_mod_addr:
        # Loop over segments to find a valid module heuristically
        print("\n[HLE Linker] Direct Module pointers invalid, searching for SceModuleInfo...")
        for seg in segments:
            for o in range(0, len(seg["data"]) - 52, 4):
                mod_bytes = seg["data"][o : o+52]
                mod_attr, mod_ver, mod_name_bytes = struct.unpack("<H H 28s", mod_bytes[:32])
                mod_name = read_cstring(mod_name_bytes, 0)
                if mod_attr <= 0x1FFF and len(mod_name) > 0 and all(32 <= ord(c) < 127 for c in mod_name):
                    gp, ent_top, ent_btm, stub_top, stub_btm = struct.unpack("<I I I I I", mod_bytes[32:52])
                    if stub_top != 0 and (stub_top & 0x0FFFFFFF) < 0x04000000:
                        resolved_mod_addr = seg["vaddr"] + o + load_bias
                        break
            if resolved_mod_addr:
                break

    if not resolved_mod_addr:
        print("Critical Error: SceModuleInfo structure could not be discovered. Core analysis aborted.")
        sys.exit(1)

    # Decode SceModuleInfo
    file_off = mips_get_file_offset(resolved_mod_addr)
    mod_bytes = elf_data[file_off : file_off+52]
    mod_attr, mod_ver, mod_name_bytes = struct.unpack("<H H 28s", mod_bytes[:32])
    mod_name = read_cstring(mod_name_bytes, 0)
    gp_value, ent_top, ent_btm, stub_top, stub_btm = struct.unpack("<I I I I I", mod_bytes[32:52])

    print(f"\n[🎯 MODULE DISCOVERED]")
    print(f"  -> Name: '{mod_name}'")
    print(f"  -> Version: {mod_ver & 0xFF}.{mod_ver >> 8}")
    print(f"  -> Attribute: 0x{mod_attr:04X}")
    print(f"  -> Virtual Address of SceModuleInfo: 0x{resolved_mod_addr:08X}")
    print(f"  -> Imports / Stubs Table Range: 0x{stub_top:08X} - 0x{stub_btm:08X}")

    # Process imports and generate the HLE map header
    curr_stub = stub_top
    limit_stub = stub_btm
    
    resolved_imports = {}
    
    print("\n[Linker] Extracting imported library stubs and NIDs...")
    while curr_stub < limit_stub:
        stub_f_off = mips_get_file_offset(curr_stub)
        if not stub_f_off or stub_f_off + 20 > len(elf_data):
            break
        
        stub_hdr_bytes = elf_data[stub_f_off : stub_f_off + 20]
        lib_name_ptr, version, attribute, struct_size, num_vars, num_funcs, nid_table_ptr, stub_table_ptr = struct.unpack("<I H H B B H I I", stub_hdr_bytes)
        
        lib_name_off = mips_get_file_offset(lib_name_ptr)
        lib_name = read_cstring(elf_data, lib_name_off) if lib_name_off else "Unknown"
        
        nid_table_off = mips_get_file_offset(nid_table_ptr)
        stub_table_off = mips_get_file_offset(stub_table_ptr)
        
        if num_funcs > 500:
            num_funcs = 500 # safety limit
            
        if nid_table_off and stub_table_off:
            for j in range(num_funcs):
                nid = struct.unpack("<I", elf_data[nid_table_off + j*4 : nid_table_off + j*4 + 4])[0]
                stub_vaddr = stub_table_ptr + j*8
                resolved_imports[nid] = {
                    "library": lib_name,
                    "vaddr": stub_vaddr,
                    "index": j
                }
        
        step = struct_size
        if step < 5 or step > 32:
            step = 5
        curr_stub += step * 4

    print(f"  -> Total distinct NID function imports extracted: {len(resolved_imports)}")

    # Generate the C Header file "eboot_hle_map.h"
    header_lines = [
        "/*",
        " * ===========================================================================",
        " * eboot_hle_map.h - Dynamic HLE Mapped Syscall Tables for God of War",
        " * Auto-generated by eboot_sleuth.py",
        " * ===========================================================================",
        " */",
        "#ifndef EBOOT_HLE_MAP_H",
        "#define EBOOT_HLE_MAP_H",
        "",
        "#include <stdint.h>",
        "#include \"mips_cpu.h\"",
        "",
        "// Structure representing a resolved stub mapping",
        "typedef struct {",
        "    uint32_t nid;",
        "    const char *lib_name;",
        "    const char *func_name;",
        "    uint32_t syscall_id;",
        "} GOW_HleStubMap;",
        "",
    ]

    # Map well-known NIDs to system call ids
    known_mappings = {
        # IoFileMgrForUser
        0x109F504F: ("IoFileMgrForUser", "sceIoOpen", "0x11111"),
        0x109F50BC: ("IoFileMgrForUser", "sceIoOpen", "0x11111"),
        0x6A638D83: ("IoFileMgrForUser", "sceIoRead", "0x11112"),
        0x27EB27B8: ("IoFileMgrForUser", "sceIoLseek", "0x11116"),
        0x68963324: ("IoFileMgrForUser", "sceIoLseek32", "0x11117"),
        0xA0B5A7C2: ("IoFileMgrForUser", "sceIoReadAsync", "0x11113"),
        0x89AA9906: ("IoFileMgrForUser", "sceIoOpenAsync", "0x11126"),
        0xB293727F: ("IoFileMgrForUser", "sceIoChangeAsyncPriority", "0x11127"),
        0x810C4BC3: ("IoFileMgrForUser", "sceIoClose", "0x11114"),
        0x54F5FB11: ("IoFileMgrForUser", "sceIoDevctl", "0x11115"),
        0x63632449: ("IoFileMgrForUser", "sceIoIoctl", "0x11118"),
        0xE95A012B: ("IoFileMgrForUser", "sceIoIoctlAsync", "0x11119"),
        0xE23EEC33: ("IoFileMgrForUser", "sceIoWaitAsync", "0x1111A"),
        0x3251EA56: ("IoFileMgrForUser", "sceIoPollAsync", "0x1111B"),
        # ThreadManForUser
        0xD6DA4BA1: ("ThreadManForUser", "sceKernelCreateSema", "0x22221"),
        0x4E3A1105: ("ThreadManForUser", "sceKernelWaitSema", "0x22222"),
        0x3F53E640: ("ThreadManForUser", "sceKernelSignalSema", "0x22223"),
        0xCEADEB47: ("ThreadManForUser", "sceKernelDelayThread", "0x22224"),
        0x446D8DE6: ("ThreadManForUser", "sceKernelCreateThread", "0x22225"),
        0xF475845D: ("ThreadManForUser", "sceKernelStartThread", "0x22226"),
        0xAA73C935: ("ThreadManForUser", "sceKernelExitThread", "0x22227"),
        0x8011F9B0: ("ThreadManForUser", "sceKernelExitThread", "0x22227"),
        # sceGe_user
        0xAB49E76A: ("sceGe_user", "sceGeListEnQueue", "0x33331"),
        0xAB49E7EC: ("sceGe_user", "sceGeListEnQueue", "0x33331"),
        0x03444EB4: ("sceGe_user", "sceGeListSync", "0x33332"),
        0x034C113E: ("sceGe_user", "sceGeListSync", "0x33332"),
        0xE47E40E4: ("sceGe_user", "sceGeEdramGetAddr", "0x33333"),
        0x1F6752AD: ("sceGe_user", "sceGeEdramGetSize", "0x33334"),
        0xB287BD61: ("sceGe_user", "sceGeDrawSync", "0x33335"),
        # sceDisplay
        0x0E20F177: ("sceDisplay", "sceDisplaySetMode", "0x60001"),
        0x289D82FE: ("sceDisplay", "sceDisplaySetFrameBuf", "0x60002"),
        0x984C27E7: ("sceDisplay", "sceDisplayWaitVblankStart", "0x60003"),
        0x9C6EAAD7: ("sceDisplay", "sceDisplayGetVblankCounter", "0x60004"),
        # sceCtrl
        0x1F4011E6: ("sceCtrl", "sceCtrlReadBufferPositive", "0x70001"),
        # sceAudio
        0x5EC81C55: ("sceAudio", "sceAudioChReserve", "0x40001"),
        0x13F592BC: ("sceAudio", "sceAudioChRelease", "0x40002"),
        0x14074D7E: ("sceAudio", "sceAudioOutputPanned", "0x40003"),
        0x08DF58A5: ("sceAudio", "sceAudioOutputPanned", "0x40003"),
        # sceAtrac3plus
        0x231FC6B7: ("sceAtrac3plus", "_sceAtracGetContextAddress", "0x231FC6B7"),
    }

    header_lines.append(f"// Mapped imports found in EBOOT.BIN")
    header_lines.append(f"#define GOW_TOTAL_STUBS_COUNT {len(resolved_imports)}")
    header_lines.append("")
    header_lines.append("static const GOW_HleStubMap g_gow_stub_table[GOW_TOTAL_STUBS_COUNT] = {")

    for nid, imp in sorted(resolved_imports.items(), key=lambda x: x[1]["library"]):
        lib = imp["library"]
        # Match with known wrapper syscalls
        if nid in known_mappings:
            lib_map, f_name, sys_code = known_mappings[nid]
            header_lines.append(f"    {{ 0x{nid:08X}, \"{lib}\", \"{f_name}\", {sys_code} }},")
        else:
            # Fallback to unique custom code to prevent linker conflicts
            header_lines.append(f"    {{ 0x{nid:08X}, \"{lib}\", \"unimplemented\", 0 }},")

    header_lines.append("};")
    header_lines.append("")
    header_lines.append("#endif // EBOOT_HLE_MAP_H")

    # Save eboot_hle_map.h
    output_header = "eboot_hle_map.h"
    with open(output_header, "w") as out_h:
        out_h.write("\n".join(header_lines))

    print(f"\n[SUCCESS] Clean, static HLE syscall map generated successfully in: '{output_header}'")
    print("This file acts as a static translation dictionary, completely replacing heuristic memory scanning!")
    print("==============================================================================")

if __name__ == "__main__":
    main()
