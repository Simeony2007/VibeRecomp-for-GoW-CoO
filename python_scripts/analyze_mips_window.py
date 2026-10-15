#!/usr/bin/env python3
"""Summarize a PC-filtered instruction and memory trace."""

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
STEP_RE = re.compile(r"^TRACE STEP (.*)$")
MEM_RE = re.compile(r"^TRACE MEM (.*)$")
HLE_RE = re.compile(r"^TRACE HLE (.*)$")
REG_NAMES = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
)


def fields(text):
    return dict(FIELD_RE.findall(text))


def parse_registers(value):
    try:
        return tuple(int(register, 16) for register in value.split(","))
    except (AttributeError, ValueError):
        return ()


def decode_instruction(word, pc):
    opcode = word >> 26
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    funct = word & 0x3F
    immediate = word & 0xFFFF
    signed_immediate = immediate - 0x10000 if immediate & 0x8000 else immediate
    target = (pc + 4 + (signed_immediate << 2)) & 0xFFFFFFFF
    s = f"${REG_NAMES[rs]}"
    t = f"${REG_NAMES[rt]}"
    d = f"${REG_NAMES[rd]}"

    if opcode == 0 and funct == 8:
        return f"jr {s}"
    if opcode == 0 and funct == 9:
        return f"jalr {d}, {s}"
    if opcode in (4, 5, 0x14, 0x15):
        name = {4: "beq", 5: "bne", 0x14: "beql", 0x15: "bnel"}[opcode]
        return f"{name} {s}, {t}, 0x{target:08X}"
    if opcode == 9:
        return f"addiu {t}, {s}, {signed_immediate}"
    if opcode == 10:
        return f"slti {t}, {s}, {signed_immediate}"
    if opcode == 0x23:
        return f"lw {t}, {signed_immediate}({s})"
    if opcode == 0x2B:
        return f"sw {t}, {signed_immediate}({s})"
    return f".word 0x{word:08X}"


def analyze_lines(lines, tail_limit=40):
    steps = 0
    memory_events = 0
    hle_events = 0
    pc_counts = Counter()
    instructions = defaultdict(Counter)
    register_samples = defaultdict(Counter)
    memory_by_operation = Counter()
    memory_by_address = Counter()
    accesses_by_sequence = defaultdict(list)
    recent_steps = []
    repeated_states = Counter()
    first_step = None
    last_step = None
    markers = []

    for line_number, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        if line.startswith(("TRACE HEADER ", "TRACE LIMIT ", "TRACE END ",
                            "TRACE WATCH STOP ")):
            markers.append(line)

        match = STEP_RE.match(line)
        if match:
            record = fields(match.group(1))
            record["_line"] = line_number
            try:
                record["_seq"] = int(record["seq"])
                pc = int(record["pc"], 16)
            except (KeyError, ValueError):
                continue
            steps += 1
            pc_counts[pc] += 1
            registers = parse_registers(record.get("gpr"))
            if "inst" in record:
                try:
                    instructions[pc][int(record["inst"], 16)] += 1
                except ValueError:
                    pass
            if registers:
                repeated_states[(pc, record.get("next"), registers)] += 1
                register_samples[pc][registers] += 1
            if first_step is None:
                first_step = record
            last_step = record
            recent_steps.append(record)
            if len(recent_steps) > tail_limit:
                recent_steps.pop(0)
            continue

        match = MEM_RE.match(line)
        if match:
            record = fields(match.group(1))
            try:
                sequence = int(record["at"])
                address = int(record["addr"], 16)
            except (KeyError, ValueError):
                continue
            record["_line"] = line_number
            record["_seq"] = sequence
            memory_events += 1
            memory_by_operation[record.get("op", "?")] += 1
            memory_by_address[(record.get("op", "?"), address)] += 1
            accesses_by_sequence[sequence].append(record)
            continue

        if HLE_RE.match(line):
            hle_events += 1

    return {
        "steps": steps,
        "memory_events": memory_events,
        "hle_events": hle_events,
        "pc_counts": pc_counts,
        "instructions": instructions,
        "register_samples": register_samples,
        "memory_by_operation": memory_by_operation,
        "memory_by_address": memory_by_address,
        "accesses_by_sequence": accesses_by_sequence,
        "recent_steps": recent_steps,
        "repeated_states": repeated_states,
        "first_step": first_step,
        "last_step": last_step,
        "markers": markers,
        "tail_limit": tail_limit,
    }


def report(path, tail_limit=40):
    try:
        with path.open(encoding="utf-8", errors="replace") as stream:
            result = analyze_lines(stream, tail_limit)
    except OSError as exc:
        print(f"Erro lendo {path}: {exc}", file=sys.stderr)
        return 2

    print(f"Trace: {path}")
    for marker in result["markers"]:
        print(f"  {marker}")
    print(
        f"Registros: {result['steps']} instrucoes, "
        f"{result['memory_events']} acessos de memoria, "
        f"{result['hle_events']} eventos HLE"
    )

    first = result["first_step"]
    last = result["last_step"]
    if first and last:
        print(
            f"Janela executada: seq={first['_seq']} PC=0x{first['pc']} ate "
            f"seq={last['_seq']} PC=0x{last['pc']}"
        )
    if result["pc_counts"]:
        print("PCs mais frequentes:")
        for pc, count in result["pc_counts"].most_common(12):
            print(f"  0x{pc:08X}: {count}")
        print("Disassembly dos PCs mais frequentes:")
        for pc, _ in result["pc_counts"].most_common(12):
            decoded = ", ".join(
                f"{decode_instruction(word, pc)} (x{count})"
                for word, count in result["instructions"][pc].most_common(3)
            )
            print(f"  0x{pc:08X}: {decoded or '<instrucao ausente>'}")
            samples = result["register_samples"][pc]
            for registers, count in samples.most_common(2):
                print(
                    f"    estado x{count}: v1=0x{registers[3]:08X} "
                    f"a1=0x{registers[5]:08X} t0=0x{registers[8]:08X} "
                    f"t3=0x{registers[11]:08X} v0=0x{registers[2]:08X} "
                    f"ra=0x{registers[31]:08X}"
                )

    repeated = [
        (state, count) for state, count in result["repeated_states"].items()
        if count > 1
    ]
    print(f"Estados completos repetidos (PC/next/GPR): {len(repeated)}")
    for (pc, next_pc, registers), count in sorted(
        repeated, key=lambda item: item[1], reverse=True
    )[:8]:
        print(
            f"  PC=0x{pc:08X} next={next_pc} repeticoes={count} "
            f"v0=0x{registers[2]:08X} a0=0x{registers[4]:08X} "
            f"t0=0x{registers[8]:08X} t1=0x{registers[9]:08X} "
            f"ra=0x{registers[31]:08X}"
        )

    if result["memory_by_operation"]:
        print("Acessos por operacao:")
        for operation, count in sorted(result["memory_by_operation"].items()):
            print(f"  {operation}: {count}")
        print("Enderecos mais acessados:")
        for (operation, address), count in result["memory_by_address"].most_common(12):
            print(f"  {operation} 0x{address:08X}: {count}")

    print(f"Ultimas {len(result['recent_steps'])} instrucoes e acessos:")
    for step in result["recent_steps"]:
        seq = step["_seq"]
        print(
            f"  seq={seq} PC={step.get('pc')} inst={step.get('inst')} "
            f"next={step.get('next')} v0={_register(step, 2)} "
            f"v1={_register(step, 3)} a0={_register(step, 4)} "
            f"a1={_register(step, 5)} t0={_register(step, 8)} "
            f"t1={_register(step, 9)} t3={_register(step, 11)} "
            f"ra={_register(step, 31)}"
        )
        for access in result["accesses_by_sequence"].get(seq, ()):
            print(
                f"    {access.get('op')} addr={access.get('addr')} "
                f"value={access.get('value')}"
            )
    print(
        "Repeticao do estado completo e evidencia de loop fechado; repeticao "
        "apenas de PC pode ser um loop valido. MEM mostra os acessos observados, "
        "nao uma imagem completa da RAM."
    )
    return 0


def _register(step, index):
    registers = parse_registers(step.get("gpr"))
    return f"0x{registers[index]:08X}" if len(registers) > index else "?"


def self_test():
    trace = [
        "TRACE HEADER format=1 mode=test max=2\n",
        "TRACE STEP seq=1 pc=08A013C0 next=08A013C4 inst=8C820000 hi=0 lo=0 fcr31=0 gpr="
        + ",".join(["00000000"] * 32) + "\n",
        "TRACE MEM at=1 pc=08A013C0 op=R32 addr=08B30000 value=00000001\n",
        "TRACE STEP seq=2 pc=08A013C0 next=08A013C4 inst=8C820000 hi=0 lo=0 fcr31=0 gpr="
        + ",".join(["00000000"] * 32) + "\n",
        "TRACE LIMIT steps=2 last_pc=08A013C0\n",
    ]
    result = analyze_lines(trace, tail_limit=2)
    assert result["steps"] == 2
    assert result["memory_events"] == 1
    assert result["pc_counts"][0x08A013C0] == 2
    assert next(iter(result["repeated_states"].values())) == 2
    assert result["accesses_by_sequence"][1][0]["value"] == "00000001"
    print("analyze_mips_window self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("--tail", type=int, default=40)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.tail < 1:
        parser.error("--tail precisa ser positivo")
    if args.self_test:
        self_test()
        return 0
    if args.trace is None:
        parser.error("informe o arquivo trace ou use --self-test")
    return report(args.trace, args.tail)


if __name__ == "__main__":
    raise SystemExit(main())
