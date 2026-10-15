#!/usr/bin/env python3
"""Catalog C handlers and compare observed HLE calls/results from emulator logs.

This is static/differential analysis, not arbitrary C execution. Exhaustively
calling C functions requires valid contracts, emulator state, and safe test
fixtures; the script never fabricates arguments or invokes project code.

Optional contract file format:
{
  "0x80001": [
    {"args": ["0", "0", "0", "0"], "result": "1"}
  ]
}
"""

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


EXCLUDED_DIRS = {".git", ".vscode", "__pycache__", "output", "PPSSPP"}
FUNCTION_START = re.compile(
    r"(?m)^[ \t]*(?:(?:static|inline|extern|__attribute__)\s+)*"
    r"(?:[A-Za-z_]\w*(?:\s*\*)*\s+)+(?P<name>[A-Za-z_]\w*)\s*\("
)
CONTROL_KEYWORDS = {"if", "for", "while", "switch"}
CASE_START = re.compile(r"\bcase\s+(0x[0-9A-Fa-f]+)\s*:")
SYSCALL_IF = re.compile(r"\bif\s*\((?P<condition>[^)]*\bsyscall_id\b[^)]*)\)")
LINKER_ENTRY = re.compile(
    r"syscall_code\s*=\s*(0x[0-9A-Fa-f]+)\s*;\s*func_name\s*=\s*\"([^\"]+)\""
)
TRACE_EVENT = re.compile(
    r"TRACE HLE at=(\d+)\s+pc=([0-9A-Fa-f]+)\s+id=([0-9A-Fa-f]+)\s+"
    r"args=([0-9A-Fa-f,]+)\s+result=([0-9A-Fa-f]+)"
)
CONSOLE_EVENT = re.compile(
    r"\[HLE TRACE\].*?syscall=0x([0-9A-Fa-f]+)\s+"
    r"args=\(([^)]*)\)\s+v0=0x([0-9A-Fa-f]+).*?caller_pc=0x([0-9A-Fa-f]+)"
)


def strip_comments_and_literals(text):
    token = re.compile(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return token.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), text)


def find_matching(text, start, opening, closing):
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    return -1


def discover_functions(source):
    clean = strip_comments_and_literals(source)
    functions = []
    for match in FUNCTION_START.finditer(clean):
        if match.group("name") in CONTROL_KEYWORDS:
            continue
        open_paren = clean.find("(", match.start("name"))
        close_paren = find_matching(clean, open_paren, "(", ")")
        if close_paren < 0:
            continue
        cursor = close_paren + 1
        while cursor < len(clean) and clean[cursor].isspace():
            cursor += 1
        if cursor >= len(clean) or clean[cursor] != "{":
            continue
        close_brace = find_matching(clean, cursor, "{", "}")
        if close_brace < 0:
            continue
        functions.append({
            "name": match.group("name"),
            "body": clean[cursor + 1:close_brace],
            "line": clean.count("\n", 0, match.start()) + 1,
        })
    return functions


def classify_body(body, annotations=""):
    evidence = (body + " " + annotations).lower()
    if re.search(r"\b(unimplemented|not implemented|todo|bypass|stub|mock)\b", evidence):
        return "POSSIBLE STUB/SHIM"
    compact = re.sub(r"\s+", " ", body)
    if re.fullmatch(
        r"(?:\s*(?:return\s+(?:0|nullptr|null|false|true|[-+]?0x[0-9a-f]+|[-+]?\d+)\s*;"
        r"|[A-Za-z_]\w*->(?:gpr|pc|next_pc)\s*=\s*[^;]+;)\s*)+",
        compact,
        re.IGNORECASE,
    ):
        return "POSSIBLE BYPASS"
    if re.search(r"\b(for|while|switch|memcpy|memset|read|write|open|close|pthread_|"
                 r"mips_(?:read|write|get_ptr)|hle_[A-Za-z_]\w*)\b", body):
        return "EXECUTES LOGIC"
    if re.search(r"(?:\+\+|--|(?<![=!<>])=(?!=)|\breturn\b)", body):
        return "EXECUTES LOGIC"
    return "UNCERTAIN"


def module_for(name, path):
    candidate = name.removeprefix("hle_")
    match = re.match(r"(sce[A-Z][A-Za-z0-9]*?)(?:[A-Z][a-z]|$)", candidate)
    if match:
        return match.group(1)
    return path.stem


def source_files(root, include_vendored):
    for path in sorted(root.rglob("*.c")):
        relative = path.relative_to(root)
        if not include_vendored and (
            any(part in EXCLUDED_DIRS for part in relative.parts[:-1])
            or path.name.endswith("OLD.c")
            or path.name.startswith("gow_core_aot")
        ):
            continue
        yield path


def parse_source_catalog(root, include_vendored):
    catalog = {}
    file_contents = {}
    for path in source_files(root, include_vendored):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print(f"[WARN] Nao foi possivel ler {path}: {error}", file=sys.stderr)
            continue
        relative = path.relative_to(root).as_posix()
        file_contents[relative] = text
        for function in discover_functions(text):
            function["file"] = relative
            function["classification"] = classify_body(function["body"])
            catalog[function["name"]] = function
    return catalog, file_contents


def parse_handlers(file_contents):
    handlers = defaultdict(lambda: {"names": set(), "bodies": []})
    for path, text in file_contents.items():
        clean = strip_comments_and_literals(text)
        entries = []
        for match in LINKER_ENTRY.finditer(text):
            handlers[int(match.group(1), 16)]["names"].add(match.group(2))
        for match in CASE_START.finditer(clean):
            prefix = clean[:match.start()]
            switch_match = list(re.finditer(r"\bswitch\s*\(\s*syscall(?:_id)?\s*\)", prefix))
            if not switch_match:
                continue
            switch_open = clean.find("{", switch_match[-1].end())
            switch_close = find_matching(clean, switch_open, "{", "}") if switch_open >= 0 else -1
            if switch_close >= match.start():
                entries.append((match.start(), match.end(), [int(match.group(1), 16)]))
        for match in SYSCALL_IF.finditer(clean):
            syscall_ids = [int(value, 16) for value in
                           re.findall(r"0x[0-9A-Fa-f]+", match.group("condition"))]
            if syscall_ids:
                entries.append((match.start(), match.end(), syscall_ids))
        entries.sort()
        for index, (start, end, syscall_ids) in enumerate(entries):
            next_start = entries[index + 1][0] if index + 1 < len(entries) else min(
                len(clean), end + 6000
            )
            code_chunk = clean[start:next_start]
            called = re.findall(r"\b(hle_[A-Za-z_]\w*)\s*\(", code_chunk)
            for syscall_id in syscall_ids:
                handlers[syscall_id]["names"].update(called)
                handlers[syscall_id]["bodies"].append((path, code_chunk))
    return handlers


def parse_number(value):
    value = value.strip()
    return int(value, 16) if value.lower().startswith("0x") else int(value, 10)


def read_events(paths):
    events = []
    for path in paths:
        try:
            with path.open("r", encoding="utf-8", errors="replace") as stream:
                for line_number, line in enumerate(stream, 1):
                    match = TRACE_EVENT.search(line)
                    if match:
                        events.append({
                            "file": path.name,
                            "line": line_number,
                            "pc": int(match.group(2), 16),
                            "id": int(match.group(3), 16),
                            "args": tuple(int(v, 16) for v in match.group(4).split(",")),
                            "result": int(match.group(5), 16),
                        })
                        continue
                    match = CONSOLE_EVENT.search(line)
                    if match:
                        args = tuple(parse_number(value) for value in
                                     re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", match.group(2)))
                        events.append({
                            "file": path.name,
                            "line": line_number,
                            "pc": int(match.group(4), 16),
                            "id": int(match.group(1), 16),
                            "args": args,
                            "result": int(match.group(3), 16),
                        })
        except OSError as error:
            print(f"[ERROR] Nao foi possivel ler {path}: {error}", file=sys.stderr)
            raise
    return events


def load_contracts(path):
    if path is None:
        return {}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"Contrato invalido em {path}: {error}") from error
    if not isinstance(raw, dict):
        raise ValueError("O arquivo de contratos precisa ser um objeto JSON.")
    contracts = {}
    for key, cases in raw.items():
        syscall_id = parse_number(key)
        if not isinstance(cases, list):
            raise ValueError(f"Contrato {key}: esperado uma lista de casos.")
        contracts[syscall_id] = []
        for case in cases:
            if not isinstance(case, dict) or "args" not in case or "result" not in case:
                raise ValueError(f"Contrato {key}: cada caso precisa de args e result.")
            args = tuple(parse_number(str(value)) for value in case["args"])
            if len(args) != 4:
                raise ValueError(f"Contrato {key}: args deve conter os quatro argumentos.")
            contracts[syscall_id].append((args, parse_number(str(case["result"]))))
    return contracts


def event_signature(event):
    return event["id"], event["args"], event["result"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1],
                        help="raiz do projeto (padrao: pasta acima de python_scripts)")
    parser.add_argument("--interpreter", type=Path, action="append", default=[],
                        help="trace ou console do interpreter; pode repetir")
    parser.add_argument("--aot", type=Path, action="append", default=[],
                        help="trace ou console do AOT; pode repetir")
    parser.add_argument("--contracts", type=Path,
                        help="JSON opcional com casos args/result esperados")
    parser.add_argument("--max-signatures", type=int, default=8,
                        help="maximo de combinacoes args/result impressas por syscall")
    parser.add_argument("--include-vendored", action="store_true",
                        help="inclui PPSSPP, AOT gerado e fontes historicas")
    args = parser.parse_args()
    if args.max_signatures < 1:
        parser.error("--max-signatures precisa ser pelo menos 1")

    root = args.source.resolve()
    if not root.is_dir():
        parser.error(f"diretorio de fontes inexistente: {root}")

    try:
        contracts = load_contracts(args.contracts)
    except ValueError as error:
        parser.error(str(error))

    catalog, contents = parse_source_catalog(root, args.include_vendored)
    handlers = parse_handlers(contents)
    print(f"Fontes C analisadas: {len(contents)}; funcoes identificadas: {len(catalog)}")
    print("Escopo padrao: fontes do projeto; exclui PPSSPP, output gerado e *_OLD.c.")

    modules = defaultdict(list)
    for name, function in catalog.items():
        modules[module_for(name, Path(function["file"]))].append(function)
    print("\n=== Arquivos C analisados ===")
    for path in sorted(contents):
        print(f"  {path}")

    print("\n=== Catalogo de funcoes por modulo/fonte ===")
    for module in sorted(modules):
        funcs = modules[module]
        counts = Counter(function["classification"] for function in funcs)
        print(f"{module}: {len(funcs)} funcoes "
              f"({', '.join(f'{key}={counts[key]}' for key in sorted(counts))})")
        for function in sorted(funcs, key=lambda item: (item["file"], item["line"])):
            print(f"  {function['name']} [{function['classification']}] "
                  f"{function['file']}:{function['line']}")

    interpreter = read_events(args.interpreter)
    aot = read_events(args.aot)
    all_events = interpreter + aot
    by_id = defaultdict(list)
    for event in all_events:
        by_id[event["id"]].append(event)

    print("\n=== Syscalls/handlers estaticos ===")
    for syscall_id in sorted(handlers):
        handler = handlers[syscall_id]
        name_text = ", ".join(sorted(handler["names"])) or "(nome nao mapeado)"
        body_classes = []
        for source, body in handler["bodies"]:
            body_classes.append(classify_body(body))
        for function_name in handler["names"]:
            function = catalog.get(function_name)
            if function is not None:
                body_classes.append(function["classification"])
        implementation = ", ".join(sorted(set(body_classes))) or "NAO LOCALIZADO"
        called_count = len(by_id.get(syscall_id, ()))
        print(f"0x{syscall_id:05X} {name_text} | {implementation} | "
              f"observado={called_count}")

    if all_events:
        print("\n=== Chamadas observadas: argumentos e saidas ===")
        for syscall_id in sorted(by_id):
            events = by_id[syscall_id]
            names = sorted(handlers.get(syscall_id, {}).get("names", ()))
            print(f"0x{syscall_id:05X} {', '.join(names) if names else '(sem handler mapeado)'}: "
                  f"{len(events)} chamadas")
            seen = set()
            omitted = 0
            for event in events:
                signature = event["args"], event["result"]
                if signature in seen:
                    continue
                if len(seen) >= args.max_signatures:
                    omitted += 1
                    continue
                seen.add(signature)
                print("  args=(" + ", ".join(f"0x{value:08X}" for value in event["args"]) +
                      f") -> 0x{event['result']:08X} [{event['file']}:{event['line']}]")
            if omitted:
                print(f"  ... {omitted} outras combinacoes args/result omitidas; "
                      "use --max-signatures para aumentar o limite.")

    if interpreter and aot:
        print("\n=== Diferencas Interpreter/AOT ===")
        limit = min(len(interpreter), len(aot))
        first_difference = None
        for index in range(limit):
            if event_signature(interpreter[index]) != event_signature(aot[index]):
                first_difference = index
                break
        if first_difference is None and len(interpreter) == len(aot):
            print(f"Sem divergencia: {limit} eventos HLE comparaveis.")
        else:
            index = first_difference if first_difference is not None else limit
            print(f"Primeira diferenca no evento {index}.")
            if index < len(interpreter):
                print(f"  Interpreter: id=0x{interpreter[index]['id']:05X} "
                      f"args={interpreter[index]['args']} result=0x{interpreter[index]['result']:08X}")
            if index < len(aot):
                print(f"  AOT:         id=0x{aot[index]['id']:05X} "
                      f"args={aot[index]['args']} result=0x{aot[index]['result']:08X}")
            if first_difference is None:
                print(f"  Quantidade: Interpreter={len(interpreter)}, AOT={len(aot)}")

    if contracts:
        print("\n=== Validacao contra contratos informados ===")
        failed = 0
        unchecked = 0
        for syscall_id, events in sorted(by_id.items()):
            cases = contracts.get(syscall_id, [])
            if not cases:
                unchecked += len(events)
                continue
            for event in events:
                expected = next((result for expected_args, result in cases
                                 if expected_args == event["args"]), None)
                if expected is None:
                    unchecked += 1
                elif event["result"] != expected:
                    failed += 1
                    print(f"DIVERGE {event['file']}:{event['line']} syscall=0x{syscall_id:05X} "
                          f"args={event['args']} esperado=0x{expected:08X} "
                          f"obtido=0x{event['result']:08X}")
        print(f"Falhas de contrato={failed}; chamadas sem caso esperado={unchecked}.")

    unhandled = sorted(syscall_id for syscall_id in by_id if syscall_id not in handlers)
    if unhandled:
        print("\n=== Syscalls vistos sem handler estatico mapeado ===")
        print(", ".join(f"0x{syscall_id:05X}" for syscall_id in unhandled))
    if not all_events:
        print("\nSem logs de HLE: nenhum In/Out dinamico foi observado.")
    print("\nNota: 'POSSIBLE STUB/SHIM' e classificacao heuristica, nao prova de bypass. "
          "Sem contratos PSP independentes, igualdade Interpreter/AOT nao prova saida correta.")


if __name__ == "__main__":
    main()
