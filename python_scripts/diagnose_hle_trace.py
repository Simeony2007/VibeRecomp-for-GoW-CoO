#!/usr/bin/env python3
"""Summarize HLE trace failures and repeated call sites without dumping every event.

Examples:
    python python_scripts/diagnose_hle_trace.py artifacts/runs/LogI15.trace
    python python_scripts/diagnose_hle_trace.py artifacts/runs/LogI15.trace --compare artifacts/runs/LogAOT15.trace
"""

import argparse
import itertools
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


HLE_RE = re.compile(
    r"^TRACE HLE at=(\d+)\s+pc=([0-9A-Fa-f]+)\s+id=([0-9A-Fa-f]+)\s+"
    r"args=([0-9A-Fa-f,]+)\s+result=([0-9A-Fa-f]+)"
)
SYSCALL_NAMES = {
    0x11126: "sceIoOpenAsync",
    0x11127: "sceIoChangeAsyncPriority",
    0x1111A: "sceIoWaitAsync",
    0x1111B: "sceIoPollAsync",
    0x1111F: "sceIoDopen",
    0x11120: "sceIoDread",
    0x11121: "sceIoDclose",
    0x22221: "sceKernelCreateSema",
    0x22222: "sceKernelWaitSema",
    0x22223: "sceKernelSignalSema",
    0x22224: "sceKernelDelayThread",
    0x22225: "sceKernelCreateThread",
    0x22226: "sceKernelStartThread",
    0x22227: "sceKernelExitThread",
    0x22228: "sceKernelCheckCallback",
    0x22229: "sceKernelDeleteCallback",
    0x22230: "sceKernelCreateEventFlag",
    0x22231: "sceKernelSetEventFlag",
    0x22232: "sceKernelClearEventFlag",
    0x22233: "sceKernelCreateFpl",
    0x22234: "sceKernelDeleteFpl",
    0x22235: "sceKernelAllocateFpl",
    0x22236: "sceKernelFreeFpl",
    0x22237: "sceKernelGetThreadId",
    0x22238: "sceKernelPollSema",
    0x22239: "sceKernelCpuSuspendIntr",
    0x2223A: "sceKernelCpuResumeIntr",
    0x2223B: "sceKernelCpuResumeIntrWithSync",
    0x2223C: "sceKernelIsCpuIntrEnable",
    0x40001: "sceAudioChReserve",
    0x40002: "sceAudioChRelease",
    0x40003: "sceAudioOutputPannedBlocking",
    0x40004: "sceAudioOutputPanned",
    0x40005: "sceAudioSetChannelDataLen",
    0x40010: "__sceSasInit",
    0x40011: "__sceSasCore",
    0x40012: "__sceSasGetEndFlag",
    0x40013: "__sceSasGetOutputMode",
    0x40014: "__sceSasRevType",
    0x40015: "__sceSasRevVON",
    0x40016: "__sceSasRevEVOL",
}


def event_stream(path):
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            match = HLE_RE.match(line)
            if not match:
                continue
            yield {
                "sequence": int(match.group(1)),
                "pc": int(match.group(2), 16),
                "id": int(match.group(3), 16),
                "args": tuple(int(value, 16) for value in match.group(4).split(",")),
                "result": int(match.group(5), 16),
                "line": line_number,
            }


def signed32(value):
    return value - 0x100000000 if value & 0x80000000 else value


def signature(event):
    return event["pc"], event["id"], event["args"], event["result"]


def summarize(path, top):
    totals = Counter()
    failures = Counter()
    results_by_syscall = defaultdict(Counter)
    sites = defaultdict(Counter)
    examples = defaultdict(list)
    event_count = 0

    for event in event_stream(path):
        event_count += 1
        syscall_id = event["id"]
        results_by_syscall[syscall_id][event["result"]] += 1
        site = syscall_id, event["pc"]
        totals[syscall_id] += 1
        sites[site]["calls"] += 1
        if syscall_id != 0x40012 and signed32(event["result"]) < 0:
            failures[syscall_id] += 1
            sites[site]["negative"] += 1
        sites[site][f"result:{event['result']:08X}"] += 1
        if len(examples[site]) < 3:
            examples[site].append(event)

    print(f"{path}: {event_count} eventos HLE")
    if not event_count:
        print("  Nenhum TRACE HLE encontrado.")
        return

    print("\nSyscalls mais chamadas:")
    for syscall_id, count in totals.most_common(top):
        failed = failures[syscall_id]
        name = SYSCALL_NAMES.get(syscall_id, "desconhecida")
        print(f"  0x{syscall_id:05X} {name}: {count} chamadas, "
              f"{failed} retornos negativos")

    poll_results = results_by_syscall.get(0x1111B)
    if poll_results:
        pending = poll_results[1]
        completed = poll_results[0]
        errors = sum(count for result, count in poll_results.items()
                     if signed32(result) < 0)
        print("\nContrato observado de sceIoPollAsync:")
        print(f"  pendente (1): {pending}; concluido (0): {completed}; "
              f"erros negativos: {errors}")
        if pending > 1000 and completed == 0:
            print("  AVISO: nenhuma conclusao foi observada; verifique se a "
                  "operacao assincrona termina e se o jogo progride.")

    print("\nCall sites com mais falhas ou repeticoes:")
    ranked_sites = sorted(
        sites,
        key=lambda site: (sites[site]["negative"], sites[site]["calls"]),
        reverse=True,
    )
    shown = 0
    for syscall_id, pc in ranked_sites:
        stats = sites[(syscall_id, pc)]
        if stats["negative"] == 0 and stats["calls"] < 100:
            continue
        result_counts = [
            (key[7:], count) for key, count in stats.items()
            if key.startswith("result:")
        ]
        result_counts.sort(key=lambda item: item[1], reverse=True)
        results = ", ".join(
            f"0x{result} x{count}" for result, count in result_counts[:3]
        )
        name = SYSCALL_NAMES.get(syscall_id, "desconhecida")
        print(f"  syscall=0x{syscall_id:05X} {name} pc=0x{pc:08X}: "
              f"{stats['calls']} chamadas, {stats['negative']} negativas; resultados: {results}")
        for event in examples[(syscall_id, pc)]:
            args = ",".join(f"{value:08X}" for value in event["args"])
            print(f"    seq={event['sequence']} args={args} -> 0x{event['result']:08X}")
        shown += 1
        if shown >= top:
            break
    if shown == 0:
        print("  Nenhum call site repetitivo ou retorno negativo observado.")


def compare(left, right):
    count = 0
    for count, pair in enumerate(itertools.zip_longest(
        event_stream(left), event_stream(right)
    ), 1):
        left_event, right_event = pair
        if left_event is None or right_event is None:
            print(f"\nInterpreter/AOT: comprimentos diferentes no evento {count - 1}.")
            print(f"  Interpreter restante: {left_event}")
            print(f"  AOT restante:         {right_event}")
            return
        if signature(left_event) != signature(right_event):
            print(f"\nInterpreter/AOT: primeira divergencia HLE no evento {count}.")
            print(f"  Interpreter linha {left_event['line']}: {left_event}")
            print(f"  AOT         linha {right_event['line']}: {right_event}")
            return
    print(f"\nInterpreter/AOT: {count} eventos HLE identicos.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="trace principal")
    parser.add_argument("--compare", type=Path, help="trace do outro modo para comparar")
    parser.add_argument("--top", type=int, default=12,
                        help="quantidade maxima de syscalls/call-sites exibidos")
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top precisa ser pelo menos 1")
    if not args.trace.is_file():
        parser.error(f"trace inexistente: {args.trace}")
    if args.compare is not None and not args.compare.is_file():
        parser.error(f"trace de comparacao inexistente: {args.compare}")

    try:
        summarize(args.trace, args.top)
        if args.compare is not None:
            compare(args.trace, args.compare)
    except OSError as error:
        print(f"Erro lendo trace: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
