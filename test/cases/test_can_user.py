#!/usr/bin/env python3
"""Interactive ZQWL UCANFD-100C CAN sender.

Opens the ZQWL CAN box, configures CAN0 as 500 kbps Classic CAN through the
existing helper, then lets a user type CAN frames in a terminal prompt.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import zqwl_can  # noqa: E402

EXIT_PASS, EXIT_SKIP = 0, 77

HELP = """\
Commands:
  <id> <data...>        send one Classic CAN frame
  send <id> <data...>   same as above
  can_send <id> <data>  same style as the board msh command
  rx [seconds]          receive and print frames from the CAN box
  drain                 clear pending USB/CAN data
  help                  show this help
  quit                  exit

Examples:
  0x123 DE AD BE EF
  123 11 22 33 44
  send 0x789 11223344
  can_send 0x123 DE AD BE EF
"""


def _hex_bytes(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def _parse_int(text: str, *, default_base: int = 16) -> int:
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    return int(text, default_base)


def _parse_payload(tokens: list[str]) -> bytes:
    if len(tokens) == 1:
        compact = tokens[0].replace("_", "").replace("-", "")
        if compact.lower().startswith("0x"):
            compact = compact[2:]
        if compact and len(compact) % 2 == 0 and len(compact) > 2:
            return bytes.fromhex(compact)

    out = bytearray()
    for token in tokens:
        value = _parse_int(token)
        if not 0 <= value <= 0xFF:
            raise ValueError(f"byte out of range: {token}")
        out.append(value)
    return bytes(out)


def _parse_tx_line(line: str) -> tuple[int, bytes]:
    tokens = line.replace(",", " ").split()
    if tokens and tokens[0] in {"send", "tx", "can_send"}:
        tokens = tokens[1:]
    if len(tokens) < 2:
        raise ValueError("usage: <id> <b0> [b1 ... b7]")

    can_id = _parse_int(tokens[0])
    if not 0 <= can_id <= 0x7FF:
        raise ValueError(f"standard CAN ID out of range: 0x{can_id:X}")

    payload = _parse_payload(tokens[1:])
    if len(payload) > 8:
        raise ValueError("Classic CAN payload is limited to 8 bytes")

    return can_id, payload


def _print_frames(frames: list[tuple[int, bytes, bool]]) -> None:
    if not frames:
        print("(no frame)")
        return
    for can_id, payload, is_fd in frames:
        proto = "FD" if is_fd else "Classic"
        print(f"rx {proto} id=0x{can_id:03X} dlc={len(payload)} data={_hex_bytes(payload)}")


def main() -> int:
    if not zqwl_can.device_present():
        print(f"SKIP: ZQWL device {zqwl_can.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with zqwl_can.ZqwlCan() as zq:
            print(f"Opened {zq.path}; CAN0 configured as 500 kbps Classic CAN.")
            print("Type `help` for commands, `quit` to exit.")
            zq.raw_drain(0.2)

            while True:
                try:
                    line = input("can> ").strip()
                except (EOFError, KeyboardInterrupt):
                    print()
                    break

                if not line:
                    continue

                cmd = line.lower().split()[0]
                if cmd in {"q", "quit", "exit"}:
                    break
                if cmd in {"h", "help", "?"}:
                    print(HELP)
                    continue
                if cmd == "drain":
                    raw = zq.raw_drain(0.5)
                    frames = zqwl_can.parse_frames(raw)
                    print(f"drained {len(raw)} byte(s), {len(frames)} frame(s)")
                    continue
                if cmd == "rx":
                    parts = line.split()
                    seconds = 3.0
                    if len(parts) > 1:
                        seconds = float(parts[1])
                    print(f"receiving for {seconds:.1f}s...")
                    _print_frames(zq.recv(seconds))
                    continue

                try:
                    can_id, payload = _parse_tx_line(line)
                    zq.send(can_id, payload)
                except (ValueError, OSError) as exc:
                    print(f"ERR: {exc}")
                    continue

                print(f"tx id=0x{can_id:03X} dlc={len(payload)} data={_hex_bytes(payload)}")
    except OSError as exc:
        print(f"SKIP: ZQWL open failed: {exc}")
        return EXIT_SKIP

    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
