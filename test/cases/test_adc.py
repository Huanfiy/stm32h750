#!/usr/bin/env python3
"""ADC 16-channel closed-loop test.

Sends `adc_dump` on the msh console and expects 16 lines of the form
`chNN <PIN>: raw=#### mv=####`. Validates that:
- the snapshot semaphore releases at all (driver init + DMA + TIM6 work);
- all 16 channels report (ADC1's 14ch + ADC3's 2ch);
- raw values stay within 16-bit range and mV ≤ 3300.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

LINE_RE = re.compile(rb"ch(\d{2})\s+(P[A-Z]\d):\s+raw=\s*(\d+)\s+mv=\s*(\d+)")


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            term.read(0.3)            # drain any stale bytes
            term.send_line("adc_dump")
            buf, ok = term.expect(rb"ch15\s+P[A-Z]\d:.*\n", timeout=3.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- adc_dump capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not ok:
        print("FAIL: did not see all 16 channels within 3 s")
        return EXIT_FAIL

    rows = LINE_RE.findall(buf)
    if len(rows) != 16:
        print(f"FAIL: parsed {len(rows)} channel rows, expected 16")
        return EXIT_FAIL

    for idx_b, pin_b, raw_b, mv_b in rows:
        idx, raw, mv = int(idx_b), int(raw_b), int(mv_b)
        if not (0 <= idx <= 15):
            print(f"FAIL: bad index {idx}")
            return EXIT_FAIL
        if not (0 <= raw <= 0xFFFF):
            print(f"FAIL: raw {raw} out of 16-bit range (ch{idx:02d})")
            return EXIT_FAIL
        if mv > 3300:
            print(f"FAIL: mv {mv} > VREF 3300 (ch{idx:02d})")
            return EXIT_FAIL

    print("PASS: 16/16 channels reported within bounds")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
