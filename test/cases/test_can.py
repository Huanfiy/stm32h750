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
from lib import jlink, serial_term, zqwl_can  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77


def _hex_bytes(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def _probe_prompt(term: serial_term.Term, timeout: float = 0.4, tries: int = 2) -> bool:
    """True if msh answers a bare CRLF with a prompt. Retries once: the first
    write right after re-opening a USB-serial port is occasionally lost."""
    for _ in range(tries):
        term.flush_input()
        term.send_raw(b"\r\n")
        _, ok = term.expect(rb"msh />", timeout)
        if ok:
            return True
    return False


def test_rx(term: serial_term.Term, zq: zqwl_can.ZqwlCan) -> bool:
    """ZQWL transmits, MCU `can_sniff` should print the same frame.

    Retries: the CAN box can replay stale frames from earlier runs when its
    channel is re-opened, and `can_sniff 1` pops exactly one frame from the
    driver's RX ring — so a stale frame shadows ours. Each retry consumes one
    stale frame; our resent 0x789s are idempotent, so the loop converges."""
    can_id, payload = 0x789, bytes([0x11, 0x22, 0x33, 0x44])
    needle = f"ID=0x{can_id:03X}".encode()
    attempts = 3
    for attempt in range(attempts):
        term.flush_input()
        term.send_line("can_sniff 1")
        # No settle delay: the FDCAN IRQ pushes frames into the driver's RX
        # ring regardless of whether can_sniff has reached app_drv_can_recv.
        zq.send(can_id, payload)
        buf, ok = term.expect(rb"ID=0x[0-9A-F]+\s+STD\s+DLC=\d", timeout=4.0)
        print(f"[rx] ZQWL → MCU sent ID=0x{can_id:03X} data={_hex_bytes(payload)}"
              + (f" (attempt {attempt + 1}/{attempts})" if attempt else ""))
        print("--- MCU side ---")
        sys.stdout.write(buf.decode(errors="replace"))
        sys.stdout.write("\n")
        if not ok:
            print("FAIL: MCU did not echo a CAN frame")
            return False
        if needle in buf and _hex_bytes(payload).encode() in buf:
            print(f"[rx] PASS")
            return True
        print(f"[rx] sniffed a stale frame (looking for {needle.decode()}), retrying")
    print(f"FAIL: MCU echo did not match ID/data after {attempts} attempts")
    return False


def test_tx(term: serial_term.Term, zq: zqwl_can.ZqwlCan) -> bool:
    """MCU transmits via msh `can_send`, ZQWL raw stream should contain the frame."""
    can_id, payload = 0x123, bytes([0xDE, 0xAD, 0xBE, 0xEF])
    cmd = f"can_send 0x{can_id:03X} " + " ".join(f"{b:02X}" for b in payload)
    zq.flush_input()
    term.flush_input()
    term.send_raw((cmd + "\r\n").encode())

    # Bytes queue up in the kernel tty buffers on both fds, so nothing is lost
    # while we wait on one side at a time. recv() returns as soon as a complete
    # frame parses (heartbeats are skipped), so the deadline is a cap, not a
    # fixed wait.
    deadline = time.time() + 8.0
    frames: list[tuple[int, bytes, bool]] = []
    matched = False
    while not matched and time.time() < deadline:
        frames.extend(zq.recv(deadline - time.time()))
        matched = any(cid == can_id and pl == payload for cid, pl, _ in frames)

    term_buf, _ = term.expect(rb"can_send id=0x[0-9A-Fa-f]+ dlc=\d+ -> -?\d+", timeout=2.0)
    print(f"[tx] MCU → ZQWL msh `{cmd}`")
    print("--- MCU side ---")
    sys.stdout.write(term_buf.decode(errors="replace"))
    sys.stdout.write("\n")
    print(f"[tx] ZQWL parsed {len(frames)} frame(s): " +
          ", ".join(f"0x{cid:03X}={_hex_bytes(pl)}" for cid, pl, _ in frames))
    if matched:
        print("[tx] PASS")
        return True
    print(f"[tx] FAIL: no match for ID=0x{can_id:03X} data={_hex_bytes(payload)}")
    print(f"[tx] unparsed bytes: {zq.pending_raw().hex(' ').upper()}")
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

    with term_ctx as term:
        # Reset only when the shell is unresponsive — a live msh is all the
        # test needs, and a JLinkExe session costs seconds per invocation.
        if not _probe_prompt(term):
            print("[setup] msh unresponsive, resetting via J-Link")
            if jlink.have_jlink():
                try:
                    jlink.reset_run()
                except jlink.JLinkUnavailable:
                    pass
                else:
                    term.expect(rb"msh />", timeout=5.0)
            if not _probe_prompt(term, timeout=1.0):
                print("FAIL: no msh prompt on serial (board hung or not flashed)")
                return EXIT_FAIL

        try:
            with zqwl_can.ZqwlCan() as zq:
                ok_rx = test_rx(term, zq)
                ok_tx = test_tx(term, zq)
        except OSError as exc:
            print(f"SKIP: ZQWL open failed: {exc}")
            return EXIT_SKIP

    return EXIT_PASS if (ok_rx and ok_tx) else EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())
