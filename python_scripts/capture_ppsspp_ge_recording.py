#!/usr/bin/env python3
"""Capture one completed PPSSPP GPU frame through its local debugger."""

import argparse
import base64
import sys
from pathlib import Path

from capture_ppsspp_ge_memory import Debugger


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--output", type=Path, required=True,
                        help="destination .ppdmp file")
    args = parser.parse_args()

    debugger = None
    try:
        debugger = Debugger(args.host, args.port, args.timeout)
        response = debugger.request("gpu.record.dump")
        uri = response.get("uri", "")
        prefix = "data:application/octet-stream;base64,"
        if not uri.startswith(prefix):
            raise ValueError("unexpected GPU recording response format")
        data = base64.b64decode(uri[len(prefix):], validate=True)
        if not data.startswith(b"PPSSPPGE"):
            raise ValueError("GPU recording has an invalid PPSSPP header")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        print(f"Saved {len(data)} recording bytes to {args.output}")
    except (OSError, ValueError, ConnectionError, RuntimeError) as exc:
        print(f"PPSSPP GPU recording capture failed: {exc}", file=sys.stderr)
        return 2
    finally:
        if debugger:
            debugger.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
