#!/usr/bin/env python3
"""Report SAS initialization, mixing, voice setup, and end-flag events in a trace."""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


HLE = re.compile(
    r"^TRACE HLE at=(\d+) pc=([0-9A-Fa-f]+) id=([0-9A-Fa-f]+) "
    r"args=([0-9A-Fa-f,]+) result=([0-9A-Fa-f]+)"
)
API_NAMES = {
    0x40010: "__sceSasInit",
    0x40011: "__sceSasCore",
    0x40012: "__sceSasGetEndFlag",
    0x40017: "__sceSasSetVoice",
    0x40018: "__sceSasSetKeyOn",
    0x50065: "fallback __sceSasSetKeyOn",
    0x50067: "fallback __sceSasSetVoice",
}


def parse_events(text):
    events = []
    for line_number, line in enumerate(text.splitlines(), 1):
        match = HLE.match(line)
        if not match:
            continue
        at, pc, event_id, args, result = match.groups()
        event_id = int(event_id, 16)
        if event_id in API_NAMES:
            events.append(
                {
                    "line": line_number,
                    "at": int(at),
                    "pc": int(pc, 16),
                    "id": event_id,
                    "args": args,
                    "result": int(result, 16),
                }
            )
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path)
    args = parser.parse_args()
    if args.trace is None:
        parser.error("informe o trace existente")
    try:
        text = args.trace.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"erro ao ler {args.trace}: {exc}", file=sys.stderr)
        return 2

    events = parse_events(text)
    counts = Counter(event["id"] for event in events)
    print(f"Trace: {args.trace}")
    for event_id, name in API_NAMES.items():
        print(f"  {name}: {counts[event_id]} chamadas")

    end_flags = [event for event in events if event["id"] == 0x40012]
    if end_flags:
        values = Counter(event["result"] for event in end_flags)
        print("End flags retornadas:")
        for value, count in values.most_common():
            print(f"  0x{value:08X}: {count}")

    voice_calls = counts[0x40017] + counts[0x40018]
    fallback_calls = counts[0x50065] + counts[0x50067]
    if fallback_calls:
        print("AVISO: SetVoice/KeyOn atingiram fallback; use PC/ID para priorizar.")
    elif voice_calls == 0 and counts[0x40012]:
        print(
            "Observacao: houve consulta ao EndFlag, mas nenhum SetVoice/KeyOn "
            "aparece nesta janela do trace."
        )

    print("Amostras:")
    for event in events[:5]:
        print(
            f"  linha {event['line']} instrucao {event['at']}: "
            f"{API_NAMES[event['id']]} PC=0x{event['pc']:08X} "
            f"args={event['args']} retorno=0x{event['result']:08X}"
        )
    if len(events) > 5:
        print(f"  ... {len(events) - 5} eventos SAS adicionais")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
