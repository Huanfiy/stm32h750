#!/usr/bin/env python3
"""ADC loop-sampling statistics closed-loop test.

`adc_stat N` consumes N consecutive 100 Hz frames and prints per-channel
min/max/mean/std plus a VREFINT row; `adc_trace N <ch>...` emits one CSV row
per frame. Validates that:
- `adc_stat 200` paces at the TIM6 frame rate (elapsed ~ 200 x 10 ms), so the
  trigger chain (TIM6 -> ADC -> DMA -> sem) sustains back-to-back frames;
- all 16 channel rows plus the vrefint row parse, with min <= mean <= max and
  p2p == max - min;
- VREFINT mean stays in the 1.14-1.29 V datasheet band and its std stays below
  100 counts (~5 mV). VREFINT never leaves the die — a noisy VREFINT indicts
  the ADC config (clock/sampling time/calibration), not the board, so this
  bound is the "config is healthy" guard the noise diagnosis builds on;
- `adc_trace 30 v` returns exactly 30 well-formed CSV rows.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

STAT_FRAMES = 200
HDR_RE = re.compile(rb"adc_stat: frames=(\d+) elapsed=(\d+) ms")
ROW_RE = re.compile(
    rb"^\s*(\d{1,2})\s+(ADC[13]_INP\d+|vrefint)\s+(P[A-Z]\d+|-)\s+([01-])"
    rb"\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+\.\d)\s+(\d+)\s*$",
    re.MULTILINE,
)
TRACE_ROW_RE = re.compile(rb"^(\d+),(\d+)\r?$", re.MULTILINE)

ELAPSED_MS_MIN, ELAPSED_MS_MAX = 1800, 3000
VREFINT_MV_MIN, VREFINT_MV_MAX = 1140, 1290
VREFINT_STD_MAX = 100.0
TRACE_FRAMES = 30


def fail(msg: str) -> int:
    print(f"FAIL: {msg}")
    return EXIT_FAIL


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            term.flush_input()
            term.send_line(f"adc_stat {STAT_FRAMES}")
            # 200 frames = ~2 s of sampling after the paced command echo.
            # NB: Term.expect searches without re.MULTILINE — anchor on \n.
            buf, ok = term.expect(rb"\n17\s+vrefint[^\n]*\n", timeout=10.0)

            term.flush_input()
            term.send_line(f"adc_trace {TRACE_FRAMES} v")
            tbuf, tok = term.expect(rb"adc_trace: done frames=\d+", timeout=8.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- adc_stat capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not ok:
        return fail("adc_stat: vrefint summary row not seen within 10 s")

    hdr = HDR_RE.search(buf)
    if not hdr:
        return fail("adc_stat: header line missing")
    elapsed = int(hdr.group(2))
    if not (ELAPSED_MS_MIN <= elapsed <= ELAPSED_MS_MAX):
        return fail(f"adc_stat: elapsed {elapsed} ms outside "
                    f"[{ELAPSED_MS_MIN}, {ELAPSED_MS_MAX}] — 100 Hz pacing broken")

    rows = ROW_RE.findall(buf)
    if len(rows) != 17:
        return fail(f"adc_stat: parsed {len(rows)} rows, expected 17")

    vref_seen = False
    for chn_b, name_b, _pin, _pwr, mean_b, mn_b, mx_b, p2p_b, std_b, mv_b in rows:
        chn, name = int(chn_b), name_b.decode()
        mean, mn, mx, p2p = int(mean_b), int(mn_b), int(mx_b), int(p2p_b)
        std, mv = float(std_b), int(mv_b)
        if not (mn <= mean <= mx):
            return fail(f"row {chn}: mean {mean} outside [{mn}, {mx}]")
        if p2p != mx - mn:
            return fail(f"row {chn}: p2p {p2p} != max-min {mx - mn}")
        if mx > 0xFFFF:
            return fail(f"row {chn}: max {mx} beyond 16-bit range")
        if name == "vrefint":
            vref_seen = True
            if chn != 17:
                return fail(f"vrefint row numbered {chn}, expected 17")
            if not (VREFINT_MV_MIN <= mv <= VREFINT_MV_MAX):
                return fail(f"vrefint mean {mv} mV outside "
                            f"[{VREFINT_MV_MIN}, {VREFINT_MV_MAX}]")
            if std > VREFINT_STD_MAX:
                return fail(f"vrefint std {std} counts > {VREFINT_STD_MAX} "
                            f"— ADC config/core noise too high")
            vref_std, vref_mv = std, mv
    if not vref_seen:
        return fail("adc_stat: vrefint row missing")

    if not tok:
        return fail("adc_trace: completion line not seen within 8 s")
    trows = TRACE_ROW_RE.findall(tbuf)
    if len(trows) != TRACE_FRAMES:
        return fail(f"adc_trace: parsed {len(trows)} rows, expected {TRACE_FRAMES}")
    for idx_b, val_b in trows:
        if int(val_b) > 0xFFFF:
            return fail(f"adc_trace: frame {idx_b.decode()} value out of range")

    print(f"PASS: 17 stat rows, elapsed {elapsed} ms, vrefint {vref_mv} mV "
          f"std {vref_std}, {TRACE_FRAMES} trace rows")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
