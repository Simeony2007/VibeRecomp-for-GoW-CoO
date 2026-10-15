#!/usr/bin/env python3
"""
Desmonta as tres regioes de codigo que o watchdog flagrou no laco de
155.000.000 de ciclos (Teste20i.txt): o corpo principal do laco, a
sub-rotina perto do PowerCallback, e a sub-rotina utilitaria em 0x08B01xxx.

Uso:
    python3 diag_loop.py /caminho/para/EBOOT.BIN

Reaproveita o mesmo parser de PT_LOAD (com bias 0x08800000) e o mesmo
decodificador do aot_recompiler_v5.py que o diag_crash.py ja usa.
"""

import struct
import sys
import os
import importlib.util

RECOMPILER_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aot_recompiler_v5.py")
PSP_RAM_BIAS = 0x08800000

# (nome, endereco_central, quantas_antes, quantas_depois)
REGIONS = [
    ("Corpo principal do laco (0x088065xx-0880667x)", 0x08806640, 24, 24),
    ("Sub-rotina perto do PowerCallback (0x0899F1xx)", 0x0899F120, 16, 16),
    ("Sub-rotina utilitaria (0x08B01AAx)",             0x08B01AA8, 12, 12),
]


def load_recompiler():
    spec = importlib.util.spec_from_file_location("aot_recompiler_v5", RECOMPILER_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_pt_loads(elf_bytes):
    e_phoff = struct.unpack_from("<I", elf_bytes, 28)[0]
    e_phentsize = struct.unpack_from("<H", elf_bytes, 42)[0]
    e_phnum = struct.unpack_from("<H", elf_bytes, 44)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", elf_bytes, off)
        if p_type == 1:
            segs.append({"offset": p_offset, "vaddr": p_vaddr + PSP_RAM_BIAS,
                         "filesz": p_filesz, "memsz": p_memsz})
    return segs


def vaddr_to_offset(segs, vaddr):
    for s in segs:
        if s["vaddr"] <= vaddr < s["vaddr"] + s["filesz"]:
            return s["offset"] + (vaddr - s["vaddr"])
    return None


def main():
    if len(sys.argv) != 2:
        print("Uso: python3 diag_loop.py /caminho/para/EBOOT.BIN")
        sys.exit(1)

    mod = load_recompiler()
    with open(sys.argv[1], "rb") as f:
        elf_bytes = f.read()
    segs = parse_pt_loads(elf_bytes)
    recompiler = mod.AOTRecompiler()

    for name, center, before, after in REGIONS:
        print(f"\n=== {name} ===")
        start = center - before * 4
        for i in range(before + after + 1):
            vaddr = start + i * 4
            file_off = vaddr_to_offset(segs, vaddr)
            if file_off is None:
                print(f"0x{vaddr:08X}: <fora de qualquer PT_LOAD>")
                continue
            word = struct.unpack_from("<I", elf_bytes, file_off)[0]
            inst = mod.MIPSInstruction(vaddr, word)
            _, comment = recompiler.translate_instruction(inst)
            marker = "  <-- watchdog viu PC aqui" if vaddr == center else ""
            print(f"0x{vaddr:08X}: {word:08X}   {comment}{marker}")


if __name__ == "__main__":
    main()
