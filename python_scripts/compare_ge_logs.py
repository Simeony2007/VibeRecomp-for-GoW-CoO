#!/usr/bin/env python3
"""Extract concise GE evidence from core and PPSSPP console logs."""

import argparse
import json
import re
import sys
from pathlib import Path


FLOAT = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
DRAW_RE = re.compile(
    rf"\[GE 3D Trace\] draw=(\d+) .*?vertices=(\d+).*?"
    rf"viewport=\(({FLOAT}),({FLOAT}),({FLOAT});"
    rf"\s*({FLOAT}),({FLOAT}),({FLOAT})\)"
)
MATRIX_RE = re.compile(
    rf"\[GE 3D Trace\].*?world=\[([^\]]+)\] view=\[([^\]]+)\] "
    rf"proj=\[([^\]]+)\]"
)
MATRIX_NUMBER_RE = re.compile(
    r"\[GE Matrix Trace\] pc=(0x[0-9A-Fa-f]+) matrix=projection op=number"
)
MATRIX_DATA_RE = re.compile(
    rf"\[GE Matrix Trace\] pc=(0x[0-9A-Fa-f]+) matrix=projection "
    rf"op=data raw=(0x[0-9A-Fa-f]+).*?index=(\d+) value=({FLOAT})"
)
VERTEX_RE = re.compile(
    rf"\[GE 3D Trace\] vertex=(\d+).*?ndc=\(({FLOAT}),({FLOAT}),({FLOAT})\)"
)
CORE_ENQUEUE_RE = re.compile(r"\[HLE GE\] Enqueued Display List \(Start: (0x[0-9A-Fa-f]+)")
CORE_FLOW_RE = re.compile(r"\[GE CTRL\] (CALL|JUMP) .*?target=(0x[0-9A-Fa-f]+)")
CORE_JUMP_RE = re.compile(
    r"\[GE CTRL\] JUMP list_pc=(0x[0-9A-Fa-f]+) raw=(0x[0-9A-Fa-f]+)"
    r".*?target=(0x[0-9A-Fa-f]+)"
)
MATRIX_WRITE_RE = re.compile(r"\[GE Matrix Trace\].*?matrix=(\w+) op=data")
PPSSPP_ENQUEUE_RE = re.compile(r"sceGeListEnQueue(?:Head)?", re.IGNORECASE)
PPSSPP_MATRIX_RE = re.compile(r"\[GE Matrix Trace\]|\[GE 3D Trace\]")


def parse_values(value):
    return [item.strip() for item in value.split(",")]


def analyze_core(lines):
    draws = []
    projection_groups = []
    active_projection = None
    for index, line in enumerate(lines):
        matrix_number = MATRIX_NUMBER_RE.search(line)
        if matrix_number:
            if active_projection:
                projection_groups.append(active_projection)
            active_projection = {
                "line": index,
                "pc": matrix_number.group(1),
                "values": {},
            }
        matrix_data = MATRIX_DATA_RE.search(line)
        if matrix_data and active_projection:
            active_projection["values"][int(matrix_data.group(3))] = float(matrix_data.group(4))
            active_projection.setdefault("words", {})[int(matrix_data.group(1), 16)] = (
                0x3F000000 | int(matrix_data.group(2), 16)
            )

        match = DRAW_RE.search(line)
        if not match:
            continue
        draw_id, vertex_count = int(match.group(1)), int(match.group(2))
        vertices = []
        for candidate in lines[index + 1:]:
            if DRAW_RE.search(candidate):
                break
            vertex = VERTEX_RE.search(candidate)
            if vertex:
                vertices.append(
                    (int(vertex.group(1)), float(vertex.group(2)),
                     float(vertex.group(3)), float(vertex.group(4)))
                )
            if len(vertices) >= vertex_count:
                break
        if vertices:
            xs = [vertex[1] for vertex in vertices]
            ys = [vertex[2] for vertex in vertices]
            draws.append({
                "id": draw_id,
                "line": index,
                "vertices": len(vertices),
                "ndc_bounds": (min(xs), max(xs), min(ys), max(ys)),
                "degenerate": max(xs) - min(xs) < 1e-5 or max(ys) - min(ys) < 1e-5,
                "offscreen": min(xs) > 1 or max(xs) < -1 or min(ys) > 1 or max(ys) < -1,
            })

    if active_projection:
        projection_groups.append(active_projection)
    first_matrix = next((MATRIX_RE.search(line) for line in lines if MATRIX_RE.search(line)), None)
    enqueues = [CORE_ENQUEUE_RE.search(line).group(1) for line in lines if CORE_ENQUEUE_RE.search(line)]
    flow = [CORE_FLOW_RE.search(line).groups() for line in lines if CORE_FLOW_RE.search(line)]
    matrix_writes = sum(bool(MATRIX_WRITE_RE.search(line)) for line in lines)
    traced_projection = parse_values(first_matrix.group(3)) if first_matrix else None
    jumps = [CORE_JUMP_RE.search(line).groups() for line in lines if CORE_JUMP_RE.search(line)]
    return draws, projection_groups, traced_projection, enqueues, flow, matrix_writes, jumps


def load_ppsspp_memory(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    words = {
        int(word["address"], 16): int(word["value"], 16)
        for memory_range in data["ranges"]
        for word in memory_range["words"]
    }
    return data, words


def make_report(core_path, ppsspp_path, memory_path=None):
    core_lines = core_path.read_text(encoding="utf-8", errors="replace").splitlines()
    ppsspp_lines = ppsspp_path.read_text(encoding="utf-8", errors="replace").splitlines()
    draws, projection_groups, traced_projection, core_enqueues, flow, matrix_writes, jumps = analyze_core(core_lines)
    ppsspp_enqueues = [
        (index + 1, line.strip())
        for index, line in enumerate(ppsspp_lines)
        if PPSSPP_ENQUEUE_RE.search(line)
    ]
    ppsspp_matrix = [
        (index + 1, line.strip())
        for index, line in enumerate(ppsspp_lines)
        if PPSSPP_MATRIX_RE.search(line)
    ]
    state_lines = [
        (index + 1, line.strip())
        for index, line in enumerate(ppsspp_lines)
        if re.search(r"Booted |Failed to load state|Loaded state", line, re.IGNORECASE)
    ]

    report = [
        "GE trace comparison (core vs PPSSPP)",
        f"Core log: {core_path}",
        f"PPSSPP log: {ppsspp_path}",
        "",
        f"Core: {len(core_enqueues)} enqueued list(s), {len(draws)} traced 3D draw(s), "
        f"{matrix_writes} GE matrix-data write(s), {len(flow)} CALL/JUMP control command(s).",
        f"PPSSPP: {len(ppsspp_enqueues)} logged sceGeListEnQueue call(s), "
        f"{len(ppsspp_matrix)} matching matrix/3D trace line(s).",
    ]
    for draw in draws[:4]:
        bounds = draw["ndc_bounds"]
        flags = []
        if draw["degenerate"]:
            flags.append("degenerate")
        if draw["offscreen"]:
            flags.append("outside viewport")
        report.append(
            f"Core draw {draw['id']}: {draw['vertices']} vertices; "
            f"NDC x=[{bounds[0]:.6g},{bounds[1]:.6g}] "
            f"y=[{bounds[2]:.6g},{bounds[3]:.6g}]"
            + (f"; {', '.join(flags)}" if flags else "")
        )
        preceding = [
            group for group in projection_groups if group["line"] < draw["line"]
        ]
        if preceding:
            group = preceding[-1]
            values = group["values"]
            entries = ", ".join(
                f"{index}={values[index]:.6g}" for index in sorted(values)
                if abs(values[index]) > 1e-8
            ) or "all zero"
            report.append(
                f"  Latest projection writes before draw: PC={group['pc']}, "
                f"{len(values)}/16 entries; nonzero {entries}"
            )
    if traced_projection:
        report.append("Core 3D trace's first projection matrix: [" +
                      ", ".join(traced_projection) + "]")
    for line_number, line in state_lines[:4]:
        report.append(f"PPSSPP state/boot log line {line_number}: {line}")
    for line_number, line in ppsspp_enqueues[:3]:
        report.append(f"PPSSPP enqueue line {line_number}: {line}")
    if flow:
        report.append("Last core control-flow targets: " + ", ".join(
            f"{kind}->{target}" for kind, target in flow[-4:]
        ))
    if memory_path:
        ppsspp_memory, words = load_ppsspp_memory(memory_path)
        report.append(
            f"PPSSPP debugger snapshot: PC=0x{ppsspp_memory['cpu_status']['pc']:08X}; "
            f"{len(words)} memory word(s) at requested addresses."
        )
        if jumps:
            pc, raw, target = jumps[0]
            expected = 0x08000000 | int(raw, 16)
            actual = words.get(int(pc, 16))
            report.append(
                f"JUMP command at {pc}: core=0x{expected:08X}; "
                + (f"PPSSPP=0x{actual:08X} ({'same word' if actual == expected else 'different word'})." 
                   if actual is not None else "not present in captured PPSSPP memory.")
            )
            target_word = words.get(int(target, 16))
            if target_word is not None:
                report.append(f"PPSSPP memory at that target {target}: 0x{target_word:08X}.")
        if projection_groups:
            first_draw_line = draws[0]["line"] if draws else len(core_lines)
            groups_before_draw = [
                group for group in projection_groups if group["line"] < first_draw_line
            ]
            if groups_before_draw:
                matrix_words = groups_before_draw[-1]["words"]
                for address in sorted(matrix_words)[:4]:
                    actual = words.get(address)
                    if actual is not None:
                        expected = matrix_words[address]
                        report.append(
                            f"Projection command at 0x{address:08X}: core=0x{expected:08X}; "
                            f"PPSSPP=0x{actual:08X} "
                            f"({'same word' if actual == expected else 'different word'})."
                        )
        report.append(
            "Memory-word matches are address-only: the debugger snapshot is not proven to be "
            "the same game state or GE-list execution point as the core trace."
        )

    if not ppsspp_enqueues or not ppsspp_matrix:
        report.extend([
            "",
            "RESULT: no comparable PPSSPP display-list/matrix trace is present in this log.",
            "The core's degenerate/off-screen projection is observable, but these logs do not "
            "establish whether its GE command stream differs from PPSSPP. No root cause is "
            "confirmed; the PPSSPP gameplay state/list capture is still required.",
        ])
    else:
        report.extend([
            "",
            "RESULT: both logs contain GE evidence, but this console-only script does not "
            "compare command words. Capture the corresponding PPSSPP display-list memory "
            "before attributing a mismatch.",
        ])
    return "\n".join(report) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core_log", type=Path)
    parser.add_argument("ppsspp_log", type=Path)
    parser.add_argument("--ppsspp-memory", type=Path,
                        help="JSON output from capture_ppsspp_ge_memory.py")
    parser.add_argument("--output", type=Path, help="also save the concise report here")
    args = parser.parse_args()
    try:
        report = make_report(args.core_log, args.ppsspp_log, args.ppsspp_memory)
        print(report, end="")
        if args.output:
            args.output.write_text(report, encoding="utf-8")
    except OSError as exc:
        print(f"Could not analyze GE logs: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
