#!/usr/bin/env python3
"""Extract sceKernelGetSystemTimeWide results from a MIPS HLE trace."""

import argparse
import re
from pathlib import Path


TIME_CALL = re.compile(
    r"^TRACE HLE\b.*\bid=2223E\b.*\bresult=([0-9A-Fa-f]{8})\b",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Core trace containing TRACE HLE records")
    parser.add_argument("output", type=Path, help="Destination file for hexadecimal time values")
    args = parser.parse_args()

    values = []
    with args.trace.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            match = TIME_CALL.search(line)
            if match:
                values.append(int(match.group(1), 16))

    if not values:
        parser.error(f"No sceKernelGetSystemTimeWide results found in {args.trace}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii", newline="\n") as destination:
        for value in values:
            destination.write(f"0x{value:016X}\n")

    print(f"Extracted {len(values)} time values to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
