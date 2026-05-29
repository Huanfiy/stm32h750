#!/usr/bin/env python3
"""SD-card insertion closed-loop test.

Resets the target with an SD card physically present in the SDMMC1 socket and
verifies that boot reaches `msh />`, the SDIO driver reports a non-zero card
capacity, and the system stays alive long enough to honour an msh command.

Regression target: when `cache_buf` in `drv_sdmmc.c` lives in DTCM (the default
`.bss`), SDMMC1 IDMA cannot reach it (DTCM is CPU-private, off the AHB
matrix) — the first block read raises an imprecise BusFault inside
`SCB_CleanInvalidateDCache` and dumps `mmcsd_de` thread. PASS requires no
`hard fault` token in the boot log.

Hardware-skip rules (autotools-style 77): no JLinkExe, no serial TTY.
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink, serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

CAPACITY_RE = re.compile(rb"SD card capacity\s+(\d+)\s*KB")


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not in PATH")
        return EXIT_SKIP
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            try:
                jlink.reset_run()
            except jlink.JLinkUnavailable as exc:
                print(f"SKIP: J-Link unreachable: {exc}")
                return EXIT_SKIP

            time.sleep(0.2)
            buf, msh_ok = term.expect(rb"msh\s*/>", timeout=6.0)
            buf += term.read(5.0)

            still_alive = False
            if msh_ok and b"hard fault" not in buf:
                term.send_line("version")
                tail, still_alive = term.expect(rb"RT-?\s*Thread", timeout=3.0)
                buf += tail
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- boot capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not msh_ok:
        print("FAIL: msh prompt not seen within 6 s")
        return EXIT_FAIL

    if b"hard fault" in buf:
        print("FAIL: hard fault during/after SDIO bring-up")
        return EXIT_FAIL

    m = CAPACITY_RE.search(buf)
    if not m:
        print("SKIP: no `SD card capacity` log — card likely not inserted")
        return EXIT_SKIP
    cap_kb = int(m.group(1))
    if cap_kb <= 0:
        print(f"FAIL: implausible card capacity {cap_kb} KB")
        return EXIT_FAIL

    if not still_alive:
        print("FAIL: msh did not respond to `version` after SDIO init")
        return EXIT_FAIL

    print(f"PASS: SDIO brought up {cap_kb} KB card, msh remains responsive")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
