#!/usr/bin/env python3
"""Compare a bounded core GE command trace with live PPSSPP list memory."""

import argparse
import re
import sys
from pathlib import Path

from capture_ppsspp_ge_memory import Debugger


COMMAND_RE = re.compile(
    r"\[GE Trace Command\] pc=(0x[0-9A-Fa-f]+) raw=(0x[0-9A-Fa-f]+)"
)
ERROR_RE = re.compile(r"\[GE Error\].*")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core_log", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--max-words", type=int, default=2048)
    args = parser.parse_args()

    try:
        lines = args.core_log.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()
    except OSError as exc:
        print(f"Cannot read core log: {exc}", file=sys.stderr)
        return 2

    trace = []
    errors = []
    for line_number, line in enumerate(lines, 1):
        match = COMMAND_RE.search(line)
        if match:
            trace.append((line_number, int(match.group(1), 16),
                          int(match.group(2), 16)))
        error = ERROR_RE.search(line)
        if error:
            errors.append((line_number, error.group(0)))

    print(f"Core log: {args.core_log}")
    print(f"GE command words captured: {len(trace)}")
    for line_number, error in errors[-8:]:
        print(f"Core error line {line_number}: {error}")
    if not trace:
        print(
            "No command-word trace found. Rerun with "
            "MIPS_GE_TRACE_COMMAND_RANGE=START-END."
        )
        return 0
    if len(trace) > args.max_words:
        print(
            f"Trace has {len(trace)} words, above --max-words={args.max_words}; "
            "narrow the PC range.",
            file=sys.stderr,
        )
        return 2

    debugger = None
    try:
        debugger = Debugger(args.host, args.port, args.timeout)
        status = debugger.request("cpu.status")
        ppsspp_pc = status.get("pc")
        print(
            "PPSSPP snapshot: "
            f"PC=0x{ppsspp_pc:08X}" if isinstance(ppsspp_pc, int)
            else f"PPSSPP snapshot: PC={ppsspp_pc}"
        )

        compared = 0
        matched = 0
        mismatches = []
        for line_number, address, core_word in trace:
            try:
                response = debugger.request("memory.read_u32",
                                            address=address)
                ppsspp_word = int(response["value"])
            except (KeyError, ValueError, ConnectionError, RuntimeError) as exc:
                mismatches.append(
                    (line_number, address, core_word, None, str(exc))
                )
                continue
            compared += 1
            if ppsspp_word == core_word:
                matched += 1
            else:
                mismatches.append(
                    (line_number, address, core_word, ppsspp_word, "")
                )

        print(f"PPSSPP word reads: {compared}; exact matches: {matched}")
        for line_number, address, core_word, ppsspp_word, error in mismatches[:24]:
            ppsspp_text = (
                f"0x{ppsspp_word:08X}" if ppsspp_word is not None else "unreadable"
            )
            detail = f" ({error})" if error else ""
            print(
                f"core line {line_number} PC=0x{address:08X}: "
                f"core=0x{core_word:08X} PPSSPP={ppsspp_text}{detail}"
            )
        if len(mismatches) > 24:
            print(f"... {len(mismatches) - 24} additional differences omitted")
        print(
            "Address-by-address equality is conclusive only when both captures "
            "refer to the same game state/list lifetime."
        )
    except (OSError, ConnectionError, RuntimeError) as exc:
        print(f"PPSSPP comparison failed: {exc}", file=sys.stderr)
        return 2
    finally:
        if debugger:
            debugger.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
