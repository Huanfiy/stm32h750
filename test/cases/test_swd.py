#!/usr/bin/env python3
"""SWD closed-loop test.

Resets the target via J-Link, then halts and verifies:
- PC sits inside the QSPI XIP window (0x90000000+), proving the bootloader
  successfully jumped to the app and that the app is executing from external
  flash (no fault, no spin in internal-flash halt loop).
- IPSR = 0 (no exception in progress).
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

QSPI_XIP_BASE = 0x90000000
QSPI_XIP_END = 0x90800000  # 8 MB W25Q64


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not in PATH")
        return EXIT_SKIP

    try:
        jlink.reset_run()
        time.sleep(1.0)  # let bootloader hand off + RT-Thread spin up

        regs = jlink.halt_and_regs()
    except jlink.JLinkUnavailable as exc:
        print(f"SKIP: J-Link unreachable: {exc}")
        return EXIT_SKIP

    pc = regs.get("PC") or regs.get("R15")
    xpsr = regs.get("XPSR") or regs.get("xPSR")
    if pc is None or xpsr is None:
        print(f"FAIL: missing PC/XPSR in {sorted(regs)}")
        return EXIT_FAIL

    ipsr = xpsr & 0x1FF
    print(f"PC=0x{pc:08X}  XPSR=0x{xpsr:08X}  IPSR={ipsr}")

    if not (QSPI_XIP_BASE <= pc < QSPI_XIP_END):
        print(f"FAIL: PC 0x{pc:08X} not in QSPI XIP window")
        return EXIT_FAIL
    if ipsr != 0:
        print(f"FAIL: IPSR={ipsr} (not Thread mode — in exception)")
        return EXIT_FAIL

    print("PASS: app running from QSPI, no exception")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
