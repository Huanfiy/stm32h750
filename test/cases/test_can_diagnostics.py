#!/usr/bin/env python3
"""FDCAN2 physical-layer diagnostic.

This case is intentionally more verbose than `test_can.py`. It proves the MCU
CAN driver accepts a transmit request, then reads FDCAN2 protocol/error
registers through J-Link so a no-ACK / Bus-Off condition is obvious.
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink, serial_term, zqwl_can  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

FDCAN2_BASE = 0x4000A400
REG_OFFSETS = {
    "CCCR": 0x018,
    "ECR": 0x040,
    "PSR": 0x044,
    "IR": 0x050,
    "RXF0S": 0x0A4,
    "TXFQS": 0x0C4,
}


def _read_regs() -> dict[str, int]:
    addrs = [FDCAN2_BASE + offset for offset in REG_OFFSETS.values()]
    raw = jlink.read32_many(addrs)
    return {name: raw[FDCAN2_BASE + offset] for name, offset in REG_OFFSETS.items()}


def _fmt_regs(regs: dict[str, int]) -> str:
    return " ".join(f"{name}=0x{value:08X}" for name, value in regs.items())


def _status(regs: dict[str, int]) -> tuple[int, int, int, int, int]:
    ecr = regs["ECR"]
    psr = regs["PSR"]
    tec = ecr & 0xFF
    rec = (ecr >> 8) & 0x7F
    lec = psr & 0x7
    error_passive = (psr >> 5) & 0x1
    bus_off = (psr >> 7) & 0x1
    return tec, rec, lec, error_passive, bus_off


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not in PATH")
        return EXIT_SKIP
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP
    if not zqwl_can.device_present():
        print(f"SKIP: ZQWL device {zqwl_can.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        jlink.reset_run()
        time.sleep(0.8)
        before = _read_regs()
        print("before:", _fmt_regs(before))

        with serial_term.Term() as term, zqwl_can.ZqwlCan() as zq:
            term.read(0.2)
            zq.raw_drain(0.8)
            chunks: list[bytes] = []

            def _capture() -> None:
                chunks.append(zq.raw_drain(8.0))

            reader = threading.Thread(target=_capture, name="zqwl-can-diag-capture", daemon=True)
            reader.start()
            time.sleep(0.05)
            term.send_line("can_send 0x123 DE AD BE EF")
            term_buf = term.read(1.0)
            reader.join(timeout=8.5)
            raw = b"".join(chunks)
            frames = zqwl_can.parse_frames(raw)

        print("--- terminal ---")
        sys.stdout.write(term_buf.decode(errors="replace"))
        print("\n--- zqwl parsed frames ---")
        for can_id, payload, is_fd in frames:
            print(f"id=0x{can_id:03X} data={payload.hex(' ').upper()} fd={int(is_fd)}")
        if not frames:
            print("(none)")

        after = _read_regs()
        print("after: ", _fmt_regs(after))
    except jlink.JLinkUnavailable as exc:
        print(f"SKIP: J-Link unreachable: {exc}")
        return EXIT_SKIP
    except OSError as exc:
        print(f"SKIP: device open failed: {exc}")
        return EXIT_SKIP

    tec, rec, lec, error_passive, bus_off = _status(after)
    print(f"decoded: TEC={tec} REC={rec} LEC={lec} ErrorPassive={error_passive} BusOff={bus_off}")

    if bus_off or tec >= 128:
        print(
            "FAIL: FDCAN2 entered error-passive/bus-off after transmit. "
            "This usually means the MCU transmitted but no CAN node ACKed the frame; "
            "check CAN_H/CAN_L, GND/reference, termination, and transceiver standby/power."
        )
        return EXIT_FAIL
    if not frames:
        print("FAIL: MCU did not enter bus-off, but ZQWL still received no frame")
        return EXIT_FAIL

    print("PASS: MCU transmit observed by ZQWL and FDCAN2 stayed error-active")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
