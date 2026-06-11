#!/usr/bin/env python3
"""Aging monitor closed-loop test: 100 Hz SD log + over-current power cut.

Uses real J-Link, serial msh, SD card, and ZQWL CAN. The case starts channel 1
with an intentionally impossible current window, expects a LOW_CURRENT event,
verifies PWR_EN1 is high (inactive) in GPIOE_ODR, stops the batch, then reads
the SD CSV log.
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / ".." / "smart_bp_ag_app"))
from lib import jlink, serial_term, zqwl_can  # noqa: E402
from smart_bp_ag.canbus import protocol  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

CAPACITY_RE = re.compile(rb"SD card capacity\s+\d+\s*KB")
PROMPT = rb"msh\s*/>"
GPIOE_ODR = 0x58021014
PWR_EN1_BIT = 1
BATCH_NO = 77
LOG_FILE = f"/aglog/B{BATCH_NO:04d}.CSV"


def _wait_frames(zq: zqwl_can.ZqwlCan, seconds: float):
    deadline = time.time() + seconds
    out = []
    while time.time() < deadline:
        out.extend(zq.recv(min(0.2, max(0.0, deadline - time.time()))))
    return out


def _decoded(frames):
    for can_id, payload, _is_fd in frames:
        msg = protocol.decode(can_id, payload)
        if msg is not None:
            yield msg


def _send_wait_ack(zq: zqwl_can.ZqwlCan, frame, ref_low: int, timeout: float = 5.0):
    rx = []
    can_id, payload = frame
    zq.send(can_id, payload)
    deadline = time.time() + timeout
    while time.time() < deadline:
        rx.extend(zq.recv(min(0.5, max(0.0, deadline - time.time()))))
        for msg in _decoded(rx):
            if isinstance(msg, protocol.Ack) and msg.ref_id_low == ref_low and msg.ok:
                return rx
    return rx


def _cmd(term: serial_term.Term, line: str, timeout: float = 5.0) -> tuple[bytes, bool]:
    term.read(0.3)
    term.send_line(line)
    return term.expect(PROMPT, timeout)


def _boot_and_wait_sd(term: serial_term.Term) -> tuple[bytes, bool, bool]:
    jlink.reset_run()
    time.sleep(0.2)
    buf, prompt_ok = term.expect(PROMPT, timeout=6.0)
    buf += term.read(4.0)
    return buf, prompt_ok, bool(CAPACITY_RE.search(buf))


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
        with serial_term.Term() as term:
            boot, prompt_ok, sd_ok = _boot_and_wait_sd(term)
            if not prompt_ok:
                print("FAIL: msh prompt not seen after reset")
                return EXIT_FAIL
            if b"hard fault" in boot.lower():
                print("FAIL: hard fault during boot")
                return EXIT_FAIL
            if not sd_ok:
                print("SKIP: no SD card capacity log; card likely not present")
                return EXIT_SKIP

            with zqwl_can.ZqwlCan() as zq:
                zq.raw_drain(2.0)
                rx = []
                rx.extend(_send_wait_ack(
                    zq,
                    protocol.encode_config(
                        report_period_100ms=1,
                        duration_s=10,
                        current_min_ma=5000,
                        current_max_ma=6000,
                    ),
                    0x00,
                ))
                rx.extend(_send_wait_ack(
                    zq,
                    protocol.encode_control(protocol.CMD_START, batch_no=BATCH_NO, channel_mask=0x0001),
                    0x20,
                ))
                rx.extend(_wait_frames(zq, 2.0))

                if not any(
                    isinstance(msg, protocol.Event) and msg.channel_no == 1 and msg.event_type == 0x02
                    for msg in _decoded(rx)
                ):
                    print("FAIL: did not observe LOW_CURRENT event for channel 1")
                    return EXIT_FAIL

                odr = jlink.read32(GPIOE_ODR)
                if ((odr >> PWR_EN1_BIT) & 1) != 1:
                    print(f"FAIL: PWR_EN1 not inactive-high after fault, GPIOE_ODR=0x{odr:08X}")
                    return EXIT_FAIL

                _send_wait_ack(
                    zq,
                    protocol.encode_control(protocol.CMD_STOP, batch_no=BATCH_NO, channel_mask=0x0001),
                    0x20,
                )
                time.sleep(0.5)

            cat, cat_ok = _cmd(term, f"cat {LOG_FILE}", timeout=6.0)
    except jlink.JLinkUnavailable as exc:
        print(f"SKIP: J-Link unavailable: {exc}")
        return EXIT_SKIP
    except OSError as exc:
        print(f"SKIP: device open failed: {exc}")
        return EXIT_SKIP

    print(f"--- {LOG_FILE} ---")
    sys.stdout.write(cat.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not cat_ok or b"elapsed_ms,ch,current_ma,adc_raw,state,error" not in cat:
        print("FAIL: SD log header not found")
        return EXIT_FAIL
    rows = [line for line in cat.decode(errors="replace").splitlines() if line.startswith(tuple(str(i) for i in range(10)))]
    if len(rows) < 2:
        print("FAIL: SD log has fewer than 2 sample rows")
        return EXIT_FAIL
    if not any(",1," in row and row.endswith(",2") for row in rows):
        print("FAIL: SD log does not contain channel 1 LOW_CURRENT sample")
        return EXIT_FAIL

    print("PASS: 100 Hz monitor logged to SD and LOW_CURRENT drove PWR_EN1 inactive-high")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
