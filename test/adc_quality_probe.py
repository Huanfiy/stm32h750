#!/usr/bin/env python3
"""ADC noise-quality probe: loop-sample the 16 channels and decide whether the
observed noise is an ADC-configuration problem or a board/input problem.

Method
------
1. `adc_stat 500` (5 s @ 100 Hz) gives per-channel mean/min/max/std.
2. The VREFINT row is the control group: the 1.212 V bandgap never leaves the
   die, so its relative std bounds the noise of the ADC core + VREF+ rail +
   timing/config. External-channel noise far above that floor must enter at
   the pin.
3. The quietest external channel is the second control: it shares clock,
   sampling time, calibration, DMA and trigger with every other channel, so a
   clean floor there exonerates the shared config for the noisy ones.
4. `adc_trace` time series (512 frames) of the noisiest channels + VREFINT are
   analysed for DC bias, zero-clipping, lag-1 autocorrelation and the dominant
   DFT bin — separating broadband noise, periodic pickup (DC-DC/mains alias)
   and floating-node wander.

Pure stdlib; needs only the USB-TTL console. Exit 0 = analysis produced,
77 = no serial device. This is a diagnostic tool, not a pass/fail case — it
is intentionally absent from run_all.py::CASE_ORDER.
"""

from __future__ import annotations

import cmath
import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from lib import serial_term  # noqa: E402

STAT_FRAMES = 500
TRACE_FRAMES = 512
FRAME_HZ = 100.0

ROW_RE = re.compile(
    rb"^\s*(\d{1,2})\s+(ADC[13]_INP\d+|vrefint)\s+(P[A-Z]\d+|-)\s+([01-])"
    rb"\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+\.\d)\s+(\d+)\s*$",
    re.MULTILINE,
)


def run_stat(term: serial_term.Term) -> list[dict]:
    term.flush_input()
    term.send_line(f"adc_stat {STAT_FRAMES}")
    buf, ok = term.expect(rb"\n17\s+vrefint[^\n]*\n", timeout=STAT_FRAMES * 0.01 + 10)
    if not ok:
        raise RuntimeError("adc_stat did not complete")
    rows = []
    for m in ROW_RE.findall(buf):
        rows.append({
            "chn": int(m[0]), "name": m[1].decode(), "pin": m[2].decode(),
            "pwr": m[3].decode(), "mean": int(m[4]), "min": int(m[5]),
            "max": int(m[6]), "p2p": int(m[7]), "std": float(m[8]),
            "mv": int(m[9]),
        })
    if len(rows) != 17:
        raise RuntimeError(f"adc_stat parsed {len(rows)} rows, expected 17")
    return rows


def run_trace(term: serial_term.Term, chns: list[str]) -> dict[str, list[int]]:
    term.flush_input()
    term.send_line(f"adc_trace {TRACE_FRAMES} " + " ".join(chns))
    buf, ok = term.expect(rb"adc_trace: done frames=\d+",
                          timeout=TRACE_FRAMES * 0.01 + 15)
    if not ok:
        raise RuntimeError("adc_trace did not complete")
    text = buf.decode(errors="replace")
    hdr = re.search(r"^frame((?:,\w+)+)\r?$", text, re.M)
    if not hdr:
        raise RuntimeError("adc_trace header missing")
    cols = hdr.group(1).lstrip(",").split(",")
    series: dict[str, list[int]] = {c: [] for c in cols}
    for line in re.findall(r"^\d+((?:,\d+)+)\r?$", text, re.M):
        vals = [int(v) for v in line.lstrip(",").split(",")]
        if len(vals) == len(cols):
            for c, v in zip(cols, vals):
                series[c].append(v)
    return series


def lag1_autocorr(x: list[int]) -> float:
    n = len(x)
    mu = sum(x) / n
    den = sum((v - mu) ** 2 for v in x)
    if den == 0:
        return 0.0
    num = sum((x[i] - mu) * (x[i + 1] - mu) for i in range(n - 1))
    return num / den


def dominant_bin(x: list[int]) -> tuple[float, float, float]:
    """Return (freq_hz, peak_mag, peak_over_median) of the DC-removed spectrum."""
    n = len(x)
    mu = sum(x) / n
    w = [v - mu for v in x]
    mags = []
    for k in range(1, n // 2 + 1):
        s = sum(w[i] * cmath.exp(-2j * math.pi * k * i / n) for i in range(n))
        mags.append(abs(s))
    peak_k = max(range(len(mags)), key=mags.__getitem__)
    med = sorted(mags)[len(mags) // 2]
    ratio = mags[peak_k] / med if med > 0 else 0.0
    return (peak_k + 1) * FRAME_HZ / n, mags[peak_k], ratio


def describe_series(label: str, x: list[int]) -> dict:
    n = len(x)
    mu = sum(x) / n
    std = math.sqrt(sum((v - mu) ** 2 for v in x) / n)
    zeros = sum(1 for v in x if v == 0) / n
    r1 = lag1_autocorr(x)
    freq, _, ratio = dominant_bin(x)
    print(f"  {label:6s} n={n} mean={mu:8.1f} std={std:7.1f} "
          f"min={min(x):6d} max={max(x):6d} zero%={zeros * 100:5.1f} "
          f"lag1={r1:+.2f} peak={freq:5.2f}Hz x{ratio:.1f}med")
    return {"mean": mu, "std": std, "zeros": zeros, "r1": r1,
            "freq": freq, "ratio": ratio}


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return 77

    with serial_term.Term() as term:
        print(f"== adc_stat {STAT_FRAMES} (5 s @ 100 Hz) ==")
        rows = run_stat(term)
        for r in rows:
            print(f"  chn{r['chn']:02d} {r['name']:<11s} {r['pin']:<4s} pwr={r['pwr']} "
                  f"mean={r['mean']:6d} p2p={r['p2p']:5d} std={r['std']:7.1f} "
                  f"({r['mv']} mV)")

        vref = next(r for r in rows if r["name"] == "vrefint")
        ext = [r for r in rows if r["name"] != "vrefint"]
        ext_by_std = sorted(ext, key=lambda r: r["std"])
        quiet, noisy = ext_by_std[0], ext_by_std[::-1][:3]

        vref_rel = vref["std"] / vref["mean"] * 100
        print(f"\n== control group ==")
        print(f"  vrefint: std={vref['std']} counts ({vref_rel:.3f}% of mean) "
              f"p2p={vref['p2p']} — ADC core + VREF+ rail + config floor")
        print(f"  quietest external: chn{quiet['chn']:02d} {quiet['pin']} "
              f"std={quiet['std']} counts — shared-config floor")

        trace_chns = [str(r["chn"]) for r in noisy] + [str(quiet["chn"]), "v"]
        print(f"\n== adc_trace {TRACE_FRAMES} {' '.join(trace_chns)} "
              f"(~{TRACE_FRAMES / FRAME_HZ:.0f} s) ==")
        series = run_trace(term, trace_chns)
        traits = {label: describe_series(label, x) for label, x in series.items()}

        print("\n== verdict ==")
        config_ok = vref_rel < 0.5 and quiet["std"] < 30
        if config_ok:
            print(f"  [config OK] vrefint rel-std {vref_rel:.3f}% < 0.5% and the "
                  f"quietest channel (chn{quiet['chn']:02d}, std {quiet['std']}) is "
                  f"clean under the SAME clock/sampling/trigger/DMA config — the "
                  f"shared ADC configuration delivers clean conversions.")
        else:
            print(f"  [config SUSPECT] vrefint rel-std {vref_rel:.3f}% or quiet-floor "
                  f"std {quiet['std']} too high — investigate clock/sampling time/"
                  f"calibration before blaming the board.")

        for r in noisy:
            label = f"ch{r['chn']}"
            t = traits.get(label)
            if t is None or t["std"] < 30:
                continue
            causes = []
            if t["ratio"] > 8:
                causes.append(f"periodic pickup at ~{t['freq']:.1f} Hz (aliased)")
            if t["r1"] > 0.5:
                causes.append("low-frequency wander (floating/undriven node)")
            elif t["r1"] < -0.3:
                causes.append("alternating pattern (~Nyquist alias, e.g. 50 Hz mains)")
            if t["mean"] > 3 * t["std"] and min(series[label]) > 0:
                causes.append(f"persistent DC bias ({t['mean']:.0f} counts ≈ "
                              f"{t['mean'] * 3300 / 65535:.0f} mV) — real voltage at pin")
            if t["zeros"] > 0.05:
                causes.append("clips at 0 — bipolar noise on a ground-referenced input")
            print(f"  [chn{r['chn']:02d} {r['pin']} pwr={r['pwr']}] "
                  f"std={t['std']:.0f} counts: " + ("; ".join(causes) or "broadband noise"))

        if config_ok:
            print("  => noise enters at the input pins (board/harness/INA240 stage), "
                  "NOT in the ADC configuration.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
