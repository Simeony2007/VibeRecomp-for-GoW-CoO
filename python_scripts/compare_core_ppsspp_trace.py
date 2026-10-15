#!/usr/bin/env python3
"""Compare a core TRACE STEP capture with a PPSSPP per-instruction JSON trace."""

import argparse
import json
import re
import sys


REGISTER_NAMES = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
)
FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
MEM_RE = re.compile(r"^TRACE MEM (.*)$")
LOAD_OPERAND_RE = re.compile(r",\s*(-?(?:0x[0-9a-fA-F]+|\d+))\((\w+)\)")
GPR_IMMEDIATE_OPS = {
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x18, 0x19, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x30, 0x32, 0x33, 0x37,
}
NON_GPR_R_FUNCTS = {
    0x08, 0x0C, 0x0D, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B,
}


def fields(text):
    return dict(FIELD_RE.findall(text))


def read_core(path, entry, target):
    rows = []
    mem_reads = {}
    started = False
    with open(path, "r", encoding="utf-8", errors="replace") as trace:
        for line_no, raw in enumerate(trace, 1):
            line = raw.rstrip("\n")
            match = re.match(r"^TRACE STEP (.*)$", line)
            if match:
                item = fields(match.group(1))
                if not started:
                    if int(item.get("pc", "0"), 16) != entry:
                        continue
                    started = True
                registers = item.get("gpr", "").split(",")
                if len(registers) != len(REGISTER_NAMES):
                    raise ValueError(f"{path}:{line_no}: expected 32 GPR values")
                item["gpr"] = {
                    name: int(value, 16)
                    for name, value in zip(REGISTER_NAMES, registers)
                }
                item["_line"] = line_no
                rows.append(item)
                if int(item["pc"], 16) == target:
                    break
                continue
            mem_match = MEM_RE.match(line)
            if mem_match and started:
                item = fields(mem_match.group(1))
                if item.get("op") == "R32":
                    mem_reads.setdefault(int(item["pc"], 16), item)
    if not rows or int(rows[0]["pc"], 16) != entry:
        raise ValueError(f"{path}: no instruction trace starting at 0x{entry:08X}")
    if int(rows[-1]["pc"], 16) != target:
        raise ValueError(f"{path}: trace ended before 0x{target:08X}")
    return rows, mem_reads


def read_ppsspp(path, entry, target):
    rows = []
    with open(path, "r", encoding="utf-8") as trace:
        header = trace.readline().rstrip("\n")
        if not header.startswith("PPSSPP_TRACE_HEADER "):
            raise ValueError(f"{path}: missing PPSSPP_TRACE_HEADER")
        for line_no, raw in enumerate(trace, 2):
            if not raw.strip():
                continue
            item = json.loads(raw)
            if item.get("type") != "instruction":
                continue
            item["_line"] = line_no
            rows.append(item)
    if not rows or rows[0].get("pc") != entry:
        raise ValueError(f"{path}: no instruction trace starting at 0x{entry:08X}")
    if rows[-1].get("pc") != target:
        raise ValueError(f"{path}: trace ended before 0x{target:08X}")
    return rows


def destination_register(instruction):
    opcode = instruction >> 26
    if opcode == 0:
        if instruction & 0x3F in NON_GPR_R_FUNCTS:
            return None
        return (instruction >> 11) & 0x1F
    if opcode == 3:
        return 31
    if opcode == 0x11 and (instruction >> 21) & 0x1F in (0, 2):
        return (instruction >> 16) & 0x1F
    if opcode in GPR_IMMEDIATE_OPS:
        return (instruction >> 16) & 0x1F
    return None


def mfc1_source_register(instruction):
    if instruction >> 26 != 0x11 or (instruction >> 21) & 0x1F != 0:
        return None
    return (instruction >> 11) & 0x1F


def changed_registers(core_regs, ppsspp_regs):
    changes = []
    for name in REGISTER_NAMES:
        a = core_regs[name]
        b = int(ppsspp_regs.get(name, 0))
        if a != b:
            changes.append((name, a, b))
    return changes


def mfc1_source_divergence(core, ppsspp):
    for index in range(min(len(core), len(ppsspp)) - 1):
        instruction = int(core[index]["inst"], 16)
        source = mfc1_source_register(instruction)
        if source is None:
            continue
        if int(core[index + 1]["pc"], 16) != int(ppsspp[index + 1]["pc"]):
            continue
        core_fprs = core[index].get("fpr")
        ppsspp_fprs = ppsspp[index].get("fpr")
        if not isinstance(core_fprs, str) or not isinstance(ppsspp_fprs, list):
            continue
        if source >= len(core_fprs.split(",")) or source >= len(ppsspp_fprs):
            continue
        core_fpr = int(core_fprs.split(",")[source], 16)
        ppsspp_fpr = int(ppsspp_fprs[source])
        destination = (instruction >> 16) & 0x1F
        name = REGISTER_NAMES[destination]
        core_value = core[index + 1]["gpr"][name]
        ppsspp_value = int(ppsspp[index + 1]["gpr"].get(name, 0))
        if core_fpr != ppsspp_fpr or core_value != ppsspp_value:
            return index, source, core_fpr, ppsspp_fpr, name, core_value, ppsspp_value
    return None


def instruction_result_divergences(core, ppsspp):
    first_exact = None
    first_nullness = None
    for index in range(min(len(core), len(ppsspp)) - 1):
        instruction = int(core[index]["inst"], 16)
        destination = destination_register(instruction)
        if destination is None:
            continue
        core_next = int(core[index + 1]["pc"], 16)
        ppsspp_next = int(ppsspp[index + 1]["pc"])
        if core_next != ppsspp_next:
            continue
        name = REGISTER_NAMES[destination]
        core_value = int(core[index + 1]["gpr"][name])
        ppsspp_value = int(ppsspp[index + 1]["gpr"].get(name, 0))
        if core_value != ppsspp_value and first_exact is None:
            first_exact = (index, name, core_value, ppsspp_value)
        if bool(core_value) != bool(ppsspp_value) and first_nullness is None:
            first_nullness = (index, name, core_value, ppsspp_value)
    return first_exact, first_nullness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core_trace", help="core TRACE STEP trace")
    parser.add_argument("ppsspp_trace", help="PPSSPP instruction JSON trace")
    parser.add_argument("--entry", type=lambda value: int(value, 0), default=0x089E7CB8)
    parser.add_argument("--target", type=lambda value: int(value, 0), default=0x089E7CF0)
    args = parser.parse_args()

    try:
        core, core_mem_reads = read_core(args.core_trace, args.entry, args.target)
        ppsspp = read_ppsspp(args.ppsspp_trace, args.entry, args.target)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"Erro ao ler traces: {error}", file=sys.stderr)
        return 2

    count = min(len(core), len(ppsspp))
    code_mismatch = None
    for index, (left, right) in enumerate(zip(core, ppsspp)):
        core_pc = int(left["pc"], 16)
        if core_pc != int(right["pc"]) or int(left["inst"], 16) != int(right["inst"]):
            code_mismatch = (index, left, right)
            break

    print("Comparacao de execucao: core x PPSSPP")
    print(f"  Core:    {args.core_trace} ({len(core)} instrucoes)")
    print(f"  PPSSPP:  {args.ppsspp_trace} ({len(ppsspp)} instrucoes)")
    print(f"  PCs e palavras de instrucao iguais: "
          f"{'sim' if code_mismatch is None and len(core) == len(ppsspp) else 'nao'}")
    entry_differences = changed_registers(core[0]["gpr"], ppsspp[0]["gpr"])
    entry_args_differ = any(
        name in ("a0", "a1", "a2", "a3")
        for name, _, _ in entry_differences
    )
    if entry_args_differ:
        print("  AVISO: argumentos da funcao diferem entre as capturas, que foram "
              "obtidas em execucoes separadas.")
        print("  Os ponteiros podem referenciar objetos diferentes; divergencias de "
              "dados abaixo sao candidatas, nao prova da causa raiz.")
    if code_mismatch:
        index, left, right = code_mismatch
        print(f"  Primeira divergencia de fluxo/codigo no indice {index + 1}:")
        print(f"    Core:   pc={left['pc']} inst={left['inst']}")
        print(f"    PPSSPP: pc=0x{int(right['pc']):08X} inst=0x{int(right['inst']):08X}")
        return 1
    if len(core) != len(ppsspp):
        print("  Os traces tem comprimentos diferentes.")
        return 1

    initial_changes = entry_differences
    if initial_changes:
        shown = ", ".join(
            f"${name}=0x{a:08X}/0x{b:08X}"
            for name, a, b in initial_changes[:8]
        )
        print("  Estado inicial difere (esperado entre execucoes/alocacoes separadas): "
              + shown)

    exact, nullness = instruction_result_divergences(core, ppsspp)
    mfc1_divergence = mfc1_source_divergence(core, ppsspp)
    if mfc1_divergence:
        index, source, core_fpr, ppsspp_fpr, reg, core_value, ppsspp_value = mfc1_divergence
        instruction = core[index]
        print(f"  Primeira diferenca COP1->GPR: pc=0x{int(instruction['pc'], 16):08X} "
              f"inst=0x{int(instruction['inst'], 16):08X}")
        print(f"    FPR${source}: Core=0x{core_fpr:08X}, PPSSPP=0x{ppsspp_fpr:08X}")
        print(f"    Destino ${reg}: Core=0x{core_value:08X}, "
              f"PPSSPP=0x{ppsspp_value:08X}")
    if exact is None:
        print("  Nenhum destino GPR diverge exatamente nas instrucoes alinhadas.")
        return 0

    index, reg, core_value, ppsspp_value = exact
    instruction = core[index]
    pc = int(instruction["pc"], 16)
    print(f"  Primeira diferenca absoluta de destino: pc=0x{pc:08X} "
          f"inst=0x{int(instruction['inst'], 16):08X} "
          f"{instruction.get('params', '')}")
    print(f"    Destino ${reg}: Core=0x{core_value:08X}, "
          f"PPSSPP=0x{ppsspp_value:08X}")

    if nullness is None:
        print("  Nenhum destino passa de zero para nao-zero (ou vice-versa).")
        return 0

    index, reg, core_value, ppsspp_value = nullness
    instruction = core[index]
    pc = int(instruction["pc"], 16)
    print(f"  Primeira diferenca zero/nao-zero: pc=0x{pc:08X} "
          f"inst=0x{int(instruction['inst'], 16):08X} "
          f"{instruction.get('params', '')}")
    print(f"    Destino ${reg}: Core=0x{core_value:08X}, "
          f"PPSSPP=0x{ppsspp_value:08X}")

    mem = core_mem_reads.get(pc)
    if mem:
        pp_item = ppsspp[index]
        operand = LOAD_OPERAND_RE.search(pp_item.get("params", ""))
        if operand:
            offset = int(operand.group(1), 0)
            base_reg = operand.group(2)
            base = int(pp_item["gpr"].get(base_reg, 0))
            effective_address = (base + offset) & 0xFFFFFFFF
            print(f"    Leitura Core:    [0x{int(mem['addr'], 16):08X}]"
                  f" -> 0x{int(mem['value'], 16):08X}")
            print(f"    Leitura PPSSPP:  [0x{effective_address:08X}]"
                  f" -> 0x{ppsspp_value:08X}")
    if entry_args_differ:
        print("  Interpretacao: o fluxo/instrucao coincide, mas os argumentos de entrada "
              "diferem. Nao e seguro atribuir esta diferenca de memoria a um bug do core "
              "sem alinhar a mesma chamada/objeto entre as execucoes.")
    else:
        print("  Interpretacao: o fluxo/instrucao e os argumentos iniciais coincidem; "
              "a diferenca de memoria merece investigacao no acesso ou na inicializacao.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
