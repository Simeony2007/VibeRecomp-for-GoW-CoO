#!/usr/bin/env python3
"""Compare instruction traces produced by interpreter and AOT builds.

Example:
    python python_scripts/compare_mips_trace.py interpreter.trace aot.trace

The emulator writes detailed traces only when MIPS_TRACE_FILE is set. Optional
controls are MIPS_TRACE_START, MIPS_TRACE_END, MIPS_TRACE_MAX, MIPS_TRACE_FP=1,
MIPS_TRACE_MEMORY=1, and MIPS_TRACE_STOP=1. STOP requests CPU-loop termination
when MAX is reached; without it, MAX only disables further trace output.
For sparse loop diagnostics, MIPS_TRACE_WATCH_PC, MIPS_TRACE_WATCH_EVERY,
MIPS_TRACE_WATCH_SAMPLES, and MIPS_TRACE_WATCH_ONLY emit sampled registers and
the three words at $a0 without writing every instruction snapshot.
Addresses accept Python-style integers (for example, 0x08806000). This script
reports the earliest architectural instruction-state mismatch, then separately
checks HLE, memory read/write, and memory-fault events.
"""

import argparse
import itertools
import re
import sys


REGISTER_NAMES = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
)
STEP_RE = re.compile(r"^TRACE STEP (.*)$")
EVENT_RE = re.compile(r"^TRACE (HLE|MEM|FAULT) (.*)$")
WATCH_RE = re.compile(r"^TRACE WATCH sample=(.*)$")
FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
ARCH_FIELDS = ("pc", "next", "inst", "hi", "lo", "fcr31", "gpr", "fpr", "vfpu", "vcr")
WATCH_FIELDS = (
    "sample", "visits", "seq", "pc", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "v0", "ra", "mem_a0", "valid",
)


def parse_fields(text):
    return dict(FIELD_RE.findall(text))


def trace_records(path, wanted):
    step_pattern = STEP_RE if wanted == "STEP" else None
    event_pattern = EVENT_RE if wanted == "EVENT" else None
    watch_pattern = WATCH_RE if wanted == "WATCH" else None
    with open(path, "r", encoding="utf-8", errors="replace") as trace:
        for line_count, raw in enumerate(trace, 1):
            line = raw.rstrip("\n")
            if step_pattern is not None:
                match = step_pattern.match(line)
                if match:
                    fields = parse_fields(match.group(1))
                    fields["_line"] = line_count
                    yield fields
            elif event_pattern is not None:
                match = event_pattern.match(line)
                if match:
                    fields = parse_fields(match.group(2))
                    fields["_kind"] = match.group(1)
                    fields["_line"] = line_count
                    yield fields
            elif watch_pattern is not None:
                match = watch_pattern.match(line)
                if match:
                    fields = parse_fields(match.group(1))
                    fields["_line"] = line_count
                    yield fields


def compare_watch_samples(left_path, right_path):
    left = trace_records(left_path, "WATCH")
    right = trace_records(right_path, "WATCH")
    for index, (a, b) in enumerate(itertools.zip_longest(left, right), 1):
        if a is None or b is None:
            return index, a, b
        changes = [
            field for field in WATCH_FIELDS
            if a.get(field) != b.get(field)
        ]
        if changes:
            return index, a, b, changes
    return None


def trace_metadata(path):
    header = None
    end_marker = None
    steps = 0
    events = 0
    line_count = 0
    with open(path, "r", encoding="utf-8", errors="replace") as trace:
        for line_count, raw in enumerate(trace, 1):
            line = raw.rstrip("\n")
            if line.startswith("TRACE HEADER "):
                header = parse_fields(line[len("TRACE HEADER "):])
            elif line.startswith("TRACE END "):
                end_marker = parse_fields(line[len("TRACE END "):])
            elif STEP_RE.match(line):
                steps += 1
            elif EVENT_RE.match(line):
                events += 1
    return header, steps, events, line_count, end_marker


def changed_architecture(left, right):
    changed = []
    for field in ARCH_FIELDS:
        if left.get(field) == right.get(field):
            continue
        if field == "gpr" and field in left and field in right:
            lregs = left[field].split(",")
            rregs = right[field].split(",")
            for index, (lvalue, rvalue) in enumerate(itertools.zip_longest(lregs, rregs)):
                if lvalue != rvalue:
                    name = REGISTER_NAMES[index] if index < len(REGISTER_NAMES) else str(index)
                    changed.append(f"${name}: {lvalue or '<missing>'} -> {rvalue or '<missing>'}")
        elif field == "fpr" and field in left and field in right:
            lregs = left[field].split(",")
            rregs = right[field].split(",")
            for index, (lvalue, rvalue) in enumerate(itertools.zip_longest(lregs, rregs)):
                if lvalue != rvalue:
                    changed.append(f"$f{index}: {lvalue or '<missing>'} -> {rvalue or '<missing>'}")
        elif field == "vfpu" and field in left and field in right:
            lvectors = left[field].split(",")
            rvectors = right[field].split(",")
            for index, (lvalue, rvalue) in enumerate(itertools.zip_longest(lvectors, rvectors)):
                if lvalue != rvalue:
                    changed.append(f"VFPU[{index}]: {lvalue or '<missing>'} -> {rvalue or '<missing>'}")
        else:
            changed.append(f"{field}: {left.get(field, '<missing>')} -> {right.get(field, '<missing>')}")
    return changed


def explain(changes, left=None, right=None):
    fields = {change.split(":", 1)[0] for change in changes}
    if (fields == {"inst"} and left is not None and right is not None):
        left_inst = int(left.get("inst", "0"), 16)
        right_inst = int(right.get("inst", "0"), 16)
        if (left_inst & 0xFFFF0000) == (right_inst & 0xFFFF0000):
            return ("Apenas os 16 bits inferiores da instrucao diferem, com opcode e "
                    "registradores iguais. Isso e compativel com imediato HI16/LO16 "
                    "relocado em runtime: confirme a relocation nesse endereco e "
                    "compare a palavra da RAM, nao apenas a constante ELF do AOT.")
    if fields == {"next"}:
        if (left is not None and right is not None and
                (int(left.get("inst", "0"), 16) & 0xFC00003F) == 0x0000000C):
            return ("A divergencia ocorre ao entrar em uma instrucao SYSCALL: o AOT "
                    "entregou o alvo com next_pc igual ao proprio PC. O pipeline deve "
                    "entrar com next_pc=PC+4, senao o interpreter/HLE observa estado "
                    "diferente do interpreter puro.")
        return ("PC e instrucao coincidem, mas o proximo PC difere antes da primeira "
                "instrucao; confira a inicializacao do pipeline e se o entry e executado "
                "uma vez ou repetido.")
    if any(field in fields for field in ("pc", "inst")):
        return ("Divergencia de fluxo ou codigo executado: confira o estado anterior, "
                "branches/delay slots, relocacoes, dispatch e troca de thread.")
    if "next" in fields:
        return ("Divergencia no PC pendente: forte indicio de semantica de branch, "
                "delay slot ou retorno de syscall.")
    if any(field.startswith("$") or field == "gpr" for field in fields):
        return ("O PC/instrucao ainda coincide, mas o estado inteiro diverge: investigue "
                "a semantica da instrucao imediatamente anterior ou o retorno HLE.")
    if fields.intersection(("hi", "lo")):
        return "Divergencia em HI/LO: investigue MULT/DIV e leitura/escrita desses registradores."
    if fields.intersection(("fcr31", "fpr", "vfpu", "vcr")):
        return "Divergencia no estado FPU/VFPU; habilite MIPS_TRACE_FP=1 nos dois modos."
    return "Estado arquitetural diferente; confira as instrucoes imediatamente anteriores."


def compare_steps(left_path, right_path):
    left = trace_records(left_path, "STEP")
    right = trace_records(right_path, "STEP")
    for index, (a, b) in enumerate(itertools.zip_longest(left, right), 1):
        if a is None or b is None:
            present = b if a is None else a
            side = "Interpreter" if a is None else "AOT"
            return index, a, b, f"{side} nao tem mais instrucoes; proxima linha: {present}"
        changes = changed_architecture(a, b)
        if changes:
            return index, a, b, changes
        if a.get("seq") != b.get("seq"):
            return index, a, b, [f"seq: {a.get('seq')} -> {b.get('seq')}"]
    return None


def detect_repeated_entry(interpreter_path, aot_path):
    interpreter = list(itertools.islice(trace_records(interpreter_path, "STEP"), 3))
    aot = list(itertools.islice(trace_records(aot_path, "STEP"), 2))
    if len(interpreter) < 3 or len(aot) < 2:
        return None
    first, repeated, interpreter_after_entry = interpreter
    aot_first, aot_second = aot
    same_entry_instruction = (
        first.get("pc") == aot_first.get("pc") == repeated.get("pc") and
        first.get("inst") == aot_first.get("inst") == repeated.get("inst")
    )
    if (same_entry_instruction and aot_second.get("pc") != aot_first.get("pc") and
            interpreter_after_entry.get("pc") == aot_second.get("pc")):
        return first, repeated, interpreter_after_entry, aot_first, aot_second
    return None


def compare_events(left_path, right_path):
    left = trace_records(left_path, "EVENT")
    right = trace_records(right_path, "EVENT")
    for index, (a, b) in enumerate(itertools.zip_longest(left, right), 1):
        if a is None or b is None:
            return index, a, b
        comparable_a = {
            key: value for key, value in a.items()
            if key not in ("_line", "at")
        }
        comparable_b = {
            key: value for key, value in b.items()
            if key not in ("_line", "at")
        }
        if comparable_a != comparable_b:
            return index, a, b
    return None


def print_step_mismatch(result):
    if result is None:
        return
    index, left, right, detail = result
    if left is None or right is None:
        print(f"\nFIM DESIGUAL DOS TRACES (indice pareado {index})")
        ended_side = "Interpreter" if left is None else "AOT"
        active = right if left is None else left
        print(f"Nao foi encontrada divergencia nos {index - 1} passos pareados.")
        print(f"O trace {ended_side} terminou antes do outro.")
        print(f"Proximo passo disponivel no outro trace: seq={active.get('seq')} "
              f"pc={active.get('pc')} next={active.get('next')} inst={active.get('inst')}")
        return
    print(f"\nPRIMEIRA DIVERGENCIA DE INSTRUCAO (indice pareado {index})")
    print(f"  Interpreter linha {left['_line']}: seq={left.get('seq')} pc={left.get('pc')} "
          f"next={left.get('next')} inst={left.get('inst')}")
    print(f"  AOT         linha {right['_line']}: seq={right.get('seq')} pc={right.get('pc')} "
          f"next={right.get('next')} inst={right.get('inst')}")
    if isinstance(detail, list):
        for item in detail[:32]:
            print(f"  diferenca: {item}")
        if len(detail) > 32:
            print(f"  ... mais {len(detail) - 32} diferencas de estado")
        print(f"  Implicacao: {explain(detail, left, right)}")
    else:
        print(f"  Implicacao: {detail}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("interpreter_trace", help="trace do build PURE_INTERPRETER")
    parser.add_argument("aot_trace", help="trace do build HYBRID_AOT_INTERPRETER")
    args = parser.parse_args()

    try:
        left_header, left_step_count, left_event_count, left_lines, left_end = trace_metadata(args.interpreter_trace)
        right_header, right_step_count, right_event_count, right_lines, right_end = trace_metadata(args.aot_trace)
    except OSError as error:
        print(f"Erro ao ler trace: {error}", file=sys.stderr)
        return 2

    if not left_header or not right_header:
        print("Erro: um dos arquivos nao contem 'TRACE HEADER'; gere ambos com MIPS_TRACE_FILE.",
              file=sys.stderr)
        return 2
    if left_step_count == 0 or right_step_count == 0:
        left_watch_count = sum(1 for _ in trace_records(args.interpreter_trace, "WATCH"))
        right_watch_count = sum(1 for _ in trace_records(args.aot_trace, "WATCH"))
        if left_watch_count == 0 and right_watch_count == 0:
            print("Erro: um trace nao contem snapshots de instrucao nem amostras WATCH. "
                  "Verifique a execucao e os filtros MIPS_TRACE_START/MIPS_TRACE_END.",
                  file=sys.stderr)
            return 2
        if left_watch_count == 0 or right_watch_count == 0:
            print("Erro: os traces nao possuem amostras WATCH nos dois modos "
                  f"(Interpreter={left_watch_count}, AOT={right_watch_count}).",
                  file=sys.stderr)
            return 2

        print("Comparacao de amostras WATCH MIPS")
        print(f"  Interpreter: {args.interpreter_trace} "
              f"({left_watch_count} amostras, {left_lines} linhas)")
        print(f"  AOT:         {args.aot_trace} "
              f"({right_watch_count} amostras, {right_lines} linhas)")
        print(f"  modo Interpreter: {left_header.get('mode', 'desconhecido')}")
        print(f"  modo AOT:         {right_header.get('mode', 'desconhecido')}")
        watch_result = compare_watch_samples(
            args.interpreter_trace, args.aot_trace)
        if watch_result is None:
            print(f"Todas as {left_watch_count} amostras WATCH coincidem.")
        else:
            index, left, right, *details = watch_result
            print(f"PRIMEIRA DIFERENCA WATCH (amostra pareada {index})")
            print(f"  Interpreter: {left or '<sem amostra>'}")
            print(f"  AOT:         {right or '<sem amostra>'}")
            if details:
                print("  campos diferentes: " + ", ".join(details[0]))

        event_result = compare_events(args.interpreter_trace, args.aot_trace)
        if event_result is None:
            print(f"Eventos HLE/memoria/fault coincidem ({left_event_count} por lado).")
        else:
            event_index, left_event, right_event = event_result
            print(f"PRIMEIRA DIFERENCA DE EVENTO (indice {event_index})")
            print(f"  Interpreter: {left_event or '<sem evento>'}")
            print(f"  AOT:         {right_event or '<sem evento>'}")
        return 1 if watch_result is not None or event_result is not None else 0

    print("Comparacao de traces MIPS")
    print(f"  Interpreter: {args.interpreter_trace} ({left_step_count} steps, {left_lines} linhas)")
    print(f"  AOT:         {args.aot_trace} ({right_step_count} steps, {right_lines} linhas)")
    print(f"  fim Interpreter: {left_end or 'sem marcador TRACE END'}")
    print(f"  fim AOT:         {right_end or 'sem marcador TRACE END'}")
    if left_header:
        print(f"  modo Interpreter: {left_header.get('mode', 'desconhecido')}")
    if right_header:
        print(f"  modo AOT:         {right_header.get('mode', 'desconhecido')}")
    if left_header.get("mode") != "PURE_INTERPRETER":
        print("  AVISO: o primeiro arquivo nao identifica build PURE_INTERPRETER.")
    if right_header.get("mode") != "HYBRID_AOT_INTERPRETER":
        print("  AVISO: o segundo arquivo nao identifica build HYBRID_AOT_INTERPRETER.")

    repeated_entry = detect_repeated_entry(args.interpreter_trace, args.aot_trace)
    if repeated_entry is not None:
        first, repeated, interpreter_after_entry, aot_first, aot_second = repeated_entry
        print("\nDIAGNOSTICO DO ENTRY POINT")
        print(f"  Interpreter repetiu pc={repeated.get('pc')} inst={repeated.get('inst')} "
              f"nas linhas {first['_line']} e {repeated['_line']}.")
        print(f"  AOT avancou de pc={aot_first.get('pc')} para pc={aot_second.get('pc')} "
              "na segunda instrucao.")
        if interpreter_after_entry.get("gpr") and aot_second.get("gpr"):
            interpreter_sp = interpreter_after_entry["gpr"].split(",")[29]
            aot_sp = aot_second["gpr"].split(",")[29]
            if interpreter_sp != aot_sp:
                print(f"  $sp no PC alinhado {aot_second.get('pc')}: "
                      f"Interpreter={interpreter_sp}, "
                      f"AOT={aot_sp}. O prologo foi aplicado mais vezes no Interpreter.")
        print("  Causa: next_pc inicial igual ao entry fazia o pipeline buscar o entry duas vezes.")

    result = compare_steps(args.interpreter_trace, args.aot_trace)
    if result is None:
        print("\nNenhuma divergencia arquitetural nas instrucoes comuns.")
        if left_step_count != right_step_count:
            print(f"Os comprimentos diferem: Interpreter={left_step_count}, AOT={right_step_count}.")
        else:
            print(f"Os {left_step_count} snapshots comparados coincidem.")
    else:
        print_step_mismatch(result)
        if result[1] is None or result[2] is None:
            print("  Implicacao: trace de comprimento diferente nao prova divergencia de CPU. "
                  "Pode ser captura interrompida, processo encerrado ou execucao que terminou; "
                  "compare o marcador TRACE END e o log do emulador.")

    event_result = compare_events(args.interpreter_trace, args.aot_trace)
    if event_result is None:
        print(f"\nEventos HLE/memoria/fault coincidem ({left_event_count} por lado).")
    else:
        event_index, left_event, right_event = event_result
        print(f"\nPRIMEIRA DIFERENCA DE EVENTO (indice {event_index})")
        print(f"  Interpreter: {left_event or '<sem evento>'}")
        print(f"  AOT:         {right_event or '<sem evento>'}")
        if left_event and right_event and left_event.get("_kind") == "HLE":
            stable = ("pc", "id", "args", "result")
            if all(left_event.get(key) == right_event.get(key) for key in stable):
                if left_event.get("ra") != right_event.get("ra"):
                    print("  Implicacao: chamada/retorno HLE coincidem, mas $ra diverge. "
                          "Se um valor esta em endereco relativo e o outro em 0x088xxxxx, "
                          "o AOT nao aplicou o load bias ao registrador de link.")
                if left_event.get("sp") != right_event.get("sp"):
                    print("  Implicacao: $sp ja divergiu antes/na chamada HLE; investigue "
                          "prologo, execucao duplicada ou ordem de troca de thread.")
            else:
                print("  Implicacao: confira NID/syscall, PC chamador, argumentos e retorno; "
                      "a primeira diferenca pode explicar o estado de CPU posterior.")

    return 1 if result is not None or event_result is not None else 0


if __name__ == "__main__":
    sys.exit(main())
