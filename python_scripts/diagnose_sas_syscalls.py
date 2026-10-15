#!/usr/bin/env python3
"""Test whether SAS voice syscalls are missing or simply never reached in a trace."""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


HLE = re.compile(
    r"^TRACE HLE at=(\d+)\s+pc=([0-9A-Fa-f]+)\s+id=([0-9A-Fa-f]+)"
    r"\s+args=([0-9A-Fa-f,]+)\s+result=([0-9A-Fa-f]+)(.*)$"
)
RA = re.compile(r"\bra=([0-9A-Fa-f]+)")
PATCHED_IMPORT = re.compile(
    r"patched import(?P<fallback> \(fallback\))?:\s*(?P<library>\S+)\s*->\s*"
    r"(?:(?P<name>.*?)\s*)?(?:\(NID:\s*0x(?P<nid>[0-9A-Fa-f]+)\)"
    r"|NID:\s*0x(?P<fallback_nid>[0-9A-Fa-f]+))"
    r"(?:\s*->\s*custom syscall:\s*0x(?P<syscall>[0-9A-Fa-f]+)"
    r"|\s*->\s*registered fallback \(custom code 0x(?P<fallback_code>[0-9A-Fa-f]+)\))"
)

SAS_IDS = {
    0x40010: "__sceSasInit",
    0x40011: "__sceSasCore",
    0x40012: "__sceSasGetEndFlag",
    0x40017: "__sceSasSetVoice",
    0x40018: "__sceSasSetKeyOn",
}
VOICE_NIDS = {0x99944089: "__sceSasSetVoice", 0x76F01ACA: "__sceSasSetKeyOn"}
KNOWN_FALLBACK_NIDS = {
    0x977DE386: "sceKernelLoadModule",
    0x50F0C1EC: "sceKernelStartModule",
    0xB293727F: "sceIoChangeAsyncPriority",
    0x24FD7BCF: "sceImposeGetLanguageMode",
    0x36AA6E91: "sceImposeSetLanguageMode",
}


def analyze(trace_text, console_text=""):
    counts = Counter()
    end_flags = Counter()
    last_end_flag = {}
    trace_fallbacks = Counter()
    events = []

    for line_number, line in enumerate(trace_text.splitlines(), 1):
        match = HLE.match(line)
        if match is None:
            continue
        at_text, pc_text, id_text, args, result_text, trailing = match.groups()
        event_id = int(id_text, 16)
        counts[event_id] += 1
        if event_id == 0x40012:
            result = int(result_text, 16)
            end_flags[result] += 1
            ra_match = RA.search(trailing)
            last_end_flag = {
                "line": line_number,
                "at": int(at_text),
                "pc": int(pc_text, 16),
                "result": result,
                "ra": int(ra_match.group(1), 16) if ra_match else None,
            }
        ra_match = RA.search(trailing)
        event = {
            "line": line_number,
            "at": int(at_text),
            "id": event_id,
            "pc": int(pc_text, 16),
            "args": args,
            "result": int(result_text, 16),
            "ra": int(ra_match.group(1), 16) if ra_match else None,
        }
        if event_id >= 0x50000:
            trace_fallbacks[event_id] += 1
        events.append(event)

    imports = {}
    for line_number, line in enumerate(console_text.splitlines(), 1):
        match = PATCHED_IMPORT.search(line)
        if match is None:
            continue
        groups = match.groupdict()
        nid = int(groups["nid"] or groups["fallback_nid"], 16)
        code = groups["syscall"] or groups["fallback_code"]
        imports[nid] = {
            "library": groups["library"],
            "name": groups["name"].strip() if groups["name"] else "",
            "code": int(code, 16),
            "fallback": groups["fallback"] is not None or groups["fallback_code"] is not None,
            "line": line_number,
        }

    runtime_fallbacks = {}
    for line_number, line in enumerate(console_text.splitlines(), 1):
        if "[HLE Fallback] Unimplemented API called:" not in line:
            continue
        nid_match = re.search(r"NID:\s*0x([0-9A-Fa-f]+)", line)
        if nid_match:
            runtime_fallbacks[int(nid_match.group(1), 16)] = line_number

    fallback_codes = {
        value["code"]: (nid, value)
        for nid, value in imports.items()
        if value["fallback"]
    }
    return {
        "counts": counts,
        "end_flags": end_flags,
        "last_end_flag": last_end_flag,
        "fallbacks": trace_fallbacks,
        "events": events,
        "imports": imports,
        "fallback_codes": fallback_codes,
        "runtime_fallbacks": runtime_fallbacks,
    }


def report(result, trace_path, console_path=None):
    counts = result["counts"]
    print(f"Trace: {trace_path}")
    if console_path:
        print(f"Console: {console_path}")
    print("Syscalls SAS observadas:")
    for syscall_id, name in SAS_IDS.items():
        print(f"  {name}: {counts[syscall_id]}")

    print("Estado dos imports de voz no console:")
    for nid, name in VOICE_NIDS.items():
        imported = result["imports"].get(nid)
        if imported is None:
            print(f"  {name} (NID 0x{nid:08X}): nao localizado no log de build")
        else:
            status = "fallback" if imported["fallback"] else "mapeado"
            print(
                f"  {name} (NID 0x{nid:08X}): {status} como "
                f"syscall 0x{imported['code']:05X}"
            )
        if nid in result["runtime_fallbacks"]:
            print(f"    fallback runtime na linha {result['runtime_fallbacks'][nid]}")

    print("Chamadas de fallback registradas no trace:")
    matched_fallbacks = {
        syscall_id: count
        for syscall_id, count in result["fallbacks"].items()
        if syscall_id in result["fallback_codes"]
    }
    if matched_fallbacks:
        for syscall_id, count in sorted(matched_fallbacks.items(), key=lambda item: -item[1]):
            nid, imported = result["fallback_codes"][syscall_id]
            print(
                f"  {imported['library']} NID 0x{nid:08X}, "
                f"syscall 0x{syscall_id:05X}: {count}"
            )
    else:
        if result["fallbacks"] and not result["imports"]:
            print("  trace tem IDs customizados, mas faltou o console para identificar fallbacks")
        else:
            print("  nenhum fallback registrado foi chamado")
    unclassified = {
        syscall_id: count
        for syscall_id, count in result["fallbacks"].items()
        if syscall_id not in result["fallback_codes"]
    }
    if unclassified:
        print("Outros IDs customizados (nao presentes como fallback no import table):")
        for syscall_id, count in sorted(unclassified.items(), key=lambda item: -item[1])[:10]:
            print(f"  0x{syscall_id:05X}: {count}")

    print("Amostras dos fallbacks de runtime relevantes:")
    fallback_events = [
        event
        for event in result["events"]
        if event["id"] in result["fallback_codes"]
    ]
    fallback_events.sort(
        key=lambda event: (
            0
            if result["fallback_codes"][event["id"]][0] in KNOWN_FALLBACK_NIDS
            else 1,
            event["at"],
        )
    )
    for event in fallback_events[:12]:
        imported = result["fallback_codes"].get(event["id"])
        nid, details = imported
        name = KNOWN_FALLBACK_NIDS.get(nid, f"{details['library']} NID 0x{nid:08X}")
        ra = f"0x{event['ra']:08X}" if event["ra"] is not None else "indisponivel"
        print(
            f"  {name}: args={event['args']} retorno=0x{event['result']:08X} "
            f"PC=0x{event['pc']:08X} RA={ra}"
        )

    if result["end_flags"]:
        print("Resultados de __sceSasGetEndFlag:")
        for flag, count in result["end_flags"].most_common():
            print(f"  0x{flag:08X}: {count}")
        last = result["last_end_flag"]
        ra = f"0x{last['ra']:08X}" if last["ra"] is not None else "indisponivel"
        print(
            f"  ultima consulta: trace linha {last['line']}, PC=0x{last['pc']:08X}, "
            f"RA= {ra}"
        )

    voice_calls = counts[0x40017] + counts[0x40018]
    if voice_calls:
        print("Conclusao: houve configuracao/acionamento de voz; a hipotese de")
        print("syscalls de voz ausentes nao explica, sozinha, o comportamento.")
    elif counts[0x40012] and counts[0x40011]:
        voice_imports = [result["imports"].get(nid) for nid in VOICE_NIDS]
        if all(item and not item["fallback"] for item in voice_imports):
            print("Conclusao: os imports estao mapeados, mas SetVoice/KeyOn nao foram")
            print("executados nesta captura. Nao ha evidencia de syscall faltando.")
            print("Investigue o fluxo do jogo anterior ao SAS; EndFlag=FFFFFFFF e")
            print("compativel com nenhuma voz ter sido acionada.")
        elif any(item and item["fallback"] for item in voice_imports):
            print("Conclusao: ao menos um import permanece como fallback, mas o trace")
            print("nao mostra uma chamada de voz. Isso ainda nao prova causalidade.")
        else:
            print("Conclusao: o trace nao mostra chamadas de voz; sem o log de build,")
            print("nao e possivel decidir se o import esta mapeado ou faltando.")
    elif not result["events"]:
        print("Conclusao: nao ha eventos SAS reconheciveis nesta captura.")
    return 0


def self_test():
    trace = "\n".join(
        [
            "TRACE HLE at=10 pc=08B00000 id=40011 args=0,0,0,0 result=00000000",
            "TRACE HLE at=11 pc=08B00004 id=40012 args=0,0,0,0 result=FFFFFFFF ra=08A235E0",
            "TRACE HLE at=12 pc=08B00008 id=50065 args=0,0,0,0 result=00000000",
        ]
    )
    console = "\n".join(
        [
            "-> patched import: sceSasCore -> __sceSasSetVoice "
            "(NID: 0x99944089) -> custom syscall: 0x40017",
            "-> patched import: sceSasCore -> __sceSasSetKeyOn "
            "(NID: 0x76F01ACA) -> custom syscall: 0x40018",
            "-> patched import (fallback): sceSasCore -> NID: 0xABCDEF01 "
            "-> registered fallback (custom code 0x50065) [non-critical]",
        ]
    )
    result = analyze(trace, console)
    assert result["counts"][0x40011] == 1
    assert result["counts"][0x40012] == 1
    assert result["fallbacks"][0x50065] == 1
    assert result["imports"][0x99944089]["code"] == 0x40017
    assert result["imports"][0xABCDEF01]["fallback"]
    assert result["fallback_codes"][0x50065][0] == 0xABCDEF01
    assert result["last_end_flag"]["ra"] == 0x08A235E0
    assert result["counts"][0x40017] == 0
    print("diagnose_sas_syscalls self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("console_log", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.trace is None:
        parser.error("informe um trace existente ou use --self-test")

    try:
        trace_text = args.trace.read_text(encoding="utf-8", errors="replace")
        console_text = (
            args.console_log.read_text(encoding="utf-8", errors="replace")
            if args.console_log
            else ""
        )
    except OSError as exc:
        print(f"erro ao ler arquivo: {exc}", file=sys.stderr)
        return 2

    return report(analyze(trace_text, console_text), args.trace, args.console_log)


if __name__ == "__main__":
    raise SystemExit(main())
