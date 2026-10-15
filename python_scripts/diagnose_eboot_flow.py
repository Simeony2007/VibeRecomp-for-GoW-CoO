#!/usr/bin/env python3
"""Correlate ELF code around a BREAK/call target with an emulator instruction trace.

This is a trace-driven flow diagnostic, not a replacement CPU emulator. It
uses the instruction trace as the executed path and the EBOOT ELF as the
disassembly source, so relocations and branch outcomes can be inspected
together.

Example:
    python python_scripts/diagnose_eboot_flow.py umd0/PSP_GAME/SYSDIR/EBOOT.BIN \
        --trace artifacts/runs/LogI17.trace --pc 0x088056AC
"""

import argparse
import re
import struct
import sys
from pathlib import Path


BIAS = 0x08800000
REGISTERS = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
)
STEP_RE = re.compile(r"^TRACE STEP (.*)$")
HLE_RE = re.compile(r"^TRACE HLE (.*)$")
FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")


def fields(text):
    return dict(FIELD_RE.findall(text))


def normalize_address(address):
    if 0 < address < 0x04000000:
        return address + BIAS
    return address


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def disassemble(word, pc):
    opcode = word >> 26
    rs, rt = (word >> 21) & 31, (word >> 16) & 31
    rd, shamt, funct = (word >> 11) & 31, (word >> 6) & 31, word & 63
    imm = signed16(word & 0xFFFF)
    r = REGISTERS
    if word == 0:
        return "nop"
    if opcode == 0:
        names = {
            0x08: f"jr ${r[rs]}",
            0x09: f"jalr ${r[rd]}, ${r[rs]}",
            0x0C: "syscall",
            0x0D: "break",
            0x21: f"addu ${r[rd]}, ${r[rs]}, ${r[rt]}",
            0x23: f"subu ${r[rd]}, ${r[rs]}, ${r[rt]}",
            0x25: f"or ${r[rd]}, ${r[rs]}, ${r[rt]}",
            0x2B: f"sltu ${r[rd]}, ${r[rs]}, ${r[rt]}",
        }
        if funct in (0, 2, 3):
            op = {0: "sll", 2: "srl", 3: "sra"}[funct]
            return f"{op} ${r[rd]}, ${r[rt]}, {shamt}"
        return names.get(funct, f"special_{funct:02x}")
    if opcode in (2, 3):
        raw = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        target = normalize_address(raw)
        op = "jal" if opcode == 3 else "j"
        return f"{op} 0x{target:08X}"
    branch_names = {4: "beq", 5: "bne", 6: "blez", 7: "bgtz",
                    1: "regimm"}
    if opcode in branch_names:
        target = normalize_address(pc + 4 + imm * 4)
        if opcode == 1:
            kind = "bgezal" if rt == 17 else "bltzal" if rt == 16 else "bltz/bgez"
            return f"{kind} ${r[rs]}, 0x{target:08X}"
        if opcode in (6, 7):
            return f"{branch_names[opcode]} ${r[rs]}, 0x{target:08X}"
        return f"{branch_names[opcode]} ${r[rs]}, ${r[rt]}, 0x{target:08X}"
    immediate = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu",
                 12: "andi", 13: "ori", 14: "xori", 15: "lui"}
    if opcode in immediate:
        value = word & 0xFFFF if opcode in (12, 13, 14, 15) else imm
        if opcode == 15:
            return f"lui ${r[rt]}, 0x{value:04X}"
        return f"{immediate[opcode]} ${r[rt]}, ${r[rs]}, {value}"
    memory = {32: "lb", 33: "lh", 35: "lw", 36: "lbu", 37: "lhu",
              40: "sb", 41: "sh", 43: "sw"}
    if opcode in memory:
        return f"{memory[opcode]} ${r[rt]}, {imm}(${r[rs]})"
    return f"op_{opcode:02x}"


def load_segments(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError(f"{path} is not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ValueError("Expected a 32-bit little-endian PSP ELF")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segments = []
    for index in range(phnum):
        off = phoff + index * phentsize
        p_type, p_offset, vaddr, _, filesz, memsz, _, _ = struct.unpack_from(
            "<IIIIIIII", data, off
        )
        if p_type == 1:
            segments.append((vaddr + BIAS, p_offset, filesz, memsz))
    return data, segments


def word_at(data, segments, address):
    address = normalize_address(address)
    for vaddr, offset, filesz, _ in segments:
        if vaddr <= address and address + 4 <= vaddr + filesz:
            return struct.unpack_from("<I", data, offset + address - vaddr)[0]
    return None


def trace_records(path):
    records = []
    hle = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.rstrip("\n")
            step = STEP_RE.match(line)
            if step:
                item = fields(step.group(1))
                try:
                    item["seq"] = int(item["seq"])
                    item["pc_num"] = int(item["pc"], 16)
                    item["inst_num"] = int(item["inst"], 16)
                    item["gpr_num"] = [int(value, 16) for value in item["gpr"].split(",")]
                    item["line"] = line_number
                except (KeyError, ValueError):
                    continue
                records.append(item)
            event = HLE_RE.match(line)
            if event:
                item = fields(event.group(1))
                try:
                    item["at"] = int(item["at"])
                    item["id_num"] = int(item["id"], 16)
                    item["result_num"] = int(item["result"], 16)
                    item["line"] = line_number
                    hle.append(item)
                except (KeyError, ValueError):
                    continue
    return records, hle


def print_static_context(data, segments, pc, radius):
    print(f"\nStatic EBOOT instructions around 0x{pc:08X}:")
    for address in range(pc - radius * 4, pc + (radius + 1) * 4, 4):
        word = word_at(data, segments, address)
        if word is None:
            text = "<outside file-backed PT_LOAD>"
        else:
            text = f"{word:08X}  {disassemble(word, address)}"
        marker = "  <== target" if address == pc else ""
        print(f"  0x{address:08X}: {text}{marker}")


def print_executed_context(records, hle, pc, radius):
    matches = [index for index, item in enumerate(records)
               if item["pc_num"] == pc]
    if not matches:
        print(f"\nTrace: no instruction at 0x{pc:08X}.")
        return
    index = matches[-1]
    first = max(0, index - radius)
    last = min(len(records), index + radius + 1)
    print(f"\nExecuted trace context (step {records[index]['seq']}, "
          f"record {index + 1}/{len(records)}):")
    for item in records[first:last]:
        inst = item["inst_num"]
        addr = item["pc_num"]
        args = item["gpr_num"]
        marker = " <== target" if addr == pc else ""
        print(f"  seq={item['seq']:>7} pc=0x{addr:08X} inst={inst:08X} "
              f"{disassemble(inst, addr):34} "
              f"a0={args[4]:08X} a1={args[5]:08X} "
              f"a2={args[6]:08X} a3={args[7]:08X} ra={args[31]:08X}{marker}")

    calls = []
    for index, item in enumerate(records):
        inst = item["inst_num"]
        if inst >> 26 == 3:
            target = normalize_address(((item["pc_num"] + 4) & 0xF0000000) |
                                       ((inst & 0x03FFFFFF) << 2))
            if target == pc:
                calls.append(item)
    if calls:
        print(f"\nJAL call sites targeting 0x{pc:08X}:")
        for call in calls[-8:]:
            regs = call["gpr_num"]
            print(f"  seq={call['seq']} caller=0x{call['pc_num']:08X} "
                  f"args=({regs[4]:08X},{regs[5]:08X},{regs[6]:08X},{regs[7]:08X}) "
                  f"ra=0x{regs[31]:08X}")

    prior_hle = [event for event in hle
                 if int(event.get("pc", "0"), 16) <= 0xFFFFFFFF and
                 event["at"] <= records[index]["seq"]]
    if prior_hle:
        print("\nMost recent HLE events before the target:")
        for event in prior_hle[-12:]:
            print(f"  at={event['at']} pc={event['pc']} syscall=0x{event['id_num']:05X} "
                  f"args={event.get('args', '?')} result=0x{event['result_num']:08X}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("eboot", type=Path, help="decrypted PSP ELF/EBOOT.BIN")
    parser.add_argument("--trace", type=Path, help="MIPS_TRACE_FILE output")
    parser.add_argument("--pc", type=lambda value: int(value, 0),
                        help="address to inspect; defaults to BREAK in trace or 0x088056AC")
    parser.add_argument("--radius", type=int, default=12,
                        help="instruction radius around target (default: 12)")
    args = parser.parse_args()
    if args.radius < 0:
        parser.error("--radius must be non-negative")
    if not args.eboot.is_file():
        parser.error(f"EBOOT does not exist: {args.eboot}")
    if args.trace is not None and not args.trace.is_file():
        parser.error(f"trace does not exist: {args.trace}")

    try:
        data, segments = load_segments(args.eboot)
        records, hle = trace_records(args.trace) if args.trace else ([], [])
        pc = args.pc
        if pc is None:
            break_record = next((item for item in reversed(records)
                                 if item["inst_num"] & 0x3F == 0x0D and
                                 item["inst_num"] >> 26 == 0), None)
            if break_record:
                pc = break_record["pc_num"]
            elif records:
                pc = next((item["pc_num"] for item in reversed(records)
                           if word_at(data, segments, item["pc_num"]) is not None),
                          records[-1]["pc_num"])
                print("Trace contains no BREAK; inspecting its last instruction "
                      "mapped to a file-backed PT_LOAD segment.")
            else:
                pc = 0x088056AC
        print(f"EBOOT: {args.eboot} ({len(data)} bytes)")
        print(f"PT_LOAD segments: {len(segments)}; load bias: 0x{BIAS:08X}")
        word = word_at(data, segments, pc)
        print(f"Target 0x{pc:08X}: " +
              (f"0x{word:08X} {disassemble(word, pc)}" if word is not None
               else "not present in a file-backed PT_LOAD"))
        print_static_context(data, segments, pc, args.radius)
        if args.trace is not None:
            print(f"\nTrace: {args.trace} ({len(records)} instruction records, "
                  f"{len(hle)} HLE events)")
            print_executed_context(records, hle, pc, args.radius)
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
