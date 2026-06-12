#!/usr/bin/env python3
"""ADC zero-offset calibration closed-loop test.

At power-up the firmware averages 50 frames into a per-channel zero baseline
(`app_drv_adc_zero_boot`, INIT_APP). `adc_zero` shows/redoes the calibration
and `adc_dump` reports a per-row `zero_ma` column plus the baseline-corrected
current in the ma column. Validates that:
- `adc_zero` status reports a non-zero frame count without this case ever
  calibrating — i.e. the boot-time calibration actually ran;
- `adc_zero 30` recalibrates and the status frame count updates to 30;
- every `adc_dump` row satisfies ma == corrected(raw, zero) bit-exactly, with
  zero taken from the `adc_zero` status table (integer math replicated from
  app_drv_adc_corrected_ma) — proving the ma column is the corrected current;
- each row's `zero_ma` column equals raw_to_current_ma(zero).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

RECAL_FRAMES = 30

STATUS_RE = re.compile(rb"adc_zero: frames=(\d+)")
ZERO_RAW_RE = re.compile(rb"zero_raw:((?:\s+\d+){16})")
DUMP_ROW_RE = re.compile(
    rb"^\s*(\d{1,2})\s+([01-])\s+(P[A-Z]\d+)\s+(ADC[13]_INP\d+)\s+(P[A-Z]\d+)"
    rb"\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
    re.MULTILINE,
)

ADC_VREF_MV = 3300
ADC_MAX_RAW = 65535
INA240A2_GAIN = 50
SHUNT_MOHM = 50


def raw_to_ma(raw: int) -> int:
    """Integer-exact replica of app_drv_adc_raw_to_current_ma (REF_MV = 0)."""
    mv = raw * ADC_VREF_MV // ADC_MAX_RAW
    return mv * 1000 // (INA240A2_GAIN * SHUNT_MOHM)


def corrected_ma(raw: int, zero: int) -> int:
    return raw_to_ma(raw - zero) if raw > zero else 0


def fail(msg: str) -> int:
    print(f"FAIL: {msg}")
    return EXIT_FAIL


def read_status(term: serial_term.Term) -> tuple[int, list[int]]:
    term.flush_input()
    term.send_line("adc_zero")
    # Trailing \r?\n guards against matching a partially received 16th number.
    buf, ok = term.expect(rb"zero_raw:(?:[ \t]+\d+){16}\r?\n", timeout=3.0)
    if not ok:
        raise RuntimeError("adc_zero status not seen")
    frames = int(STATUS_RE.search(buf).group(1))
    zeros = [int(v) for v in ZERO_RAW_RE.search(buf).group(1).split()]
    return frames, zeros


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            boot_frames, _ = read_status(term)
            if boot_frames == 0:
                return fail("boot-time zero calibration never ran (frames=0)")
            print(f"boot calibration present: frames={boot_frames}")

            term.flush_input()
            term.send_line(f"adc_zero {RECAL_FRAMES}")
            buf, ok = term.expect(rb"adc_zero: ok frames=(\d+)", timeout=5.0)
            if not ok:
                return fail(f"adc_zero {RECAL_FRAMES} did not complete")

            frames, zeros = read_status(term)
            if frames != RECAL_FRAMES:
                return fail(f"status frames={frames} after recal, "
                            f"expected {RECAL_FRAMES}")

            term.flush_input()
            term.send_line("adc_dump")
            dump, ok = term.expect(rb"vrefint:[^\n]*\n", timeout=3.0)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}")
        return EXIT_FAIL

    print("--- adc_dump capture ---")
    sys.stdout.write(dump.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")
    if not ok:
        return fail("adc_dump did not complete")

    rows = DUMP_ROW_RE.findall(dump)
    if len(rows) != 16:
        return fail(f"parsed {len(rows)} dump rows, expected 16")

    for (chn_b, _pwr, _en, _adc_ch, _pin, raw_b, zero_ma_b, ma_b), zero in zip(rows, zeros):
        chn, raw, ma = int(chn_b), int(raw_b), int(ma_b)
        want = corrected_ma(raw, zero)
        if ma != want:
            return fail(f"chn{chn:02d}: ma={ma} but corrected({raw}, {zero})={want} "
                        f"— ma column is not the corrected current")
        want_zero_ma = raw_to_ma(zero)
        if int(zero_ma_b) != want_zero_ma:
            return fail(f"chn{chn:02d}: zero_ma={int(zero_ma_b)}, "
                        f"expected raw_to_ma({zero})={want_zero_ma}")

    nonzero = sum(1 for z in zeros if z)
    print(f"PASS: boot cal ran, recal frames={frames}, 16 rows corrected-exact, "
          f"zero_ma column consistent ({nonzero} channels with non-zero baseline)")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
