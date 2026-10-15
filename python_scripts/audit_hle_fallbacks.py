#!/usr/bin/env python3
"""Summarize unimplemented HLE APIs observed in an emulator console log."""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path


FALLBACK = re.compile(
    r"\[HLE Fallback\]\s+Unimplemented API called:\s*(.*?)\s*->\s*"
    r"NID:\s*0x([0-9A-Fa-f]+)\s+at PC:\s*0x([0-9A-Fa-f]+)"
    r"(?:.*?call #(\d+))?"
)
IMPORT_FALLBACK = re.compile(
    r"patched import \(fallback\):\s*(\S+)\s*->\s*NID:\s*0x([0-9A-Fa-f]+)"
    r"\s*->\s*registered fallback \(custom code 0x([0-9A-Fa-f]+)\)"
)
SUGGESTIONS = {
    0xB77905EA: "sceGeEdramSetAddrTranslation (handler added; rebuild and recapture)",
    0x4C06E472: "sceGeContinue; confirm display-list continuation semantics",
    0x99944089: "sceSasSetVoice (handler added; rebuild and recapture)",
    0x76F01ACA: "sceSasSetKeyOn (handler added; rebuild and recapture)",
}


def audit(text):
    apis = defaultdict(lambda: {"pcs": set(), "samples": 0, "max_call": 0})
    imports = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        import_match = IMPORT_FALLBACK.search(line)
        if import_match:
            library, nid_text, syscall_text = import_match.groups()
            nid = int(nid_text, 16)
            imports[(library, nid)] = {
                "line": line_number,
                "syscall": int(syscall_text, 16),
            }
        match = FALLBACK.search(line)
        if match is None:
            continue
        name, nid_text, pc_text, call_text = match.groups()
        nid = int(nid_text, 16)
        entry = apis[(name, nid)]
        entry["pcs"].add((line_number, int(pc_text, 16)))
        entry["samples"] += 1
        if call_text:
            entry["max_call"] = max(entry["max_call"], int(call_text))
    return imports, apis


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("console_log", nargs="?", type=Path)
    args = parser.parse_args()
    if args.console_log is None:
        parser.error("informe o log do console")
    try:
        text = args.console_log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"erro ao ler {args.console_log}: {exc}", file=sys.stderr)
        return 2

    imports, apis = audit(text)
    print(f"Log: {args.console_log}")
    print(f"Imports HLE fallback registrados: {len(imports)}")
    if imports:
        print("Esses imports foram mapeados para fallback no build; isso nao prova chamada em runtime.")
        for (library, nid), data in sorted(imports.items()):
            called = apis.get((library, nid))
            status = "CHAMADO" if called else "sem chamada registrada"
            print(
                f"- {library}: NID=0x{nid:08X}, syscall=0x{data['syscall']:05X}, "
                f"linha={data['line']}; {status}"
            )
    print(f"APIs HLE fallback chamadas em runtime: {len(apis)}")
    if not apis:
        print("Nenhum fallback HLE encontrado no log.")
    for (name, nid), data in sorted(apis.items(),
                                    key=lambda item: (-item[1]["max_call"], -item[1]["samples"])):
        pcs = ", ".join(
            f"0x{pc:08X} (linha {line})"
            for line, pc in sorted(data["pcs"], key=lambda item: item[0])[:5]
        )
        print(
            f"- {name}: NID=0x{nid:08X}; linhas amostradas={data['samples']}; "
            f"maior call # registrado={data['max_call'] or 'ausente'}; PCs={pcs}"
        )
        suggestion = SUGGESTIONS.get(nid)
        if suggestion:
            print(f"  Ação: {suggestion}")
    print("Nota: o log pode registrar apenas algumas ocorrências, não a contagem total.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
