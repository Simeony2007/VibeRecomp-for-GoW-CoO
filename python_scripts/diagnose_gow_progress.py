#!/usr/bin/env python3
"""Summarize emulator progress and explain why a traced run stopped.

Examples:
    python python_scripts/diagnose_gow_progress.py artifacts/runs/Run65.trace artifacts/runs/Run65.console.log
    python python_scripts/diagnose_gow_progress.py --self-test
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
HLE_RE = re.compile(r"^TRACE HLE at=(\d+).*? id=([0-9A-Fa-f]+).*? result=([0-9A-Fa-f]+)")
STEP_RE = re.compile(r"^TRACE STEP seq=(\d+) pc=([0-9A-Fa-f]+)")
WATCH_RE = re.compile(r"^TRACE WATCH sample=(.*)$")
THREAD_SWITCH_RE = re.compile(r"\[HLE Scheduler TCB\] Switched to Thread '([^']+)' \(ID: (\d+)\)")

HLE_NAMES = {
    0x22222: "sceKernelWaitSema",
    0x22223: "sceKernelSignalSema",
    0x22224: "sceKernelDelayThread",
    0x22225: "sceKernelCreateThread",
    0x22226: "sceKernelStartThread",
    0x22227: "sceKernelExitThread",
    0x22237: "sceKernelGetThreadId",
    0x22238: "sceKernelPollSema",
    0x2223C: "sceKernelIsCpuIntrEnable",
    0x11120: "sceIoDread",
    0x40003: "sceAudioOutputPannedBlocking",
    0x40004: "sceAudioOutputPanned",
    0x40005: "sceAudioSetChannelDataLen",
    0x40011: "__sceSasCore",
    0x40012: "__sceSasGetEndFlag",
    0x40017: "__sceSasSetVoice",
    0x40018: "__sceSasSetKeyOn",
}


def parse_fields(text):
    return dict(FIELD_RE.findall(text))


def summarize(trace_path, console_path=None):
    hle_counts = Counter()
    hle_results = Counter()
    watch_samples = []
    markers = {}
    last_hle = None
    last_step = None

    with trace_path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            line = line.rstrip("\n")
            if line.startswith("TRACE WATCH STOP "):
                markers["WATCH STOP"] = line
            elif line.startswith(("TRACE HEADER ", "TRACE LIMIT ", "TRACE END ")):
                markers[line.split(" ", 2)[1]] = line

            match = HLE_RE.match(line)
            if match:
                syscall_id = int(match.group(2), 16)
                result = int(match.group(3), 16)
                hle_counts[syscall_id] += 1
                hle_results[(syscall_id, result)] += 1
                last_hle = {
                    "sequence": int(match.group(1)),
                    "pc": re.search(r"\bpc=([0-9A-Fa-f]+)", line).group(1),
                    "id": syscall_id,
                    "result": result,
                    "ra": re.search(r"\bra=([0-9A-Fa-f]+)", line).group(1),
                }

            match = STEP_RE.match(line)
            if match:
                last_step = {
                    "sequence": int(match.group(1)),
                    "pc": match.group(2),
                }

            match = WATCH_RE.match(line)
            if match:
                watch_samples.append(parse_fields(match.group(1)))

    print(f"Trace: {trace_path}")
    header = parse_fields(markers.get("HEADER", "").removeprefix("TRACE HEADER "))
    end = parse_fields(markers.get("END", "").removeprefix("TRACE END "))
    limit = parse_fields(markers.get("LIMIT", "").removeprefix("TRACE LIMIT "))
    if header:
        header_line = markers["HEADER"]
        build_match = re.search(r"\bbuild=(.*?)\s+start=", header_line)
        build = build_match.group(1) if build_match else header.get("build", "desconhecido")
        print(f"  Modo: {header.get('mode', 'desconhecido')}; build: {build}")
    if limit:
        print(f"  Parou no limite configurado: {limit.get('steps', '?')} instrucoes; "
              f"PC final 0x{limit.get('last_pc', '?')}")
    elif "WATCH STOP" in markers:
        print(f"  Parou pelo limite de amostras do watch: "
              f"{parse_fields(markers['WATCH STOP'].removeprefix('TRACE WATCH STOP ')).get('samples', '?')}")
    elif end and end.get("enabled") == "0":
        print("  Trace fechado depois que a captura foi desativada.")
    else:
        expected_max = int(header.get("max", "0")) if header else 0
        if last_hle is not None:
            print(f"  Trace incompleto: ultimo evento HLE na instrucao "
                  f"{last_hle['sequence']}; maximo configurado={expected_max}.")
        else:
            print("  Sem marcador de limite ou encerramento; trace incompleto.")
        print("  O processo pode ter sido encerrado por timeout/sinal; isso nao "
              "identifica sozinho a causa do bloqueio.")
    if end:
        print(f"  Sequencia total: {end.get('seq', '?')}; enabled={end.get('enabled', '?')}")

    total_hle = sum(hle_counts.values())
    print(f"  Eventos HLE: {total_hle}")
    if last_step is not None:
        print(f"  Ultima instrucao registrada: seq={last_step['sequence']} "
              f"PC=0x{last_step['pc']}")
    if last_hle is not None:
        print(f"  Ultimo HLE na instrucao {last_hle['sequence']}: "
              f"0x{last_hle['id']:05X} em 0x{last_hle['pc']}, "
              f"retorno=0x{last_hle['result']:08X}, RA=0x{last_hle['ra']}")
    for syscall_id, count in hle_counts.most_common(10):
        name = HLE_NAMES.get(syscall_id, "desconhecida")
        print(f"    0x{syscall_id:05X} {name}: {count}")

    sas_end_flag_count = hle_counts[0x40012]
    if sas_end_flag_count:
        end_flag_results = Counter({
            result: amount for (sid, result), amount in hle_results.items()
            if sid == 0x40012
        })
        if len(end_flag_results) == 1 and 0xFFFFFFFF in end_flag_results:
            print(f"  SAS: todas as {sas_end_flag_count} consultas de "
                  "__sceSasGetEndFlag retornaram 0xFFFFFFFF nesta captura "
                  "(todas as vozes reportadas inativas). Isso nao identifica sozinho "
                  "a causa: confira se houve chamadas a __sceSasSetVoice/KeyOn e "
                  "lembre que a sintese/mixagem SAS ainda nao esta implementada.")

    for syscall_id in (0x40003, 0x40004):
        count = hle_counts[syscall_id]
        if not count:
            continue
        name = HLE_NAMES[syscall_id]
        results = Counter({
            result: amount for (sid, result), amount in hle_results.items()
            if sid == syscall_id
        })
        shown = ", ".join(f"0x{result:08X} x{amount}"
                          for result, amount in results.most_common(4))
        print(f"  {name}: {count} chamadas; retornos: {shown}")

    if watch_samples:
        watch_pcs = sorted({sample.get("pc", "?") for sample in watch_samples})
        return_addresses = sorted({sample.get("ra", "?") for sample in watch_samples})
        input_strings = sorted({sample.get("a1", "?") for sample in watch_samples})
        pointers = [int(sample["t1"], 16) for sample in watch_samples if "t1" in sample]
        print(f"  Watch: {len(watch_samples)} amostras em PC(s) "
              f"{', '.join('0x' + pc for pc in watch_pcs)}")
        print(f"    RA(s): {', '.join('0x' + ra for ra in return_addresses)}; "
              f"strings de entrada em $a1: {', '.join('0x' + ptr for ptr in input_strings)}")
        if pointers:
            print(f"    $t1 variou de 0x{min(pointers):08X} a 0x{max(pointers):08X} "
                  f"({len(set(pointers))} valores distintos). Isso mostra avanco entre "
                  "amostras, mas nao prova que a rotina retornou.")
    else:
        print("  Nenhuma amostra TRACE WATCH encontrada.")

    if console_path is not None:
        console_text = console_path.read_text(encoding="utf-8", errors="replace")
        print(f"Console: {console_path}")
        switches = THREAD_SWITCH_RE.findall(console_text)
        if switches:
            name, thread_id = switches[-1]
            print(f"  Ultima troca registrada: thread '{name}' (ID {thread_id})")
        if "Dispatcher encerrou:" in console_text:
            line = next(line.strip() for line in console_text.splitlines()
                        if "Dispatcher encerrou:" in line)
            print(f"  {line}")
            if "LIMIT" in markers or "WATCH STOP" in markers:
                print("  O encerramento foi solicitado pelo mecanismo de trace, nao "
                      "evidencia de conclusao normal do jogo.")
        else:
            print("  Nao ha mensagem final do dispatcher no console.")
        for marker in ("HLE EXCEPTION", "WATCHDOG", "FAULT", "No runnable thread"):
            matches = [line.strip() for line in console_text.splitlines()
                       if marker in line]
            if matches:
                print(f"  {marker}: {len(matches)} registro(s); ultimo: {matches[-1][:240]}")


def self_test():
    trace = [
        "TRACE HEADER format=1 mode=HYBRID build=test max=20\n",
        "TRACE STEP seq=25 pc=088167F4 next=088167F8 inst=00000000\n",
        "TRACE HLE at=10 pc=08B00000 id=40005 args=00000004,00000800 result=00000000\n",
        "TRACE HLE at=20 pc=08B00004 id=40003 args=00000004,00008000,00008000,08B10000 result=00000000\n",
        "TRACE HLE at=21 pc=08B00008 id=40012 args=08B5F700,00000001,00000000,FFFFFFFF result=FFFFFFFF ra=08A235E0 sp=0BBFEE60\n",
        "TRACE WATCH sample=1 visits=5000 seq=50000 pc=08820788 a0=0 a1=08B20000 a2=0 a3=0 t0=20 t1=08B20008 t2=25 v0=0 ra=0881EB64 mem_a0=0,0,0 valid=1\n",
        "TRACE LIMIT steps=20 last_pc=0881ED3C\n",
        "TRACE END steps=0 seq=20 enabled=0\n",
    ]
    assert any("TRACE LIMIT" in line for line in trace)
    step = STEP_RE.match(trace[1])
    assert step and int(step.group(1)) == 25 and step.group(2) == "088167F4"
    watch = parse_fields(WATCH_RE.match(trace[5]).group(1))
    assert watch["t1"] == "08B20008"
    audio = [HLE_RE.match(line) for line in trace if HLE_RE.match(line)]
    assert len(audio) == 3 and int(audio[1].group(3), 16) == 0
    print("diagnose_gow_progress self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("console_log", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.trace is None:
        parser.error("informe o caminho do trace ou use --self-test")
    if not args.trace.is_file():
        parser.error(
            f"trace nao encontrado: {args.trace} "
            f"(caminho resolvido: {args.trace.resolve()}); "
            "copie o arquivo para esta pasta ou informe o caminho correto"
        )
    if args.console_log is not None and not args.console_log.is_file():
        parser.error(
            f"log de console nao encontrado: {args.console_log} "
            f"(caminho resolvido: {args.console_log.resolve()}); "
            "copie o arquivo para esta pasta ou informe o caminho correto"
        )
    try:
        summarize(args.trace, args.console_log)
    except OSError as error:
        print(f"Erro lendo arquivo: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
