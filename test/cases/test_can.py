#!/usr/bin/env python3
"""FDCAN2 bidirectional closed-loop test.

Drives both directions through one ZQWL UCANFD-100C CAN box bridged onto the
500 kbps Classic CAN bus that the MCU's FDCAN2 (PB12/PB13) sits on:

1. ZQWL → MCU: ZQWL transmits a frame, msh `can_sniff 1` decodes it.
2. MCU → ZQWL: msh `can_send <id> <data>`, ZQWL raw-USB stream is parsed for
   the matching CAN payload (skipping heartbeats).

Requires CAN_H/CAN_L wired between the box and the board, 120 Ω termination
at both ends.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term, zqwl_can  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77


def _hex_bytes(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def test_rx(term: serial_term.Term, zq: zqwl_can.ZqwlCan) -> bool:
    """ZQWL transmits, MCU `can_sniff` should print the same frame."""
    can_id, payload = 0x789, bytes([0x11, 0x22, 0x33, 0x44])
    term.read(0.2)
    term.send_line("can_sniff 1")
    time.sleep(0.4)            # let msh enter recv state
    zq.send(can_id, payload)
    buf, ok = term.expect(rb"ID=0x[0-9A-F]+\s+STD\s+DLC=\d", timeout=4.0)
    print(f"[rx] ZQWL → MCU sent ID=0x{can_id:03X} data={_hex_bytes(payload)}")
    print("--- MCU side ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n")
    if not ok:
        print("FAIL: MCU did not echo a CAN frame")
        return False
    needle = f"ID=0x{can_id:03X}".encode()
    if needle not in buf or _hex_bytes(payload).encode() not in buf:
        print(f"FAIL: MCU echo did not match ID/data (looking for {needle.decode()})")
        return False
    print(f"[rx] PASS")
    return True


def test_tx(term: serial_term.Term, zq: zqwl_can.ZqwlCan) -> bool:
    """MCU transmits via msh `can_send`, ZQWL raw stream should contain the frame."""
    can_id, payload = 0x123, bytes([0xDE, 0xAD, 0xBE, 0xEF])
    # Settle: drain the ZQWL stream of stale heartbeats / prior frames.
    zq.raw_drain(0.3)
    cmd = f"can_send 0x{can_id:03X} " + " ".join(f"{b:02X}" for b in payload)
    term.read(0.2)
    term.send_line(cmd)
    buf = zq.raw_drain(2.0)
    frames = zqwl_can.parse_frames(buf)
    print(f"[tx] MCU → ZQWL msh `{cmd}`")
    print(f"[tx] ZQWL parsed {len(frames)} frame(s): " +
          ", ".join(f"0x{cid:03X}={_hex_bytes(pl)}" for cid, pl, _ in frames))
    for cid, pl, _ in frames:
        if cid == can_id and pl == payload:
            print("[tx] PASS")
            return True
    print(f"[tx] FAIL: no match for ID=0x{can_id:03X} data={_hex_bytes(payload)}")
    print(f"[tx] raw bytes: {buf.hex(' ').upper()}")
    return False


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP
    if not zqwl_can.device_present():
        print(f"SKIP: ZQWL device {zqwl_can.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        term_ctx = serial_term.Term()
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    try:
        with term_ctx as term, zqwl_can.ZqwlCan() as zq:
            ok_rx = test_rx(term, zq)
            ok_tx = test_tx(term, zq)
    except OSError as exc:
        print(f"SKIP: ZQWL open failed: {exc}")
        return EXIT_SKIP

    return EXIT_PASS if (ok_rx and ok_tx) else EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())
