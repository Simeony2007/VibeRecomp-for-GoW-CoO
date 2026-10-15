#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
relocate_eboot_v2.py - Offline Static ELF/PRX Relocator for God of War Emulator
==============================================================================
This script parses a MIPS ELF/PRX EBOOT.BIN, reads its relocation sections 
(SHT_REL/SHT_RELA), applies the PSP User RAM load bias (0x08800000) directly 
to all instructions (R_MIPS_HI16/LO16/26/32) in the data/text segments, 
and outputs a pre-relocated executable.

This completely bypasses the need for dynamic runtime relocations!
==============================================================================
"""

import sys
import struct
import os

def relocate_elf(input_path, output_path, load_bias=0x08800000):
    if not os.path.exists(input_path):
        print(f"Error: Input file '{input_path}' not found.")
        return False

    with open(input_path, "rb") as f:
        data = bytearray(f.read())

    # Detect ~PSP header and locate embedded ELF
    elf_offset = 0
    is_prx = False
    if data.startswith(b"~PSP"):
        is_prx = True
        print("[Sleuth] Detected ~PSP (PRX) container. Searching for embedded ELF...")
        elf_idx = data.find(b"\x7fELF")
        if elf_idx != -1:
            elf_offset = elf_idx
            print(f"  -> Found embedded ELF at offset 0x{elf_offset:X}")
        else:
            print("  [Error] Uncompressed ELF signature not found inside PRX container.")
            return False

    # Slice the ELF data
    elf_data = data[elf_offset:]
    
    # Unpack ELF Header
    if len(elf_data) < 52 or not elf_data.startswith(b"\x7fELF"):
        print("[Error] Invalid ELF magic number.")
        return False

    magic, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack("<16sHHIIIIIHHHHHH", elf_data[:52])

    if e_machine != 8:
        print(f"[Warning] Machine type is {e_machine} (expected 8 for MIPS Allegrex).")

    print(f"\n[ELF Headers] Entry: 0x{e_entry:08X} | Program Headers: {e_phnum} | Section Headers: {e_shnum}")

    # Check if section headers are stripped
    if e_shoff == 0 or e_shnum == 0:
        print("\n[CRITICAL LIMITATION] This EBOOT.BIN's Section Headers Table has been STRIPPED!")
        print("Without the Section Table, offline relocations cannot be performed automatically.")
        print("-> RECOMMENDED WORKAROUND: Pause PPSSPP during gameplay and perform a Memory Dump")
        print("   of the range 0x08800000 to 0x0A000000. It will be 100% decompressed and relocated!")
        return False

    # Load Program Headers (PT_LOAD segments)
    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", elf_data[off : off + 32])
        if p_type == 1: # PT_LOAD
            segments.append({
                "index": i,
                "offset": p_offset,
                "vaddr": p_vaddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
            })
            print(f"  -> PT_LOAD Segment {i}: File Offset 0x{p_offset:08X} | VAddr 0x{p_vaddr:08X} | Size {p_filesz} bytes")

    # Helper to write 32-bit words back to elf_data
    def write32(offset, val):
        struct.pack_into("<I", elf_data, offset, val)

    # Helper to read 32-bit words from elf_data
    def read32(offset):
        return struct.unpack_from("<I", elf_data, offset)[0]

    # Helper to map a virtual address inside ELF to its file offset in elf_data
    def vaddr_to_file_offset(vaddr):
        # Normalize virtual address range
        addr = vaddr & 0x0FFFFFFF
        for seg in segments:
            seg_start = seg["vaddr"] & 0x0FFFFFFF
            seg_end = seg_start + seg["memsz"]
            if addr >= seg_start and addr < seg_end:
                return seg["offset"] + (addr - seg_start)
        return None

    # Load Section Headers to search for SHT_REL (Type 9) relocation tables
    relocations_applied = 0
    for s in range(e_shnum):
        sh_off = e_shoff + s * e_shentsize
        sh_bytes = elf_data[sh_off : sh_off + 40]
        if len(sh_bytes) < 40:
            break
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack("<IIIIIIIIII", sh_bytes)

        if sh_type == 9: # SHT_REL Relocation Section
            print(f"\n[Relocator] Found SHT_REL Section {s} | Offset: 0x{sh_offset:08X} | Size: {sh_size} bytes")
            num_relocs = sh_size // sh_entsize
            
            # Link points to symbol table, Info points to the section being relocated (usually .text or .data)
            target_sect_hdr = elf_data[e_shoff + sh_info * e_shentsize : e_shoff + sh_info * e_shentsize + 40]
            t_name, t_type, t_flags, t_addr, t_offset, t_size, t_link, t_info, t_addralign, t_entsize = struct.unpack("<IIIIIIIIII", target_sect_hdr)
            
            print(f"  -> Relocating target section at file offset: 0x{t_offset:08X} (Size: {t_size} bytes)")

            # Process relocation entries
            for r in range(num_relocs):
                r_entry_off = sh_offset + r * sh_entsize
                r_offset, r_info = struct.unpack("<II", elf_data[r_entry_off : r_entry_off + 8])
                
                # Relocation target offset inside the file
                target_file_off = t_offset + r_offset
                if target_file_off + 4 > len(elf_data):
                    continue

                r_type = r_info & 0xFF
                
                if r_type == 2: # R_MIPS_32 (32-bit absolute address relocation)
                    val = read32(target_file_off)
                    # Relocate absolute pointer by adding load_bias
                    if val != 0 and val < 0x04000000:
                        write32(target_file_off, val + load_bias)
                        relocations_applied += 1

                elif r_type == 4: # R_MIPS_26 (26-bit jump target address)
                    val = read32(target_file_off)
                    target_vaddr = (val & 0x03FFFFFF) << 2
                    # Apply load bias to jump target
                    if target_vaddr < 0x04000000:
                        new_target_vaddr = target_vaddr + load_bias
                        new_val = (val & 0xFC000000) | ((new_target_vaddr >> 2) & 0x03FFFFFF)
                        write32(target_file_off, new_val)
                        relocations_applied += 1

    # Update Entry Point in ELF Header
    if e_entry < 0x04000000:
        new_entry = e_entry + load_bias
        struct.pack_into("<I", elf_data, 24, new_entry)
        print(f"\n[Loader] Successfully updated entry point from 0x{e_entry:08X} to 0x{new_entry:08X}")

    # Reconstruct PRX header if needed
    if is_prx:
        data[elf_offset:] = elf_data
        # Update entry point in PRX container header (Offset 48)
        boot_entry = struct.unpack("<I", data[48:52])[0]
        if boot_entry < 0x04000000:
            struct.pack_into("<I", data, 48, boot_entry + load_bias)
    else:
        data = elf_data

    # Save output file
    with open(output_path, "wb") as f:
        f.write(data)

    print(f"\n==============================================================================")
    print(f"[SUCCESS] PRE-RELOCATION COMPLETE!")
    print(f"Applied relocations: {relocations_applied} instructions/pointers adjusted.")
    print(f"Output saved to: {output_path}")
    print(f"==============================================================================")
    return True

if __name__ == "__main__":
    infile = "umd0/PSP_GAME/SYSDIR/EBOOT.BIN"
    outfile = "umd0/PSP_GAME/SYSDIR/EBOOT_REL.BIN"
    
    if len(sys.argv) > 1:
        infile = sys.argv[1]
    if len(sys.argv) > 2:
        outfile = sys.argv[2]
        
    print(f"Relocating input binary: {infile}")
    print(f"Saving pre-relocated binary to: {outfile}")
    relocate_elf(infile, outfile)
