#!/usr/bin/env python3
"""Compare audio HLE calls with PPSSPP logs and PSP return-value behavior.

The emulator trace is produced by MIPS_TRACE_FILE. PPSSPP should have the
sceAudio log category enabled; its standard HLE format includes lines such as:
    00000800=sceAudioOutputPannedBlocking(4, 00008000, 00008000, 08B5B6C0)

PPSSPP prints many arguments as eight-digit hexadecimal values without a 0x
prefix. Buffer addresses are build/run-specific, so comparisons check only
whether the buffer is null, not whether its address is identical.

Examples:
    python python_scripts/compare_audio_implementations.py artifacts/runs/Run65.trace
    python python_scripts/compare_audio_implementations.py artifacts/runs/Run65.trace artifacts/runs/Log_Run70.txt
    python python_scripts/compare_audio_implementations.py --self-test
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


PROJECT_HLE_RE = re.compile(
    r"^TRACE HLE at=(\d+).*? id=([0-9A-Fa-f]+) args=([^ ]+) "
    r"result=([0-9A-Fa-f]+)"
)
PPSSPP_CALL_RE = re.compile(
    r"(?:(?P<result>[0-9A-Fa-f]{8})=)?"
    r"(?P<name>sceAudio(?:OutputPannedBlocking|OutputPanned|ChReserve|SetChannelDataLen))"
    r"\((?P<args>[^)]*)\)"
)
OUTPUT_NAMES = {
    0x40003: "sceAudioOutputPannedBlocking",
    0x40004: "sceAudioOutputPanned",
}
NAME_TO_ID = {name: syscall_id for syscall_id, name in OUTPUT_NAMES.items()}


def signed32(value):
    return value - 0x100000000 if value & 0x80000000 else value


def parse_number(value):
    value = value.strip()
    if value.lower().startswith("0x"):
        return int(value, 16)
    if value.startswith("-"):
        return int(value, 10)
    if re.fullmatch(r"[0-9A-Fa-f]{8}", value) or re.search(r"[A-Fa-f]", value):
        return int(value, 16)
    return int(value, 10)


def project_events(path):
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return parse_project_lines(stream)


def parse_project_lines(lines):
    lengths = {}
    outputs = []
    for line_number, line in enumerate(lines, 1):
        match = PROJECT_HLE_RE.match(line)
        if not match:
            continue
        syscall_id = int(match.group(2), 16)
        args = tuple(int(value, 16) for value in match.group(3).split(","))
        result = int(match.group(4), 16)
        if syscall_id == 0x40001:
            lengths[result] = args[1]
        elif syscall_id == 0x40005:
            lengths[args[0]] = args[1]
        elif syscall_id in OUTPUT_NAMES:
            outputs.append({
                "implementation": "core",
                "line": line_number,
                "name": OUTPUT_NAMES[syscall_id],
                "channel": args[0],
                "left": args[1],
                "right": args[2],
                "buffer": args[3],
                "result": result,
                "samples": lengths.get(args[0]),
            })
    return outputs


def ppsspp_events(path):
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return parse_ppsspp_lines(stream)


def parse_ppsspp_lines(lines):
    lengths = {}
    outputs = []
    for line_number, line in enumerate(lines, 1):
        for match in PPSSPP_CALL_RE.finditer(line):
            name = match.group("name")
            try:
                args = tuple(parse_number(value)
                             for value in match.group("args").split(","))
            except ValueError:
                continue
            result_text = match.group("result")
            result = int(result_text, 16) if result_text else None
            if name == "sceAudioChReserve" and len(args) >= 2 and result is not None:
                lengths[result] = args[1]
            elif name == "sceAudioSetChannelDataLen" and len(args) >= 2:
                lengths[args[0]] = args[1]
            elif name in NAME_TO_ID and len(args) >= 4:
                outputs.append({
                    "implementation": "PPSSPP",
                    "line": line_number,
                    "name": name,
                    "channel": args[0],
                    "left": args[1],
                    "right": args[2],
                    "buffer": args[3],
                    "result": result,
                    "samples": lengths.get(args[0]),
                })
    return outputs


def contract_failures(events):
    failures = []
    for event in events:
        if event["samples"] is None or event["result"] is None:
            continue
        if signed32(event["result"]) >= 0 and event["result"] != event["samples"]:
            failures.append(event)
    return failures


def event_profile(event):
    return (
        event["name"], event["channel"], event["left"], event["right"],
        event["samples"], event["result"],
    )


def describe_profile(profile):
    name, channel, left, right, samples, result = profile
    result_text = "unknown" if result is None else f"0x{result:08X}"
    return (f"{name} ch={channel} vol=({left},{right}) "
            f"amostras={samples} retorno={result_text}")


def describe_event(event):
    if event is None:
        return "<sem evento correspondente>"
    result = "sem retorno registrado" if event["result"] is None else f"0x{event['result']:08X}"
    return (f"{event['implementation']} linha {event['line']}: "
            f"{event['name']} ch={event['channel']} "
            f"vol=({event['left']},{event['right']}) "
            f"buffer=0x{event['buffer']:08X} "
            f"amostras={event['samples']} retorno={result}")


def run(project_trace, ppsspp_log=None):
    core_events = project_events(project_trace)
    if not core_events:
        print(f"{project_trace}: nenhuma chamada de audio 0x40003/0x40004 encontrada.")
        return 2

    print(f"Core: {len(core_events)} chamadas de output de audio em {project_trace}")
    print(f"  Buffers nulos: {sum(event['buffer'] == 0 for event in core_events)}")
    by_name = Counter(event["name"] for event in core_events)
    for name, count in by_name.items():
        print(f"  {name}: {count}")

    failures = contract_failures(core_events)
    if failures:
        first = failures[0]
        print(f"FALHA de contrato: {len(failures)} retorno(s) positivo(s) diferente(s) "
              "do tamanho configurado.")
        print(f"  Primeiro: {describe_event(first)}; PPSSPP retorna a quantidade "
              "de amostras aceita.")
    else:
        print("Contrato local: todos os retornos de sucesso correspondem ao "
              "tamanho de amostras configurado.")

    if ppsspp_log is not None:
        reference_events = ppsspp_events(ppsspp_log)
        print(f"PPSSPP: {len(reference_events)} chamadas comparaveis em {ppsspp_log}")
        if not reference_events:
            print("  Nenhuma chamada reconhecida; habilite o log HLE/sceAudio do PPSSPP.")
            return 2
        null_buffers = [event for event in reference_events if event["buffer"] == 0]
        print(f"  Buffers nulos: {len(null_buffers)}")
        if null_buffers:
            print("  Chamadas com buffer nulo: " + ", ".join(
                f"linha {event['line']}" for event in null_buffers[:5]
            ))

        core_profiles = Counter(event_profile(event) for event in core_events)
        reference_profiles = Counter(event_profile(event) for event in reference_events)
        common_profiles = core_profiles.keys() & reference_profiles.keys()
        core_only_profiles = core_profiles.keys() - reference_profiles.keys()
        reference_only_profiles = reference_profiles.keys() - core_profiles.keys()
        common_calls = sum(min(core_profiles[p], reference_profiles[p])
                           for p in common_profiles)
        print(f"Perfis de chamada em comum: {len(common_profiles)}; "
              f"chamadas com perfil equivalente: {common_calls}")
        if core_only_profiles:
            print("Perfis vistos somente no core:")
            for profile in sorted(core_only_profiles, key=str)[:10]:
                print(f"  {describe_profile(profile)} "
                      f"(x{core_profiles[profile]})")
        if reference_only_profiles:
            print("Perfis vistos somente no PPSSPP:")
            for profile in sorted(reference_only_profiles, key=str)[:10]:
                print(f"  {describe_profile(profile)} "
                      f"(x{reference_profiles[profile]})")
        count_differences = [
            (profile, core_profiles[profile], reference_profiles[profile])
            for profile in common_profiles
            if core_profiles[profile] != reference_profiles[profile]
        ]
        if count_differences:
            print("Contagens diferentes por perfil (podem refletir duracoes/pontos "
                  "de captura diferentes):")
            for profile, core_count, reference_count in sorted(
                    count_differences, key=lambda item: str(item[0]))[:10]:
                print(f"  {describe_profile(profile)}: core={core_count}, "
                      f"PPSSPP={reference_count}")
        print("Enderecos de buffer ignorados porque variam entre execucoes; "
              "buffers nulos sao contabilizados separadamente.")
        reference_failures = contract_failures(reference_events)
        if reference_failures:
            print(f"AVISO: o log PPSSPP tambem tem {len(reference_failures)} "
                  "retorno(s) inconsistente(s) com a quantidade configurada.")

    return 1 if failures else 0


def self_test():
    core = [
        "TRACE HLE at=1 pc=0 id=40001 args=FFFFFFFF,00000800,00000000,0 result=00000004\n",
        "TRACE HLE at=2 pc=0 id=40005 args=00000004,00000800,0,0 result=00000000\n",
        "TRACE HLE at=3 pc=0 id=40003 args=00000004,00008000,00008000,08B5B6C0 result=00000800\n",
    ]
    ppsspp = [
        "00000004=sceAudioChReserve(-1, 2048, 0)\n",
        "00000000=sceAudioSetChannelDataLen(4, 2048)\n",
        "00000800=sceAudioOutputPannedBlocking(4, 00008000, 00008000, 08B5F6C0)\n",
    ]
    core_events = parse_project_lines(core)
    ppsspp_events_list = parse_ppsspp_lines(ppsspp)
    assert len(core_events) == len(ppsspp_events_list) == 1
    assert ppsspp_events_list[0]["left"] == ppsspp_events_list[0]["right"] == 0x8000
    assert not contract_failures(core_events)
    assert event_profile(core_events[0]) == event_profile(ppsspp_events_list[0])
    null_event = parse_ppsspp_lines([
        "00000800=sceAudioOutputPannedBlocking(4, 00008000, 00008000, 00000000)\n"
    ])[0]
    assert null_event["buffer"] == 0
    bad = dict(core_events[0], result=0)
    assert len(contract_failures([bad])) == 1
    print("compare_audio_implementations self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project_trace", nargs="?", type=Path)
    parser.add_argument("ppsspp_log", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.project_trace is None:
        parser.error("informe o caminho do trace do core ou use --self-test")
    if not args.project_trace.is_file():
        parser.error(
            f"trace do core nao encontrado: {args.project_trace} "
            f"(caminho resolvido: {args.project_trace.resolve()}); "
            "copie o arquivo para esta pasta ou informe o caminho correto"
        )
    if args.ppsspp_log is not None and not args.ppsspp_log.is_file():
        parser.error(
            f"log PPSSPP nao encontrado: {args.ppsspp_log} "
            f"(caminho resolvido: {args.ppsspp_log.resolve()}); "
            "copie o arquivo para esta pasta ou informe o caminho correto"
        )
    try:
        return run(args.project_trace, args.ppsspp_log)
    except OSError as error:
        print(f"Erro lendo arquivo: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
