#!/usr/bin/env python3
"""ADC 16-channel closed-loop test.

Sends `adc_dump` on the msh console and expects 16 lines of the form
`pwr=<0|1|-> chNN <PIN>: raw=#### mA=####`. Validates that:
- the snapshot semaphore releases at all (driver init + DMA + TIM6 work);
- all 16 channels report (ADC1's 14ch + ADC3's 2ch);
- PWR_EN status is readable as 0/1, or '-' for reserved channels;
- raw values stay within 16-bit range and mA stays within the INA240A2
  + 50 mohm shunt ADC range;
- the VREFINT self-check channel reads ~1.21 V. VREFINT needs >= 4.3 us of
  sampling, so this doubles as a regression guard for the per-channel
  sampling time (an 1.5-cycle config reads VREFINT far below 1 V).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

MAX_CURRENT_MA = 1320
RESERVED_PWR_IDX = {6, 15}

LINE_RE = re.compile(rb"pwr=([01-])\s+ch(\d{2})\s+(P[A-Z]\d):\s+raw=\s*(\d+)\s+mA=\s*(\d+)")
VREF_RE = re.compile(rb"vrefint:\s+raw=\s*(\d+)\s+mV=\s*(\d+)")
VREFINT_MV_MIN, VREFINT_MV_MAX = 1140, 1290


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            term.read(0.3)            # drain any stale bytes
            term.send_line("adc_dump")
            buf, ok = term.expect(rb"vrefint:.*\n", timeout=3.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- adc_dump capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not ok:
        print("FAIL: did not see 16 channels + vrefint within 3 s")
        return EXIT_FAIL

    vref = VREF_RE.search(buf)
    if not vref:
        print("FAIL: vrefint line missing")
        return EXIT_FAIL
    vref_mv = int(vref.group(2))
    if not (VREFINT_MV_MIN <= vref_mv <= VREFINT_MV_MAX):
        print(f"FAIL: vrefint {vref_mv} mV outside "
              f"[{VREFINT_MV_MIN}, {VREFINT_MV_MAX}] (sampling time regression?)")
        return EXIT_FAIL

    rows = LINE_RE.findall(buf)
    if len(rows) != 16:
        print(f"FAIL: parsed {len(rows)} channel rows, expected 16")
        return EXIT_FAIL

    for pwr_b, idx_b, pin_b, raw_b, ma_b in rows:
        idx, raw, ma = int(idx_b), int(raw_b), int(ma_b)
        pwr = pwr_b.decode()
        if pwr not in ("0", "1", "-"):
            print(f"FAIL: bad pwr state {pwr!r} (ch{idx:02d})")
            return EXIT_FAIL
        if not (0 <= idx <= 15):
            print(f"FAIL: bad index {idx}")
            return EXIT_FAIL
        if idx in RESERVED_PWR_IDX and pwr != "-":
            print(f"FAIL: reserved PWR_EN channel reported {pwr!r} (ch{idx:02d})")
            return EXIT_FAIL
        if idx not in RESERVED_PWR_IDX and pwr == "-":
            print(f"FAIL: GPIO-owned PWR_EN channel reported reserved (ch{idx:02d})")
            return EXIT_FAIL
        if not (0 <= raw <= 0xFFFF):
            print(f"FAIL: raw {raw} out of 16-bit range (ch{idx:02d})")
            return EXIT_FAIL
        if ma > MAX_CURRENT_MA:
            print(f"FAIL: mA {ma} > range {MAX_CURRENT_MA} (ch{idx:02d})")
            return EXIT_FAIL

    print(f"PASS: 16/16 channels reported within bounds, vrefint={vref_mv} mV")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
