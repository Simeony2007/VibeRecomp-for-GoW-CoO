#!/usr/bin/env python3
"""Check sceAudiocodec HLE import mapping and runtime calls."""

import argparse
import re
import sys
from pathlib import Path


APIS = {
    0x9D3F790C: ("sceAudiocodecCheckNeedMem", 0x40019),
    0x3A20A200: ("sceAudiocodecGetEDRAM", 0x4001A),
}
IMPORT_RE = re.compile(
    r"patched import(?P<fallback> \(fallback\))?:\s*sceAudiocodec\s*->\s*"
    r"(?:(?P<name>.*?)\s*)?"
    r"(?:\(NID:\s*0x(?P<nid>[0-9A-Fa-f]+)\)|"
    r"NID:\s*0x(?P<fallback_nid>[0-9A-Fa-f]+))\s*->\s*"
    r"(?:(?:custom syscall:\s*0x(?P<syscall>[0-9A-Fa-f]+))|"
    r"(?:registered fallback \(custom code 0x(?P<fallback_code>[0-9A-Fa-f]+)\)))"
)
FALLBACK_RE = re.compile(
    r"Unimplemented API called:\s*sceAudiocodec\s*->\s*NID:\s*0x([0-9A-Fa-f]+)"
)
TRACE_RE = re.compile(
    r"^TRACE HLE at=(\d+)\s+pc=([0-9A-Fa-f]+)\s+id=([0-9A-Fa-f]+)"
    r"\s+args=([0-9A-Fa-f,]+)\s+result=([0-9A-Fa-f]+)"
)


def analyze(console_lines, trace_lines=()):
    imports = {}
    runtime_fallbacks = set()
    for line in console_lines:
        match = IMPORT_RE.search(line)
        if match:
            nid = int(match.group("nid") or match.group("fallback_nid"), 16)
            imports[nid] = {
                "fallback": match.group("fallback") is not None,
                "syscall": match.group("syscall"),
            }
        match = FALLBACK_RE.search(line)
        if match:
            runtime_fallbacks.add(int(match.group(1), 16))

    calls = {nid: [] for nid in APIS}
    syscall_to_nid = {syscall: nid for nid, (_, syscall) in APIS.items()}
    for line in trace_lines:
        match = TRACE_RE.match(line)
        if match is None:
            continue
        sequence, pc, syscall, args, result = match.groups()
        nid = syscall_to_nid.get(int(syscall, 16))
        if nid is not None:
            calls[nid].append({
                "sequence": int(sequence),
                "pc": int(pc, 16),
                "args": args,
                "result": int(result, 16),
            })
    return imports, runtime_fallbacks, calls


def report(console_path, trace_path=None):
    try:
        with console_path.open(encoding="utf-8", errors="replace") as console:
            if trace_path:
                with trace_path.open(encoding="utf-8", errors="replace") as trace:
                    imports, runtime_fallbacks, calls = analyze(console, trace)
            else:
                imports, runtime_fallbacks, calls = analyze(console)
    except OSError as exc:
        print(f"Erro ao ler entrada: {exc}", file=sys.stderr)
        return 2

    print(f"Console: {console_path}")
    if trace_path:
        print(f"Trace: {trace_path}")
    for nid, (name, syscall) in APIS.items():
        imported = imports.get(nid)
        if imported is None:
            mapping = "import nao localizado"
        elif imported["fallback"]:
            mapping = "ainda mapeado como fallback"
        else:
            mapping = f"mapeado para syscall 0x{syscall:05X}"
        print(f"{name} (NID 0x{nid:08X}): {mapping}")
        print(f"  fallback runtime: {'sim' if nid in runtime_fallbacks else 'nao registrado'}")
        events = calls[nid]
        print(f"  chamadas no trace: {len(events)}")
        for event in events[:3]:
            print(
                f"    seq={event['sequence']} PC=0x{event['pc']:08X} "
                f"args={event['args']} retorno=0x{event['result']:08X}"
            )
        if len(events) > 3:
            print(f"    ... mais {len(events) - 3} chamadas")
    print("Este relatorio verifica dispatch/fallback, nao prova que o jogo concluiu.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("console", nargs="?", type=Path)
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        console = (
            "patched import: sceAudiocodec -> sceAudiocodecCheckNeedMem "
            "(NID: 0x9D3F790C) -> custom syscall: 0x40019\n"
            "patched import (fallback): sceAudiocodec -> NID: 0x3A20A200 "
            "-> registered fallback (custom code 0x50087)"
        )
        trace = (
            "TRACE HLE at=42 pc=08B8334C id=40019 "
            "args=08B83640,00001001,00000000,0000003F result=00000000\n"
            "TRACE HLE at=50 pc=08B8332C id=4001A "
            "args=08B83640,00001001,00019000,0000003F result=00000000"
        )
        imports, fallbacks, calls = analyze(console.splitlines(), trace.splitlines())
        assert set(imports) == set(APIS)
        assert not fallbacks
        assert imports[0x3A20A200]["fallback"]
        assert all(len(calls[nid]) == 1 for nid in APIS)
        print("Audio codec HLE diagnostic self-test passed.")
        return 0
    if args.console is None:
        parser.error("informe o console do emulador")
    return report(args.console, args.trace)


if __name__ == "__main__":
    raise SystemExit(main())
