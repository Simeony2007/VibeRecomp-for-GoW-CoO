#!/usr/bin/env python3
"""
Diagnóstico do crash BREAK em 0x088056AC (Teste18.txt, modo AOT).

Uso:
    python3 diag_crash.py /caminho/para/EBOOT.BIN

O que faz:
  1. Localiza, nos PT_LOAD do ELF, o segmento que contém o vaddr do crash
     e o vaddr da tabela do laço AOT, calculando os offsets corretos no
     arquivo (em vez de assumir tudo dentro do segmento 0).
  2. Desmonta ~12 instruções antes e depois de 0x088056AC usando o mesmo
     decodificador do aot_recompiler_v5.py (garante que a leitura bate
     com o que o recompilador "vê").
  3. Lista as 64 entradas de 8 bytes a partir de 0x08B4B3C0 (a tabela que
     o laço AOT percorre), mostrando os dois words de cada entrada e um
     veredito sobre se aquela entrada seria um ponteiro válido de função
     dentro do range do executável.

Não precisa de nenhuma dependência além da stdlib + aot_recompiler_v5.py
no mesmo diretório (ou ajuste RECOMPILER_PATH abaixo).
"""

import struct
import sys
import os

RECOMPILER_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aot_recompiler_v5.py")

CRASH_PC = 0x088056AC
TABLE_BASE = 0x08B4B3C0
TABLE_ENTRIES = 64
TABLE_STRIDE = 8

# ELF relocável: os PT_LOAD vêm com vaddr baseado em 0x00000000.
# O loader (ver log "[Loader] Applying PSP user RAM load bias: 0x08800000")
# soma esse bias em runtime. Sem isso, todo lookup de vaddr cai fora do range.
PSP_RAM_BIAS = 0x08800000


def load_recompiler():
    import importlib.util
    spec = importlib.util.spec_from_file_location("aot_recompiler_v5", RECOMPILER_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_pt_loads(elf_bytes):
    """Extrai todos os PT_LOAD (offset, vaddr, filesz, memsz) do header ELF32."""
    e_phoff = struct.unpack_from("<I", elf_bytes, 28)[0]
    e_phentsize = struct.unpack_from("<H", elf_bytes, 42)[0]
    e_phnum = struct.unpack_from("<H", elf_bytes, 44)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", elf_bytes, off)
        if p_type == 1:  # PT_LOAD
            segs.append({
                "offset": p_offset,
                "vaddr": p_vaddr + PSP_RAM_BIAS,  # aplica o mesmo bias que o loader real aplica
                "vaddr_raw": p_vaddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
            })
    return segs


def vaddr_to_offset(segs, vaddr):
    for s in segs:
        if s["vaddr"] <= vaddr < s["vaddr"] + s["filesz"]:
            return s["offset"] + (vaddr - s["vaddr"]), s
    return None, None


def read_word(elf_bytes, file_off):
    return struct.unpack_from("<I", elf_bytes, file_off)[0]


def disasm_range(mod, elf_bytes, segs, center_vaddr, before=12, after=12):
    print(f"\n=== Desmontagem ao redor de 0x{center_vaddr:08X} ===")
    recompiler = mod.AOTRecompiler()
    start_vaddr = center_vaddr - before * 4
    for i in range(before + after + 1):
        vaddr = start_vaddr + i * 4
        file_off, seg = vaddr_to_offset(segs, vaddr)
        marker = "  <-- CRASH PC" if vaddr == center_vaddr else ""
        if file_off is None:
            print(f"0x{vaddr:08X}: <fora de qualquer PT_LOAD>{marker}")
            continue
        word = read_word(elf_bytes, file_off)
        inst = mod.MIPSInstruction(vaddr, word)
        _, comment = recompiler.translate_instruction(inst)
        print(f"0x{vaddr:08X}: {word:08X}   {comment}{marker}")


def dump_table(elf_bytes, segs):
    print(f"\n=== Tabela em 0x{TABLE_BASE:08X} ({TABLE_ENTRIES} entradas de {TABLE_STRIDE} bytes) ===")
    text_segs = [(s["vaddr"], s["vaddr"] + s["filesz"]) for s in segs]

    def looks_like_code_ptr(v):
        return any(lo <= v < hi for lo, hi in text_segs)

    for i in range(TABLE_ENTRIES):
        vaddr = TABLE_BASE + i * TABLE_STRIDE
        file_off, seg = vaddr_to_offset(segs, vaddr)
        if file_off is None:
            print(f"[{i:2d}] 0x{vaddr:08X}: <fora de qualquer PT_LOAD>")
            continue
        w0 = read_word(elf_bytes, file_off)
        w1 = read_word(elf_bytes, file_off + 4)
        veredito = []
        if w0 in (0x00000000, 0xFFFFFFFF):
            veredito.append("w0 = sentinela (0/-1) -> deveria ser SKIP")
        elif looks_like_code_ptr(w0):
            veredito.append("w0 parece ponteiro de função válido")
        else:
            veredito.append("w0 NAO cai em nenhum PT_LOAD -> suspeito")
        print(f"[{i:2d}] 0x{vaddr:08X}: w0={w0:08X} w1={w1:08X}  {' | '.join(veredito)}")


def main():
    if len(sys.argv) != 2:
        print("Uso: python3 diag_crash.py /caminho/para/EBOOT.BIN")
        sys.exit(1)

    path = sys.argv[1]
    if not os.path.exists(RECOMPILER_PATH):
        print(f"[ERRO] Não encontrei aot_recompiler_v5.py em {RECOMPILER_PATH}. "
              f"Coloque este script na mesma pasta do recompilador.")
        sys.exit(1)

    mod = load_recompiler()

    with open(path, "rb") as f:
        elf_bytes = f.read()

    if elf_bytes[:4] != b"\x7fELF":
        print("[ERRO] Arquivo não parece ser um ELF (checar se já está descriptografado).")
        sys.exit(1)

    segs = parse_pt_loads(elf_bytes)
    print("PT_LOAD encontrados (vaddr já com bias 0x08800000 somado, igual ao loader real):")
    for s in segs:
        print(f"  vaddr_raw=0x{s['vaddr_raw']:08X} -> vaddr=0x{s['vaddr']:08X} "
              f"offset=0x{s['offset']:08X} filesz={s['filesz']} memsz={s['memsz']}")

    disasm_range(mod, elf_bytes, segs, CRASH_PC)
    dump_table(elf_bytes, segs)


if __name__ == "__main__":
    main()
