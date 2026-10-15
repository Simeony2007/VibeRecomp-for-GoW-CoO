#!/usr/bin/env python3
"""Compare the observed null-load path in AOT/interpreter traces with PPSSPP policy."""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


ENTRY_PC = 0x089E3CB8
NULL_FIELD_LOAD_PC = 0x089E3CD0
GLOBAL_STORE_PC = 0x089E3CEC
FAULT_PC = 0x089E3CF0
FAULT_ADDRESS = 0x00000028
GAME_ID = "UCUS98653"

STEP_RE = re.compile(
    r"^TRACE STEP seq=(\d+) pc=([0-9A-Fa-f]+) next=([0-9A-Fa-f]+) "
    r"inst=([0-9A-Fa-f]+).* gpr=([0-9A-Fa-f,]+)$"
)
MEM_RE = re.compile(
    r"^TRACE MEM at=(\d+) pc=([0-9A-Fa-f]+) op=([RW])(\d+) "
    r"addr=([0-9A-Fa-f]+) value=([0-9A-Fa-f]+)$"
)
FAULT_RE = re.compile(
    r"^TRACE FAULT at=(\d+) pc=([0-9A-Fa-f]+) addr=([0-9A-Fa-f]+) "
    r"access=([0-9A-Fa-f]+) count=(\d+)$"
)


@dataclass
class Step:
    sequence: int
    pc: int
    next_pc: int
    instruction: int
    registers: list


@dataclass
class Access:
    sequence: int
    pc: int
    operation: str
    width: int
    address: int
    value: int


@dataclass
class Fault:
    sequence: int
    pc: int
    address: int
    access: int


@dataclass
class Trace:
    path: Path
    mode: str = "unknown"
    steps: list = field(default_factory=list)
    accesses: dict = field(default_factory=dict)
    faults: list = field(default_factory=list)


def parse_trace(path):
    trace = Trace(path=Path(path))
    with trace.path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("TRACE HEADER "):
                mode_match = re.search(r"\bmode=(\S+)", line)
                if mode_match:
                    trace.mode = mode_match.group(1)
                continue
            match = STEP_RE.match(line.rstrip())
            if match:
                registers = [int(value, 16) for value in match.group(5).split(",")]
                if len(registers) != 32:
                    raise ValueError(
                        f"{trace.path}: expected 32 GPR values, got {len(registers)}"
                    )
                trace.steps.append(
                    Step(
                        sequence=int(match.group(1)),
                        pc=int(match.group(2), 16),
                        next_pc=int(match.group(3), 16),
                        instruction=int(match.group(4), 16),
                        registers=registers,
                    )
                )
                continue
            match = MEM_RE.match(line.rstrip())
            if match:
                access = Access(
                    sequence=int(match.group(1)),
                    pc=int(match.group(2), 16),
                    operation=match.group(3),
                    width=int(match.group(4)),
                    address=int(match.group(5), 16),
                    value=int(match.group(6), 16),
                )
                trace.accesses.setdefault(access.sequence, []).append(access)
                continue
            match = FAULT_RE.match(line.rstrip())
            if match:
                trace.faults.append(
                    Fault(
                        sequence=int(match.group(1)),
                        pc=int(match.group(2), 16),
                        address=int(match.group(3), 16),
                        access=int(match.group(4), 16),
                    )
                )
    if not trace.steps:
        raise ValueError(f"{trace.path}: no TRACE STEP records found")
    return trace


def sign_extend_16(value):
    return value - 0x10000 if value & 0x8000 else value


def decode(instruction):
    opcode = instruction >> 26
    rs = (instruction >> 21) & 31
    rt = (instruction >> 16) & 31
    rd = (instruction >> 11) & 31
    immediate = sign_extend_16(instruction & 0xFFFF)
    if opcode == 0x09:
        return f"addiu r{rt},r{rs},{immediate}"
    if opcode == 0x0F:
        return f"lui r{rt},0x{instruction & 0xFFFF:04X}"
    if opcode == 0x23:
        return f"lw r{rt},{immediate}(r{rs})"
    if opcode == 0x2B:
        return f"sw r{rt},{immediate}(r{rs})"
    if opcode == 0 and (instruction & 0x3F) == 0x25:
        return f"or r{rd},r{rs},r{rt}"
    return f"unsupported 0x{instruction:08X}"


def fault_context(trace, fault):
    fault_index = next(
        (
            index
            for index, step in enumerate(trace.steps)
            if step.sequence == fault.sequence and step.pc == fault.pc
        ),
        None,
    )
    if fault_index is None:
        raise ValueError(f"{trace.path}: no instruction record for fault sequence")
    start_index = next(
        (
            index
            for index in range(fault_index - 1, -1, -1)
            if trace.steps[index].pc == ENTRY_PC
        ),
        None,
    )
    if start_index is None:
        raise ValueError(f"{trace.path}: no function entry before fault")
    steps = trace.steps[start_index : fault_index + 1]
    expected_pcs = [
        step.pc for step in steps if NULL_FIELD_LOAD_PC <= step.pc <= FAULT_PC
    ]
    if NULL_FIELD_LOAD_PC not in expected_pcs or FAULT_PC not in expected_pcs:
        raise ValueError(f"{trace.path}: incomplete path from null-field load to fault")
    return start_index, fault_index, steps


def simulate_observed_path(trace, fault):
    start_index, fault_index, path = fault_context(trace, fault)
    registers = path[0].registers.copy()
    rendered = []
    for index in range(start_index, fault_index + 1):
        step = trace.steps[index]
        if index != start_index and registers != step.registers:
            raise ValueError(
                f"{trace.path}: replay diverged before 0x{step.pc:08X}"
            )
        instruction = step.instruction
        opcode = instruction >> 26
        rs = (instruction >> 21) & 31
        rt = (instruction >> 16) & 31
        rd = (instruction >> 11) & 31
        immediate = sign_extend_16(instruction & 0xFFFF)
        events = trace.accesses.get(step.sequence, [])

        if opcode == 0x09:  # ADDIU
            if rt:
                registers[rt] = (registers[rs] + immediate) & 0xFFFFFFFF
        elif opcode == 0x0F:  # LUI
            if rt:
                registers[rt] = (instruction & 0xFFFF) << 16
        elif opcode == 0 and (instruction & 0x3F) == 0x25:  # OR
            if rd:
                registers[rd] = registers[rs] | registers[rt]
        elif opcode == 0x23:  # LW
            address = (registers[rs] + immediate) & 0xFFFFFFFF
            if step.pc == FAULT_PC:
                if address != fault.address or address != FAULT_ADDRESS:
                    raise ValueError(
                        f"{trace.path}: expected null read at 0x{FAULT_ADDRESS:08X}, "
                        f"got 0x{address:08X}"
                    )
                if fault.access != 0x104:
                    raise ValueError(f"{trace.path}: fault is not a 32-bit read")
                value = 0  # Same ignored-bad-read result used by PPSSPP.
            else:
                reads = [
                    event
                    for event in events
                    if event.operation == "R" and event.width == 32
                ]
                if len(reads) != 1 or reads[0].address != address:
                    raise ValueError(
                        f"{trace.path}: LW at 0x{step.pc:08X} has no matching "
                        "32-bit memory-read trace"
                    )
                value = reads[0].value
            if rt:
                registers[rt] = value
        elif opcode == 0x2B:  # SW
            address = (registers[rs] + immediate) & 0xFFFFFFFF
            writes = [
                event
                for event in events
                if event.operation == "W" and event.width == 32
            ]
            if len(writes) != 1 or writes[0].address != address:
                raise ValueError(
                    f"{trace.path}: SW at 0x{step.pc:08X} has no matching "
                    "32-bit memory-write trace"
                )
            value = registers[rt]
            if writes[0].value != value:
                raise ValueError(
                    f"{trace.path}: SW value mismatch at 0x{step.pc:08X}"
                )
        else:
            raise ValueError(
                f"{trace.path}: unsupported instruction 0x{instruction:08X} "
                f"at 0x{step.pc:08X}"
            )

        rendered.append((step.pc, instruction, registers.copy()))
        if index < fault_index and registers != trace.steps[index + 1].registers:
            raise ValueError(
                f"{trace.path}: simulated GPRs differ after 0x{step.pc:08X}"
            )

    null_load = next(item for item in rendered if item[0] == NULL_FIELD_LOAD_PC)
    stored_zero = [
        access
        for seq, accesses in trace.accesses.items()
        if start_index <= next(
            (i for i, s in enumerate(trace.steps) if s.sequence == seq), fault_index
        ) <= fault_index
        for access in accesses
        if access.pc == GLOBAL_STORE_PC
    ]
    if not stored_zero or stored_zero[-1].value != 0:
        raise ValueError(f"{trace.path}: global pointer store of zero not confirmed")
    if null_load[2][4] != 0:
        raise ValueError(f"{trace.path}: the +4 field was not loaded as null")
    return path, rendered


def signature(trace):
    relevant = [
        (step.pc, step.instruction, tuple(step.registers))
        for step in trace.steps
        if NULL_FIELD_LOAD_PC <= step.pc <= FAULT_PC
    ]
    memory = [
        (access.pc, access.operation, access.width, access.address, access.value)
        for sequence in sorted(trace.accesses)
        for access in trace.accesses[sequence]
        if NULL_FIELD_LOAD_PC <= access.pc <= FAULT_PC
    ]
    faults = [(fault.pc, fault.address, fault.access) for fault in trace.faults]
    return relevant, memory, faults


def psp_valid_word_address(address, null_page_enabled=False):
    if null_page_enabled and address < 0x00010000:
        return address % 4 == 0
    if (address & 0x3E000003) == 0x08000000:
        return True
    if (address & 0xBF800003) == 0x04000000:
        return True
    if (address & 0x3FFFC003) == 0x00010000:
        return True
    masked = address & 0x3FFFFFFF
    return 0x08000000 <= masked < 0x0A000000 and address % 4 == 0


def check_ppsspp_policy(trace, fault, null_page_enabled=False):
    _, fault_index, _ = fault_context(trace, fault)
    step = trace.steps[fault_index]
    base = step.registers[(step.instruction >> 21) & 31]
    offset = sign_extend_16(step.instruction & 0xFFFF)
    address = (base + offset) & 0xFFFFFFFF
    if address != FAULT_ADDRESS or psp_valid_word_address(
        address, null_page_enabled=null_page_enabled
    ):
        raise ValueError("PPSSPP address-validity model did not classify the read as invalid")
    next_step = (
        trace.steps[fault_index + 1]
        if fault_index + 1 < len(trace.steps)
        else None
    )
    if next_step is not None and next_step.pc != step.next_pc:
        next_step = None
    if next_step is None or next_step.registers[4] != 0:
        raise ValueError("PPSSPP ignored-read model did not produce a0=0 and advance")
    return address, next_step.pc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--aot",
        type=Path,
        default=Path("artifacts/runs/Run104-aot.trace"),
        help="AOT/hybrid trace file",
    )
    parser.add_argument(
        "--interpreter",
        type=Path,
        default=Path("artifacts/runs/Run104-interpreter.trace"),
        help="pure interpreter trace file",
    )
    parser.add_argument(
        "--ppsspp-null-page",
        action="store_true",
        help="simulate a PPSSPP per-game NullPageValid setting",
    )
    args = parser.parse_args()

    try:
        aot = parse_trace(args.aot)
        interpreter = parse_trace(args.interpreter)
        if "HYBRID_AOT" not in aot.mode:
            raise ValueError(f"{args.aot}: expected a hybrid AOT trace, got {aot.mode}")
        if interpreter.mode != "PURE_INTERPRETER":
            raise ValueError(
                f"{args.interpreter}: expected PURE_INTERPRETER, got {interpreter.mode}"
            )
        aot_faults = [fault for fault in aot.faults if fault.pc == FAULT_PC]
        interp_faults = [fault for fault in interpreter.faults if fault.pc == FAULT_PC]
        if not aot_faults or not interp_faults:
            raise ValueError("both traces must contain a fault at 0x089E3CF0")
        if signature(aot) != signature(interpreter):
            raise ValueError("AOT and interpreter trace signatures differ")

        for label, trace, faults in (
            ("AOT", aot, aot_faults),
            ("interpreter", interpreter, interp_faults),
        ):
            for occurrence, fault in enumerate(faults, 1):
                path, rendered = simulate_observed_path(trace, fault)
                address, resume_pc = check_ppsspp_policy(
                    trace, fault, null_page_enabled=args.ppsspp_null_page
                )
                zero_load_count = sum(
                    1 for pc, _, regs in rendered if pc == NULL_FIELD_LOAD_PC and regs[4] == 0
                )
                print(
                    f"{label} occurrence {occurrence}: replayed "
                    f"{len(path)} instructions from 0x{ENTRY_PC:08X}; "
                    f"field +4 -> 0, global store -> 0, "
                    f"lw at 0x{FAULT_PC:08X} reads 0x{address:08X}; "
                    f"PPSSPP-ignore model resumes at 0x{resume_pc:08X} "
                    f"with a0=0 (field-load checks={zero_load_count})"
                )

        print(
            "AOT/interpreter: matching registers and memory events in the "
            "captured fault window."
        )
        print(
            "PPSSPP: modeled source behavior only (default IgnoreBadMemAccess=true); "
            "this script does not launch PPSSPP."
        )
    except (OSError, ValueError) as error:
        print(f"diagnostic failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
