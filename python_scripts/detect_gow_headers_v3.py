#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
God of War Clean Module Info Scanner - V3
This script implements the exact C-level validation logic (is_valid_module_info)
from mips_linker.c to filter out false positives and find the REAL module structures
for 'dax' and 's'.
"""

import sys
import os
import struct

def relocate_addr(addr, load_bias):
    if addr == 0:
        return 0
    relative_addr = addr & 0x0FFFFFFF
    if relative_addr >= 0x08000000 and relative_addr < 0x0C000000:
        return 0x08000000 + (relative_addr & 0x03FFFFFF)
    if relative_addr < 0x04000000:
        return relative_addr + load_bias
    return addr

def resolve_stub_address(stub_val, mod_addr, load_bias, sh_num):
    # If it's a section index
    if stub_val != 0 and stub_val < sh_num:
        return stub_val # Will be resolved via section headers later
    if stub_val == 0:
        return 0
    if stub_val >= 0x08800000 and stub_val < 0x0C000000:
        return stub_val
    return stub_val + load_bias

def parse_sce_module_info(data, offset, load_bias, e_shnum):
    if offset + 52 > len(data):
        return None
        
    fmt = "<HH27sBIIIII"
    fields = struct.unpack(fmt, data[offset : offset + 52])
    
    modAttribute = fields[0]
    modVersion = fields[1]
    name_bytes = fields[2].split(b'\0')[0]
    
    try:
        name = name_bytes.decode('ascii')
    except Exception:
        return None
        
    if len(name) == 0:
        return None
        
    # Check printable ASCII and chars
    for c in name:
        if not (c.isalnum() or c in '_-'):
            return None
            
    # Attribute validation
    if modAttribute > 0x1FFF:
        return None
        
    gp_value = fields[4]
    ent_top = fields[5]
    ent_btm = fields[6]
    stub_top = fields[7]
    stub_btm = fields[8]
    
    # gp validation
    gp = relocate_addr(gp_value, load_bias)
    if gp != 0 and (gp < 0x08800000 or gp >= 0x0C000000):
        return None
        
    # stub address checks (allowing section indices < e_shnum)
    mod_vaddr = offset + load_bias # fallback mod_addr
    st_top = resolve_stub_address(stub_top, mod_vaddr, load_bias, e_shnum)
    st_btm = resolve_stub_address(stub_btm, mod_vaddr, load_bias, e_shnum)
    
    if st_top != 0 and st_top >= e_shnum and (st_top < 0x08800000 or st_top >= 0x0C000000):
        return None
    if st_btm != 0 and st_btm >= e_shnum and (st_btm < 0x08800000 or st_btm >= 0x0C000000):
        return None
        
    return {
        "modAttribute": modAttribute,
        "modVersion": modVersion,
        "modName": name,
        "gp_value": gp_value,
        "ent_top": ent_top,
        "ent_btm": ent_btm,
        "stub_top": stub_top,
        "stub_btm": stub_btm
    }

def main():
    print("======================================================================")
    print("   God of War Precise Module & Linker Diagnostic Tool V3")
    print("======================================================================\n")
    
    boot_paths = [
        "EBOOT.BIN", "BOOT.BIN", "EBOOT.ELF", "eboot.bin", "boot.bin",
        "umd0/PSP_GAME/SYSDIR/EBOOT.BIN", "umd0/PSP_GAME/SYSDIR/EBOOT.BIN"
    ]
    
    filepath = None
    if len(sys.argv) > 1:
        filepath = sys.argv[1]
    else:
        for path in boot_paths:
            if os.path.exists(path):
                filepath = path
                break
                
    if not filepath:
        print("[FAIL] No decrypted game binary found in current workspace.")
        return
        
    print(f"[Init] Found binary for test: '{filepath}' ({os.path.getsize(filepath)} bytes)")
    
    with open(filepath, "rb") as f:
        data = f.read()
        
    elf_data = data
    if data.startswith(b"~PSP"):
        elf_offset = data.find(bytes([0x7f, ord('E'), ord('L'), ord('F')]))
        if elf_offset != -1:
            elf_data = data[elf_offset:]
            
    if not elf_data.startswith(bytes([0x7f, ord('E'), ord('L'), ord('F')])):
        print("[FAIL] Embedded block or file is not a valid ELF format.")
        return
        
    e_entry = struct.unpack("<I", elf_data[24:28])[0]
    e_phoff = struct.unpack("<I", elf_data[28:32])[0]
    e_shoff = struct.unpack("<I", elf_data[32:36])[0]
    e_phnum = struct.unpack("<H", elf_data[44:46])[0]
    e_shnum = struct.unpack("<H", elf_data[48:50])[0]
    
    is_relocatable = e_entry < 0x08000000
    load_bias = 0x08800000 if is_relocatable else 0
    
    # Map program segments to virtual memory
    vram_segments = []
    for i in range(e_phnum):
        ph_offset = e_phoff + i * 32
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<IIIIIIII", elf_data[ph_offset:ph_offset+32])
        if p_type == 1: # PT_LOAD
            vram_segments.append({
                "offset": p_offset,
                "vaddr": p_vaddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
            })
            
    def file_offset_to_vaddr(foffset):
        for seg in vram_segments:
            if foffset >= seg["offset"] and foffset < seg["offset"] + seg["filesz"]:
                rel_offset = foffset - seg["offset"]
                return (seg["vaddr"] & 0x0FFFFFFF) + rel_offset + load_bias
        return None

    print("\n--- PRECISE SCANNING ---")
    print(f"Scanning program segments with exact is_valid_module_info constraints...")
    
    found_modules = []
    for seg in vram_segments:
        start_offset = seg["offset"]
        end_offset = seg["offset"] + seg["filesz"]
        
        for foffset in range(start_offset, end_offset - 52 + 1, 4):
            mod = parse_sce_module_info(elf_data, foffset, load_bias, e_shnum)
            if mod:
                vaddr = file_offset_to_vaddr(foffset)
                if vaddr:
                    found_modules.append({
                        "foffset": foffset,
                        "vaddr": vaddr,
                        "mod": mod
                    })
                    
    print(f"\nFound {len(found_modules)} REAL SceModuleInfo structures:")
    for idx, entry in enumerate(found_modules):
        mod = entry["mod"]
        foffset = entry["foffset"]
        vaddr = entry["vaddr"]
        print(f"\n[{idx+1}] Module Name: '{mod['modName']}'")
        print(f"    File Offset:     0x{foffset:08X}")
        print(f"    Virtual Address: 0x{vaddr:08X}")
        print(f"    Attributes:      0x{mod['modAttribute']:04X}")
        print(f"    GP ($gp):        0x{mod['gp_value']:08X}")
        print(f"    stub_top:        0x{mod['stub_top']:08X}")
        print(f"    stub_btm:        0x{mod['stub_btm']:08X}")
        
    print("\n======================================================================")
    print("If you see 'dax' and 's' above, we have their exact file offsets and addresses!")
    print("======================================================================\n")

if __name__ == "__main__":
    main()
