#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
validate_eboot.py - Decrypted EBOOT.BIN (PSP ELF/PRX) Parser and Validator
==============================================================================
This tool parses decrypted MIPS Allegrex EBOOT.BIN files of God of War,
identifying segment layouts, SceModuleInfo, Import Tables (stubs), and NIDs.
It generates a detailed report to help debug the HLE linker.
"""

import sys
import struct
import os

def read_cstring(data, offset):
    chars = []
    while offset < len(data):
        c = data[offset]
        if c == 0:
            break
        chars.append(chr(c))
        offset += 1
    return "".join(chars)

def parse_eboot(filepath, output_txt):
    if not os.path.exists(filepath):
        print(f"Error: Target file '{filepath}' does not exist.")
        return False

    with open(filepath, "rb") as f:
        data = f.read()

    report = []
    report.append("==============================================================================")
    report.append(f"            PSP EBOOT.BIN ANALYZER REPORT FOR GOD OF WAR EMULATOR")
    report.append("==============================================================================")
    report.append(f"File Path: {filepath}")
    report.append(f"File Size: {len(data)} bytes")

    if len(data) < 52:
        report.append("Error: File size too small to be a valid ELF or PRX.")
        save_report(report, output_txt)
        return False

    # 1. Detect ~PSP header (PRX)
    is_prx = False
    modinfo_offset_from_header = 0
    elf_data = data
    if data.startswith(b"~PSP"):
        is_prx = True
        report.append("\n[Container] Detected ~PSP (PRX) executable container.")
        modinfo_offset_from_header = struct.unpack("<I", data[52:56])[0]
        report.append(f"  -> SceModuleInfo offset from PRX Header: 0x{modinfo_offset_from_header:08X}")
        
        elf_offset = data.find(b"\x7fELF")
        if elf_offset != -1:
            report.append(f"  -> Found embedded MIPS ELF32 at offset 0x{elf_offset:X} inside PRX.")
            elf_data = data[elf_offset:]
        else:
            comp_type = struct.unpack("<H", data[6:8])[0] & 0xF00
            report.append(f"  -> Compression/Encryption Type: 0x{comp_type:03X}")
            if comp_type != 0x300:
                report.append("  [Warning] PRX is encrypted or compressed! Links may be scrambled.")
                report.append("  Please make sure you are using PPSSPP's 'Decrypted ELF' dump.")

    # 2. Parse standard ELF header
    if not elf_data.startswith(b"\x7fELF"):
        report.append("\n[Error] Invalid ELF magic number. Embedded ELF not found.")
        save_report(report, output_txt)
        return False

    magic, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack("<16sHHIIIIIHHHHHH", elf_data[:52])
    
    report.append("\n[ELF Header]")
    report.append(f"  Machine Type: {e_machine} (Expected: 8 for MIPS)")
    report.append(f"  Virtual Entry Point: 0x{e_entry:08X}")
    report.append(f"  Program Headers Offset: 0x{e_phoff:08X} (Count: {e_phnum})")
    report.append(f"  Section Headers Offset: 0x{e_shoff:08X} (Count: {e_shnum})")

    # Load bias virtualization calculation
    load_bias = 0
    if e_entry < 0x08000000:
        load_bias = 0x08800000
        report.append(f"  -> relocatable ELF (PRX) detected. Applied PSP User RAM Load Bias: 0x{load_bias:08X}")
        report.append(f"  -> Real entry point: 0x{e_entry + load_bias:08X}")
    else:
        report.append("  -> standard absolute ELF detected (No Load Bias).")

    # 3. Parse Program Segments
    report.append("\n[Program Headers / Segments]")
    segments = []
    modinfo_offset_from_phdr = 0
    
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        phdr_bytes = elf_data[off : off + e_phentsize]
        if len(phdr_bytes) < 32:
            break
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", phdr_bytes)
        
        segments.append({
            "type": p_type,
            "offset": p_offset,
            "vaddr": p_vaddr,
            "paddr": p_paddr,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "flags": p_flags
        })
        
        type_str = "PT_LOAD" if p_type == 1 else f"Type {p_type}"
        flags_str = []
        if p_flags & 1: flags_str.append("X")
        if p_flags & 2: flags_str.append("W")
        if p_flags & 4: flags_str.append("R")
        flags_str = "|".join(flags_str)
        
        report.append(f"  Segment {i}: {type_str} | FileOffset: 0x{p_offset:08X} | VAddr: 0x{p_vaddr + load_bias:08X} | FileSz: {p_filesz} bytes | MemSz: {p_memsz} bytes | Flags: {flags_str}")
        
        if i == 0 and p_paddr != 0 and p_paddr != p_vaddr:
            modinfo_offset_from_phdr = p_paddr & 0x7FFFFFFF
            report.append(f"    -> Extracted SceModuleInfo offset from Phdr p_paddr: 0x{modinfo_offset_from_phdr:08X}")

    # Memory address mapper
    def mips_get_ptr(addr):
        vaddr = addr & 0x0FFFFFFF
        if vaddr < 0x04000000:
            vaddr += 0x08800000
        real_vaddr = vaddr - load_bias
        for seg in segments:
            if seg["type"] == 1: # PT_LOAD
                start = seg["vaddr"]
                end = seg["vaddr"] + seg["memsz"]
                if real_vaddr >= start and real_vaddr < end:
                    file_offset = seg["offset"] + (real_vaddr - start)
                    if file_offset < len(elf_data):
                        return file_offset
        return None

    # Resolve SceModuleInfo address
    report.append("\n[HLE Linker Resolution Test]")
    offsets_to_try = []
    if modinfo_offset_from_phdr != 0:
        offsets_to_try.append(("Program Header p_paddr", modinfo_offset_from_phdr))
    if modinfo_offset_from_header != 0:
        offsets_to_try.append(("~PSP Header offset", modinfo_offset_from_header))
        
    sce_module_infos = []
    
    for src, offset in offsets_to_try:
        # Check virtual address first
        vaddr = offset + load_bias
        file_offset = mips_get_ptr(vaddr)
        if file_offset is not None and file_offset + 52 <= len(elf_data):
            sce_module_infos.append((vaddr, file_offset, f"Virtual pointer from {src}"))
            
        # Check raw file offset
        for seg in segments:
            if seg["type"] == 1:
                if offset >= seg["offset"] and offset < seg["offset"] + seg["filesz"]:
                    relative = offset - seg["offset"]
                    vaddr = seg["vaddr"] + relative + load_bias
                    file_offset = offset
                    sce_module_infos.append((vaddr, file_offset, f"Raw file offset from {src}"))

    # De-duplicate
    sce_module_infos = list(set(sce_module_infos))
    
    # Heuristic Memory Scan (Just like main_v18.c)
    report.append("\nExecuting Heuristic Module Scan (to verify is_valid_module_info conditions):")
    scanned_modules = []
    for seg in segments:
        if seg["type"] == 1:
            start_off = seg["offset"]
            end_off = seg["offset"] + seg["filesz"]
            # Scan in steps of 4 bytes
            for f_off in range(start_off, end_off - 52, 4):
                mod_bytes = elf_data[f_off : f_off + 52]
                mod_attr, mod_ver, mod_name_bytes = struct.unpack("<H H 28s", mod_bytes[:32])
                mod_name = read_cstring(mod_name_bytes, 0)
                
                # Check attributes and printable name
                if mod_attr <= 0x1FFF and len(mod_name) > 0 and all(32 <= ord(c) < 127 for c in mod_name):
                    # Unpack remaining fields
                    gp, ent_top, ent_btm, stub_top, stub_btm = struct.unpack("<I I I I I", mod_bytes[32:52])
                    
                    # Apply validator check (Allows ent_top/btm == 0 for consumer games like God of War)
                    valid = True
                    # Simple range validation matching our C engine:
                    # Addresses should either be 0 or point into User RAM range 0x08800000 - 0x0C000000
                    for ptr in [gp, ent_top, ent_btm, stub_top, stub_btm]:
                        if ptr != 0:
                            vptr = (ptr & 0x0FFFFFFF) + 0x08800000
                            if vptr < 0x08800000 or vptr >= 0x0C000000:
                                valid = False
                                break
                    
                    if valid and (stub_top != 0 or ent_top != 0):
                        vaddr = seg["vaddr"] + (f_off - seg["offset"]) + load_bias
                        scanned_modules.append((vaddr, f_off, mod_name, gp, ent_top, ent_btm, stub_top, stub_btm))

    report.append(f"  -> Heuristically scanned modules: {len(scanned_modules)} found.")
    for vaddr, f_off, name, gp, ent_top, ent_btm, stub_top, stub_btm in scanned_modules:
        est_imports = int(stub_btm - stub_top) // 20 if stub_top != 0 and stub_btm != 0 else 0
        report.append(f"    * Found Module '{name}' at VAddr: 0x{vaddr:08X} (FileOffset: 0x{f_off:08X}) | GP: 0x{gp:08X} | Imports: {est_imports} stubs | Exports: {int(ent_btm-ent_top)//8 if ent_top != 0 else 0}")

    if not scanned_modules:
        report.append("  [CRITICAL ERROR] No valid module headers found in memory!")
        save_report(report, output_txt)
        return False

    # 4. Dump the main modules' stub imports
    report.append("\n==============================================================================")
    report.append("                     PARSING LIBRARY IMPORT STUB HEADERS")
    report.append("==============================================================================")
    
    # We prioritize GOW main module and dax module
    for vaddr, f_off, name, gp, ent_top, ent_btm, stub_top, stub_btm in scanned_modules:
        report.append(f"\n>>> Module Name: '{name}' (GP: 0x{gp:08X}, VAddr: 0x{vaddr:08X}) <<<")
        
        stub_top_rel = stub_top + load_bias if stub_top < 0x04000000 else stub_top
        stub_btm_rel = stub_btm + load_bias if stub_btm < 0x04000000 else stub_btm
        
        curr_stub_vaddr = stub_top_rel
        stub_count = 0
        
        while curr_stub_vaddr < stub_btm_rel:
            f_stub_off = mips_get_ptr(curr_stub_vaddr)
            if f_stub_off is None or f_stub_off + 20 > len(elf_data):
                break
                
            stub_bytes = elf_data[f_stub_off : f_stub_off + 20]
            lib_name_ptr, version, attribute, struct_size, num_vars, num_funcs, nid_table_ptr, stub_table_ptr = struct.unpack("<I H H B B H I I", stub_bytes)
            
            # Resolve Library Name
            lib_name_rel = lib_name_ptr + load_bias if lib_name_ptr < 0x04000000 else lib_name_ptr
            f_lib_name_off = mips_get_ptr(lib_name_rel)
            lib_name = read_cstring(elf_data, f_lib_name_off) if f_lib_name_off is not None else "Unknown"
            
            report.append(f"  Library Import: '{lib_name}' | Version: {version} | Attribute: 0x{attribute:04X} | StructSize: {struct_size} words | Functions Count: {num_funcs}")
            
            # Dump NIDs and mapped functions
            nid_table_rel = nid_table_ptr + load_bias if nid_table_ptr < 0x04000000 else nid_table_ptr
            f_nid_table_off = mips_get_ptr(nid_table_rel)
            
            stub_table_rel = stub_table_ptr + load_bias if stub_table_ptr < 0x04000000 else stub_table_ptr
            
            if f_nid_table_off is not None and num_funcs < 512: # Safety limit
                for fn_idx in range(num_funcs):
                    nid_off = f_nid_table_off + fn_idx * 4
                    if nid_off + 4 <= len(elf_data):
                        nid = struct.unpack("<I", elf_data[nid_off : nid_off + 4])[0]
                        stub_func_vaddr = stub_table_rel + fn_idx * 8
                        
                        # Match NIDs with official GoW SDK symbols
                        func_name = match_nid(lib_name, nid)
                        report.append(f"    - NID: 0x{nid:08X} @ Stub VAddr: 0x{stub_func_vaddr:08X} -> {func_name}")
            
            # Advance
            step = struct_size
            if step < 5 or step > 32:
                step = 5
            curr_stub_vaddr += step * 4
            stub_count += 1
            if stub_count > 128: # Safety limit
                report.append("  [Warning] Exceeded maximum stub headers limit!")
                break

    save_report(report, output_txt)
    print(f"[SUCCESS] EBOOT parsing completed. Report saved to: {output_txt}")
    return True

def match_nid(library, nid):
    # official Sony NID database subset for GOW
    database = {
        "IoFileMgrForUser": {
            0x109F504F: "sceIoOpen (Alternate)",
            0x109F50BC: "sceIoOpen",
            0x6A638D83: "sceIoRead",
            0x27EB27B8: "sceIoLseek",
            0x68963324: "sceIoLseek32",
            0xA0B5A7C2: "sceIoReadAsync",
            0x89AA9906: "sceIoOpenAsync",
            0xB293727F: "sceIoChangeAsyncPriority",
            0x810C4BC3: "sceIoClose",
            0x54F5FB11: "sceIoDevctl",
            0x3251EA56: "sceIoPollAsync",
            0xE23EEC33: "sceIoWaitAsync",
            0x35DBD746: "sceIoWaitAsyncCB"
        },
        "ThreadManForUser": {
            0xD6DA4BA1: "sceKernelCreateSema",
            0x4E3A1105: "sceKernelWaitSema",
            0x3F53E640: "sceKernelSignalSema",
            0xCEADEB47: "sceKernelDelayThread",
            0x446D8DE6: "sceKernelCreateThread",
            0xF475845D: "sceKernelStartThread",
            0xAA73C935: "sceKernelExitThread",
            0x8011F9B0: "sceKernelExitThread (Alt)"
        },
        "sceGe": {
            0xAB49E7EC: "sceGeListEnqueue",
            0xE47E40E4: "sceGeListEnqueue (Alt)",
            0x034C113E: "sceGeListSync"
        },
        "sceGe_Driver": {
            0xAB49E7EC: "sceGeListEnqueue",
            0xE47E40E4: "sceGeListEnqueue (Alt)",
            0x034C113E: "sceGeListSync"
        },
        "sceAudio": {
            0x5EC81C55: "sceAudioChReserve",
            0x13F592BC: "sceAudioChRelease",
            0x14074D7E: "sceAudioOutputPanned",
            0x08DF58A5: "sceAudioOutputPanned (Alt)"
        }
    }
    
    if library in database and nid in database[library]:
        return f"{database[library][nid]}"
    return "(unknown/custom fallback)"

def save_report(lines, filepath):
    with open(output_txt_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

if __name__ == "__main__":
    target_eboot = "umd0/PSP_GAME/SYSDIR/EBOOT.BIN"
    output_txt_path = "eboot_analysis.txt"
    
    if len(sys.argv) > 1:
        target_eboot = sys.argv[1]
    if len(sys.argv) > 2:
        output_txt_path = sys.argv[2]
        
    print(f"Targeting binary: {target_eboot}")
    print(f"Output report: {output_txt_path}")
    parse_eboot(target_eboot, output_txt_path)
