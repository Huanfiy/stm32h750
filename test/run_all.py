#!/usr/bin/env python3
"""Run the default closed-loop acceptance cases and print a summary table.

Exit code = number of FAIL cases (0 on full success). SKIP does not fail.
Each case runs as a subprocess so its sys.exit / stdout are isolated.
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

CASES_DIR = Path(__file__).resolve().parent / "cases"

# Order matters: swd resets first (proves bootloader+app jump), boot then
# verifies the msh banner before any case sends an msh command. Manual tools
# such as test_can_diagnostics.py and test_can_user.py are intentionally omitted.
CASE_ORDER = [
    "test_swd.py",
    "test_boot.py",
    "test_sd.py",
    "test_fs.py",
    "test_adc.py",
    "test_adc_stat.py",
    "test_can.py",
    "test_can_protocol.py",
    "test_ag_monitor_power_log.py",
    "test_pwr_en.py",
    "test_msh_history.py",
]

STATUS = {0: "PASS", 1: "FAIL", 77: "SKIP"}
COLOR = {"PASS": "\033[92m", "FAIL": "\033[91m", "SKIP": "\033[93m"}
RESET = "\033[0m"


def run_case(path: Path) -> tuple[int, float]:
    start = time.time()
    proc = subprocess.run([sys.executable, str(path)], capture_output=True, text=True, timeout=60)
    elapsed = time.time() - start
    sys.stdout.write(proc.stdout)
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)
    return proc.returncode, elapsed


def main() -> int:
    results: list[tuple[str, int, float]] = []
    for name in CASE_ORDER:
        path = CASES_DIR / name
        if not path.exists():
            print(f"\033[93mMISSING\033[0m {name}")
            continue
        print(f"\n{'=' * 70}\nRunning {name}\n{'=' * 70}")
        rc, elapsed = run_case(path)
        results.append((name, rc, elapsed))

    print(f"\n{'=' * 70}\nSummary\n{'=' * 70}")
    fails = 0
    for name, rc, elapsed in results:
        label = STATUS.get(rc, f"EXIT {rc}")
        if rc not in (0, 77):
            fails += 1
        color = COLOR.get(label, "")
        print(f"  {color}{label:<5}{RESET}  {name:<20s}  {elapsed:6.2f}s")

    return fails


if __name__ == "__main__":
    sys.exit(main())
