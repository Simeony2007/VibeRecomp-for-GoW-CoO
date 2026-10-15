#!/usr/bin/env python3
"""Verify GE 3D trace transforms against PPSSPP's viewport equations."""

import argparse
import math
import re
import sys
from pathlib import Path


FLOAT = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
DRAW_RE = re.compile(
    rf"draw=(\d+) vtype=0x[0-9A-Fa-f]+ vertices=(\d+) "
    rf"viewport=\(({FLOAT}),({FLOAT}),({FLOAT});"
    rf"\s*({FLOAT}),({FLOAT}),({FLOAT})\)"
    rf"(?: raster_offset=\(({FLOAT}),({FLOAT})\))?"
)
MATRIX_RE = re.compile(
    rf"world=\[([^\]]+)\] view=\[([^\]]+)\] proj=\[([^\]]+)\]"
)
VERTEX_RE = re.compile(
    rf"vertex=(\d+) raw=\(({FLOAT}),({FLOAT}),({FLOAT})\) "
    rf"clip=\(({FLOAT}),({FLOAT}),({FLOAT}),({FLOAT})\) "
    rf"viewport=\(({FLOAT}),({FLOAT}),({FLOAT}),({FLOAT})\) "
    rf"ndc=\(({FLOAT}),({FLOAT}),({FLOAT})\)"
)


def matrix_43(point, matrix):
    x, y, z = point
    return (
        x * matrix[0] + y * matrix[3] + z * matrix[6] + matrix[9],
        x * matrix[1] + y * matrix[4] + z * matrix[7] + matrix[10],
        x * matrix[2] + y * matrix[5] + z * matrix[8] + matrix[11],
    )


def matrix_44(point, matrix):
    x, y, z = point
    return (
        x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12],
        x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13],
        x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14],
        x * matrix[3] + y * matrix[7] + z * matrix[11] + matrix[15],
    )


def transform(raw, world, view, proj, viewport, raster_offset):
    world_pos = matrix_43(raw, world)
    view_pos = matrix_43(world_pos, view)
    clip = matrix_44(view_pos, proj)
    sx, sy, sz, cx, cy, cz = viewport
    ox, oy = raster_offset
    x = sx / 240.0 * clip[0] + ((cx - ox) / 240.0 - 1.0) * clip[3]
    y = -sy / 136.0 * clip[1] + (1.0 - (cy - oy) / 136.0) * clip[3]
    z = 2.0 * sz / 65535.0 * clip[2] + (2.0 * cz / 65535.0 - 1.0) * clip[3]
    return clip, (x, y, z, clip[3]), (x / clip[3], y / clip[3], z / clip[3])


def parse_vector(text):
    return tuple(float(value.strip()) for value in text.split(","))


def verify_log(path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    draws = []
    index = 0
    while index < len(lines):
        draw_match = DRAW_RE.search(lines[index])
        if not draw_match:
            index += 1
            continue

        draw_id = int(draw_match.group(1))
        expected_vertices = int(draw_match.group(2))
        viewport = tuple(float(draw_match.group(i)) for i in range(3, 9))
        raster_offset = (
            (float(draw_match.group(9)), float(draw_match.group(10)))
            if draw_match.group(9) is not None
            else (0.0, 0.0)
        )
        index += 1
        if index >= len(lines):
            raise ValueError(f"draw {draw_id}: missing matrix trace")
        matrix_match = MATRIX_RE.search(lines[index])
        if not matrix_match:
            raise ValueError(f"draw {draw_id}: malformed matrix trace")
        world = parse_vector(matrix_match.group(1))
        view = parse_vector(matrix_match.group(2))
        proj = parse_vector(matrix_match.group(3))
        if (len(world), len(view), len(proj)) != (12, 12, 16):
            raise ValueError(f"draw {draw_id}: unexpected matrix sizes")

        vertices = []
        index += 1
        while index < len(lines):
            if DRAW_RE.search(lines[index]):
                break
            vertex_match = VERTEX_RE.search(lines[index])
            if vertex_match:
                values = [float(vertex_match.group(i)) for i in range(2, 15)]
                raw = tuple(values[0:3])
                clip = tuple(values[3:7])
                screen = tuple(values[7:11])
                ndc = tuple(values[11:14])
                vertices.append((raw, clip, screen, ndc))
            index += 1

        if not vertices:
            raise ValueError(f"draw {draw_id}: no vertex traces found")
        if len(vertices) != expected_vertices:
            raise ValueError(
                f"draw {draw_id}: trace has {len(vertices)} of "
                f"{expected_vertices} vertices"
            )

        mismatches = []
        for vertex_id, (raw, actual_clip, actual_screen, actual_ndc) in enumerate(vertices):
            expected_clip, expected_screen, expected_ndc = transform(
                raw, world, view, proj, viewport, raster_offset
            )
            for stage, actual, expected in (
                ("clip", actual_clip, expected_clip),
                ("viewport", actual_screen, expected_screen),
                ("ndc", actual_ndc, expected_ndc),
            ):
                for component, (got, want) in enumerate(zip(actual, expected)):
                    if not math.isclose(got, want, rel_tol=2e-4, abs_tol=0.02):
                        mismatches.append(
                            f"v{vertex_id} {stage}[{component}] "
                            f"trace={got:.6g} PPSSPP={want:.6g}"
                        )

        xs = [vertex[3][0] for vertex in vertices]
        ys = [vertex[3][1] for vertex in vertices]
        degenerate = max(xs) - min(xs) < 1e-5 or max(ys) - min(ys) < 1e-5
        outside = (
            min(xs) > 1.0 or max(xs) < -1.0 or
            min(ys) > 1.0 or max(ys) < -1.0
        )
        draws.append((draw_id, len(vertices), mismatches, degenerate, outside,
                      (min(xs), max(xs), min(ys), max(ys))))

    if not draws:
        raise ValueError("no [GE 3D Trace] draws found")

    failures = 0
    for draw_id, count, mismatches, degenerate, outside, bounds in draws:
        result = "MATCH" if not mismatches else "MISMATCH"
        print(
            f"draw {draw_id}: {result}, vertices={count}, "
            f"NDC bounds x=[{bounds[0]:.5g},{bounds[1]:.5g}] "
            f"y=[{bounds[2]:.5g},{bounds[3]:.5g}]"
        )
        if degenerate:
            print("  WARNING: projected geometry is degenerate on an axis.")
        if outside:
            print("  WARNING: all vertices lie beyond the same viewport edge.")
        for mismatch in mismatches[:8]:
            print(f"  ERROR: {mismatch}")
        failures += bool(mismatches)

    print(
        f"Checked {len(draws)} draw(s) against PPSSPP transform and "
        f"viewport equations; {failures} mismatch(es)."
    )
    return 1 if failures else 0


def self_test():
    identity43 = (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    identity44 = (
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    )
    result = transform(
        (-1, -1, 0), identity43, identity43, identity44,
        (240, -136, 32767.5, 2048, 2048, 32767.5),
        (1808, 1912),
    )
    expected = ((-1, -1, 0, 1), (-1, -1, 0, 1), (-1, -1, 0))
    for actual_values, expected_values in zip(result, expected):
        if any(not math.isclose(a, e, abs_tol=1e-6)
               for a, e in zip(actual_values, expected_values)):
            raise AssertionError(f"offset/viewport self-test failed: {result}")
    print("PPSSPP transform/viewport self-test: OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path,
                        help="console log containing [GE 3D Trace] records")
    parser.add_argument("--self-test", action="store_true",
                        help="test the transform equations with a known vector")
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.log is None:
            parser.error("provide a log path or use --self-test")
        return verify_log(args.log)
    except (OSError, ValueError, AssertionError) as exc:
        print(f"GE trace verification failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
