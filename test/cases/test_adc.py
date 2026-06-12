#!/usr/bin/env python3
"""ADC 16-channel closed-loop test.

Sends `adc_dump` on the msh console and expects 16 table rows of the form
`chn pwr_en en_pin adc_ch adc_pin raw zero_ma ma`. Validates that:
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
RESERVED_PWR_CHN = {7, 15}
EXPECTED_LAST_ADC_PIN = {15: "PC2", 16: "PC3"}

LINE_RE = re.compile(
    rb"^\s*(\d{1,2})\s+([01-])\s+(P[A-Z]\d+)\s+(ADC[13]_INP\d+)\s+(P[A-Z]\d+)"
    rb"\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
    re.MULTILINE,
)
VREF_RE = re.compile(rb"vrefint:\s+raw=\s*(\d+)\s+mV=\s*(\d+)")
VREFINT_MV_MIN, VREFINT_MV_MAX = 1140, 1290


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            term.flush_input()
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

    for chn_b, pwr_b, en_pin_b, adc_ch_b, adc_pin_b, raw_b, zero_b, ma_b in rows:
        chn, raw, ma = int(chn_b), int(raw_b), int(ma_b)
        zero_ma = int(zero_b)
        pwr = pwr_b.decode()
        adc_pin = adc_pin_b.decode()
        if pwr not in ("0", "1", "-"):
            print(f"FAIL: bad pwr state {pwr!r} (chn{chn:02d})")
            return EXIT_FAIL
        if not (1 <= chn <= 16):
            print(f"FAIL: bad channel {chn}")
            return EXIT_FAIL
        if chn in RESERVED_PWR_CHN and pwr != "-":
            print(f"FAIL: reserved PWR_EN channel reported {pwr!r} (chn{chn:02d})")
            return EXIT_FAIL
        if chn not in RESERVED_PWR_CHN and pwr == "-":
            print(f"FAIL: GPIO-owned PWR_EN channel reported reserved (chn{chn:02d})")
            return EXIT_FAIL
        if chn in EXPECTED_LAST_ADC_PIN and adc_pin != EXPECTED_LAST_ADC_PIN[chn]:
            print(f"FAIL: chn{chn:02d} adc_pin={adc_pin}, want {EXPECTED_LAST_ADC_PIN[chn]}")
            return EXIT_FAIL
        if not (0 <= raw <= 0xFFFF):
            print(f"FAIL: raw {raw} out of 16-bit range (chn{chn:02d})")
            return EXIT_FAIL
        if ma > MAX_CURRENT_MA:
            print(f"FAIL: mA {ma} > range {MAX_CURRENT_MA} (chn{chn:02d})")
            return EXIT_FAIL
        if zero_ma > MAX_CURRENT_MA:
            print(f"FAIL: zero_ma {zero_ma} > range {MAX_CURRENT_MA} (chn{chn:02d})")
            return EXIT_FAIL

    print(f"PASS: 16/16 channels reported within bounds, vrefint={vref_mv} mV")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
