#!/usr/bin/env python3
"""Find PSP threads left waiting after their sceKernelWaitThreadEnd target exits.

Usage:
    python python_scripts/diagnose_thread_waits.py artifacts/runs/Run60.console.log
    python python_scripts/diagnose_thread_waits.py artifacts/runs/Run60.console.log --self-test
"""

import argparse
import re
import sys
from pathlib import Path


SWITCH_RE = re.compile(
    r"Switched to Thread '([^']+)' \(ID: (\d+)\)"
)
WAIT_RE = re.compile(
    r"sceKernelWaitThreadEndCB\(thid=(\d+)\).*?"
    r"(?:chamadora|caller) \(ID:?\s*(\d+)\).*?WAITING"
)
FREE_RE = re.compile(
    r"Saved Thread '([^']+)' \(ID: (\d+)\):.*?-> State: 0"
)
EXIT_RE = re.compile(r"ExitThread called for Thread ID (\d+)")
WAKE_RE = re.compile(
    r"Woke thread '([^']+)' \(ID: (\d+)\): "
    r"waited-for thread (\d+) terminated"
)


def analyze_lines(lines):
    switches = {}
    waits = []
    exits = {}
    explicit_wakes = []

    for line_number, line in enumerate(lines, 1):
        match = SWITCH_RE.search(line)
        if match:
            switches.setdefault(int(match.group(2)), []).append(line_number)

        match = WAIT_RE.search(line)
        if match:
            waits.append({
                "line": line_number,
                "target": int(match.group(1)),
                "caller": int(match.group(2)),
            })

        match = EXIT_RE.search(line)
        if match:
            exits.setdefault(int(match.group(1)), []).append(line_number)

        match = FREE_RE.search(line)
        if match:
            exits.setdefault(int(match.group(2)), []).append(line_number)

        match = WAKE_RE.search(line)
        if match:
            explicit_wakes.append({
                "line": line_number,
                "caller": int(match.group(2)),
                "target": int(match.group(3)),
            })

    outcomes = []
    for wait in waits:
        later_exits = [
            line for line in exits.get(wait["target"], [])
            if line > wait["line"]
        ]
        if not later_exits:
            continue
        target_exit = min(later_exits)
        later_switches = [
            line for line in switches.get(wait["caller"], [])
            if line > target_exit
        ]
        later_wakes = [
            wake for wake in explicit_wakes
            if wake["caller"] == wait["caller"]
            and wake["target"] == wait["target"]
            and wake["line"] > wait["line"]
        ]
        outcomes.append({
            **wait,
            "target_exit": target_exit,
            "resumed": bool(later_switches or later_wakes),
        })

    return {
        "switches": switches,
        "waits": waits,
        "exits": exits,
        "outcomes": outcomes,
    }


def diagnose(path):
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        result = analyze_lines(stream)

    print(f"{path}: {sum(map(len, result['switches'].values()))} trocas de thread")
    print(f"  Esperas por sceKernelWaitThreadEndCB: {len(result['waits'])}")
    if not result["outcomes"]:
        print("  Nenhuma espera cujo alvo termine depois foi encontrada.")
        return 0

    stale = [outcome for outcome in result["outcomes"] if not outcome["resumed"]]
    for outcome in result["outcomes"]:
        status = "retomada observada" if outcome["resumed"] else "ALVO terminou, chamadora não retomou"
        print(
            f"  caller={outcome['caller']} esperou target={outcome['target']} "
            f"(linha {outcome['line']}); fim do alvo na linha "
            f"{outcome['target_exit']}: {status}"
        )

    if stale:
        print(
            "DIAGNÓSTICO: provável espera órfã de WaitThreadEndCB. "
            "A saída do alvo precisa marcar a chamadora READY, sem depender "
            "de todas as demais threads ficarem ociosas."
        )
        print(
            "Pontos de correção: sceKernelThread.c::"
            "hle_sceKernelWaitThreadEndCB e run_scheduler."
        )
        return 1

    print("DIAGNÓSTICO: todas as esperas observadas foram retomadas após o fim do alvo.")
    return 0


def self_test():
    fixture = [
        "[HLE ThreadMan] sceKernelWaitThreadEndCB(thid=5): "
        "thread alvo ainda ativa -> colocando chamadora (ID 1) em WAITING\n",
        "[HLE Scheduler TCB] Saved Thread 'snd' (ID: 5): -> State: 0\n",
        "[HLE Scheduler TCB] Switched to Thread 'disc' (ID: 3)\n",
    ]
    result = analyze_lines(fixture)
    assert len(result["outcomes"]) == 1
    assert not result["outcomes"][0]["resumed"]

    fixed_fixture = fixture + [
        "[HLE Scheduler] Woke thread 'user_main' (ID: 1): "
        "waited-for thread 5 terminated.\n",
        "[HLE Scheduler TCB] Switched to Thread 'user_main' (ID: 1)\n",
    ]
    fixed = analyze_lines(fixed_fixture)
    assert len(fixed["outcomes"]) == 1
    assert fixed["outcomes"][0]["resumed"]
    print("diagnose_thread_waits self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("console_log", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.console_log is None or not args.console_log.is_file():
        parser.error("informe um log existente ou use --self-test")
    try:
        return diagnose(args.console_log)
    except OSError as error:
        print(f"Erro lendo log: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
