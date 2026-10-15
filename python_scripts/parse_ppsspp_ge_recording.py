#!/usr/bin/env python3
"""Extract GE matrix commands from a PPSSPP .ppdmp GPU recording."""

import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from compare_ge_logs import analyze_core


HEADER_MAGIC = b"PPSSPPGE"
COMMAND_REGISTER = 1
COMMAND_VERTICES = 2
GE_VERTEX_TYPE = 0x12
GE_PROJECTION_NUMBER = 0x3E
GE_PROJECTION_DATA = 0x3F
FULLSCREEN_QUAD = (
    (0.0, 0.0, 0.0),
    (480.0, 0.0, 0.0),
    (480.0, 272.0, 0.0),
    (0.0, 272.0, 0.0),
)


def read_exact(file, size):
    data = file.read(size)
    if len(data) != size:
        raise ValueError("truncated PPSSPP recording")
    return data


def decompress_block(file, zstd_path, expected_size):
    compressed_size = struct.unpack("<I", read_exact(file, 4))[0]
    compressed = read_exact(file, compressed_size)
    result = subprocess.run(
        [zstd_path, "-d", "-c"],
        input=compressed,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise ValueError(
            "zstd decompression failed: "
            + result.stderr.decode("utf-8", errors="replace").strip()
        )
    if len(result.stdout) != expected_size:
        raise ValueError(
            f"decompressed block has {len(result.stdout)} bytes; expected {expected_size}"
        )
    return result.stdout


def decode_float24(value):
    return struct.unpack("<f", struct.pack("<I", (value & 0xFFFFFF) << 8))[0]


def transform_projection(point, matrix):
    x, y, z = point
    clip = (
        x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12],
        x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13],
        x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14],
        x * matrix[3] + y * matrix[7] + z * matrix[11] + matrix[15],
    )
    if abs(clip[3]) < 1e-12:
        return None
    return clip[0] / clip[3], clip[1] / clip[3]


def find_fullscreen_quads(commands, pushbuf):
    vtype = 0
    projection = [0.0] * 16
    projection_index = 0
    quads = []
    for command_index, (command_type, size, ptr) in enumerate(commands):
        if command_type == COMMAND_REGISTER:
            for offset in range(ptr, ptr + size, 4):
                word = struct.unpack_from("<I", pushbuf, offset)[0]
                opcode, value = word >> 24, word & 0xFFFFFF
                if opcode == GE_VERTEX_TYPE:
                    vtype = value
                elif opcode == GE_PROJECTION_NUMBER:
                    projection_index = value & 0x0F
                elif opcode == GE_PROJECTION_DATA:
                    projection[projection_index] = decode_float24(value)
                    projection_index = (projection_index + 1) & 0x0F
            continue
        if command_type != COMMAND_VERTICES or size > 512 or size % 4:
            continue
        vertices = pushbuf[ptr:ptr + size]
        for stride in range(12, min(size, 64) + 1, 4):
            if size % stride:
                continue
            count = size // stride
            if count < 4:
                continue
            for start in range(count - 3):
                for position_offset in range(0, stride - 11, 4):
                    decoded = [
                        struct.unpack_from(
                            "<fff", vertices, vertex_index * stride + position_offset
                        )
                        for vertex_index in range(start, start + 4)
                    ]
                    if all(
                        math.isclose(actual[component], expected[component],
                                     rel_tol=1e-6, abs_tol=1e-4)
                        for actual, expected in zip(decoded, FULLSCREEN_QUAD)
                        for component in range(3)
                    ):
                        quads.append({
                            "operation": command_index,
                            "vtype": vtype,
                            "stride": stride,
                            "position_offset": position_offset,
                            "projection": list(projection),
                        })
                        break
                else:
                    continue
                break
    return quads


def read_recording(path, zstd_path):
    with path.open("rb") as file:
        header = read_exact(file, 24)
        if header[:8] != HEADER_MAGIC:
            raise ValueError("not a PPSSPP GE recording (bad magic)")
        version = struct.unpack_from("<I", header, 8)[0]
        game_id = header[12:21].split(b"\0", 1)[0].decode("ascii", errors="replace")
        if version < 5:
            raise ValueError(f"unsupported compression version {version}; expected version 5+")
        command_count, pushbuf_size = struct.unpack("<II", read_exact(file, 8))
        commands_data = decompress_block(file, zstd_path, command_count * 9)
        pushbuf = decompress_block(file, zstd_path, pushbuf_size)
        if file.read(1):
            raise ValueError("unexpected trailing data after PPSSPP recording blocks")

    commands = [
        struct.unpack_from("<BII", commands_data, index * 9)
        for index in range(command_count)
    ]
    projections = []
    projection = None
    register_words = 0
    command_types = {}
    for command_type, size, ptr in commands:
        command_types[command_type] = command_types.get(command_type, 0) + 1
        if command_type != COMMAND_REGISTER:
            continue
        if size % 4 or ptr + size > len(pushbuf):
            raise ValueError(f"invalid register block: ptr={ptr} size={size}")
        register_words += size // 4
        for offset in range(ptr, ptr + size, 4):
            word = struct.unpack_from("<I", pushbuf, offset)[0]
            opcode, value = word >> 24, word & 0xFFFFFF
            if opcode == GE_PROJECTION_NUMBER:
                if projection is not None:
                    projections.append(projection)
                projection = {"values": {}, "writes": 0}
                projection["index"] = value & 0x0F
            elif opcode == GE_PROJECTION_DATA and projection is not None:
                index = projection["index"]
                projection["values"][index] = decode_float24(value)
                projection["writes"] += 1
                projection["index"] = (index + 1) & 0x0F
    if projection is not None:
        projections.append(projection)
    return {
        "version": version,
        "game_id": game_id,
        "command_count": command_count,
        "pushbuf_size": pushbuf_size,
        "command_types": command_types,
        "register_words": register_words,
        "projections": projections,
        "fullscreen_quads": find_fullscreen_quads(commands, pushbuf),
    }


def compare_core(path, ppsspp_projections):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    draws, core_projections, *_ = analyze_core(lines)
    first_draw_line = draws[0]["line"] if draws else len(lines)
    prior = [group for group in core_projections if group["line"] < first_draw_line]
    if not prior:
        return ["Core log has no projection writes preceding a traced draw."]
    core_values = prior[-1]["values"]
    output = [
        "Core first-draw projection: "
        + json.dumps([core_values.get(index) for index in range(16)])
    ]
    if all(index in core_values for index in range(16)):
        core_matrix = [core_values[index] for index in range(16)]
        core_corners = [
            transform_projection(point, core_matrix) for point in FULLSCREEN_QUAD
        ]
        output.append(
            "Core matrix on 480x272 screen-space corners: " + json.dumps(core_corners)
        )
    exact_matches = []
    for index, projection in enumerate(ppsspp_projections):
        values = projection["values"]
        if len(values) == 16 and all(values.get(i) == core_values.get(i) for i in range(16)):
            exact_matches.append(index)
    if exact_matches:
        output.append(f"PPSSPP projection matrix exactly matches core group(s): {exact_matches}")
    else:
        output.append(
            f"No exact match among {len(ppsspp_projections)} recorded PPSSPP projection group(s)."
        )
        if ppsspp_projections:
            first = ppsspp_projections[0]["values"]
            output.append(
                "First PPSSPP projection: "
                + json.dumps([first.get(index) for index in range(16)])
            )
    output.append(
        "Matrix comparison is numeric only; it does not prove both captures are the same frame."
    )
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--core-log", type=Path,
                        help="optional core log with [GE Matrix Trace] and [GE 3D Trace]")
    parser.add_argument("--zstd", default=shutil.which("zstd") or shutil.which("zstd.exe"),
                        help="zstd CLI path (defaults to zstd on PATH)")
    parser.add_argument("--output", type=Path, help="also save the concise report here")
    args = parser.parse_args()
    try:
        if not args.zstd:
            raise ValueError("zstd CLI not found; pass --zstd with its executable path")
        result = read_recording(args.recording, args.zstd)
        report = [
            "PPSSPP GE recording analysis",
            f"File: {args.recording}",
            f"Game ID: {result['game_id']}; format version: {result['version']}",
            f"Recorded operations: {result['command_count']}; "
            f"GE register words: {result['register_words']}; "
            f"push buffer: {result['pushbuf_size']} bytes",
            "Operation types: " + ", ".join(
                f"0x{kind:02X}={count}" for kind, count in sorted(result["command_types"].items())
            ),
            f"Projection matrix groups: {len(result['projections'])}",
        ]
        for index, projection in enumerate(result["projections"][-6:]):
            values = projection["values"]
            nonzero = ", ".join(
                f"{i}={values[i]:.7g}" for i in sorted(values) if abs(values[i]) > 1e-8
            ) or "all zero"
            report.append(
                f"Projection group {index + max(0, len(result['projections']) - 6)}: "
                f"{projection['writes']}/16 writes; nonzero {nonzero}"
            )
        if args.core_log:
            report.extend(["", *compare_core(args.core_log, result["projections"])])
        expected_corners = ((-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0))
        for index, projection in enumerate(result["projections"]):
            values = projection["values"]
            if len(values) != 16:
                continue
            matrix = [values[i] for i in range(16)]
            projected = [transform_projection(point, matrix) for point in FULLSCREEN_QUAD]
            if all(
                point is not None and all(
                    math.isclose(actual, expected, rel_tol=1e-3, abs_tol=1e-3)
                    for actual, expected in zip(point, target)
                )
                for point, target in zip(projected, expected_corners)
            ):
                report.append(
                    f"PPSSPP screen-space projection group {index} maps the 480x272 "
                    f"corners to NDC {json.dumps(projected)}."
                )
        for quad in result["fullscreen_quads"][:8]:
            projected = [
                transform_projection(point, quad["projection"])
                for point in FULLSCREEN_QUAD
            ]
            report.append(
                f"PPSSPP fullscreen quad at operation {quad['operation']}: "
                f"vtype=0x{quad['vtype']:06X}, stride={quad['stride']}, "
                f"position offset={quad['position_offset']}; "
                f"NDC corners={json.dumps(projected)}"
            )
        rendered = "\n".join(report) + "\n"
        print(rendered, end="")
        if args.output:
            args.output.write_text(rendered, encoding="utf-8")
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"PPSSPP recording analysis failed: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
