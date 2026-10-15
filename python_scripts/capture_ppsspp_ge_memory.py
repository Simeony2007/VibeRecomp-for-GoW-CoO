#!/usr/bin/env python3
"""Capture PPSSPP CPU status and selected GE-related memory words via debugger WebSocket."""

import argparse
import base64
import json
import os
import socket
import struct
import sys
from datetime import datetime
from pathlib import Path


class Debugger:
    def __init__(self, host, port, timeout):
        self.socket = socket.create_connection((host, port), timeout)
        self.socket.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET /debugger HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            f"Origin: http://{host}:{port}\r\n\r\n"
        )
        self.socket.sendall(request.encode("ascii"))
        headers = bytearray()
        while not headers.endswith(b"\r\n\r\n"):
            chunk = self.socket.recv(1)
            if not chunk:
                raise ConnectionError("PPSSPP closed the debugger handshake")
            headers.extend(chunk)
        if not headers.startswith(b"HTTP/1.1 101"):
            raise ConnectionError(headers.decode("latin1", errors="replace").strip())
        self.ticket = 1

    def close(self):
        self.socket.close()

    def send(self, value, opcode=1):
        if opcode == 1:
            payload = json.dumps(value).encode("utf-8")
        elif isinstance(value, bytes):
            payload = value
        else:
            payload = str(value).encode("utf-8")
        mask = os.urandom(4)
        length = len(payload)
        frame = bytearray([0x80 | opcode])
        if length < 126:
            frame.append(0x80 | length)
        elif length < 65536:
            frame.append(0x80 | 126)
            frame.extend(struct.pack("!H", length))
        else:
            frame.append(0x80 | 127)
            frame.extend(struct.pack("!Q", length))
        frame.extend(mask)
        frame.extend(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(frame)

    def receive_frame(self):
        header = self.read_exact(2)
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read_exact(8))[0]
        return bool(header[0] & 0x80), header[0] & 0x0F, self.read_exact(length)

    def read_exact(self, length):
        data = bytearray()
        while len(data) < length:
            chunk = self.socket.recv(length - len(data))
            if not chunk:
                raise ConnectionError("PPSSPP closed the debugger connection")
            data.extend(chunk)
        return bytes(data)

    def request(self, event, **params):
        ticket = self.ticket
        self.ticket += 1
        self.send({"event": event, "ticket": ticket, **params})
        message = bytearray()
        while True:
            final, opcode, payload = self.receive_frame()
            if opcode == 9:
                self.send(payload.decode("utf-8", errors="replace"), opcode=10)
                continue
            if opcode not in (0, 1):
                continue
            message.extend(payload)
            if not final:
                continue
            response = json.loads(message)
            message.clear()
            if response.get("ticket") != ticket:
                continue
            if response.get("event") == "error":
                raise RuntimeError(response.get("message", "PPSSPP debugger error"))
            return response


def parse_range(value):
    try:
        address_text, size_text = value.split(":", 1)
        address, size = int(address_text, 0), int(size_text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use START:SIZE, e.g. 0x08400000:64") from exc
    if address < 0 or size <= 0 or size % 4:
        raise argparse.ArgumentTypeError("range needs a non-negative address and positive size divisible by 4")
    return address, size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--range", action="append", type=parse_range, required=True,
                        metavar="START:SIZE", help="memory range to read as 32-bit words")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    debugger = None
    try:
        debugger = Debugger(args.host, args.port, args.timeout)
        result = {
            "captured_at": datetime.now().astimezone().isoformat(),
            "cpu_status": debugger.request("cpu.status"),
            "ranges": [],
        }
        for address, size in args.range:
            words = []
            for offset in range(0, size, 4):
                response = debugger.request("memory.read_u32", address=address + offset)
                words.append({
                    "address": f"0x{address + offset:08X}",
                    "value": f"0x{response['value']:08X}",
                })
            result["ranges"].append({
                "start": f"0x{address:08X}",
                "size": size,
                "words": words,
            })
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"Captured {sum(len(r['words']) for r in result['ranges'])} words to {args.output}")
        print(f"PPSSPP PC: 0x{result['cpu_status']['pc']:08X}")
    except (OSError, ValueError, KeyError, ConnectionError, RuntimeError) as exc:
        print(f"PPSSPP GE memory capture failed: {exc}", file=sys.stderr)
        return 2
    finally:
        if debugger:
            debugger.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
