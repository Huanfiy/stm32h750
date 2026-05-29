#!/usr/bin/env python3
"""SD-card filesystem mount closed-loop test.

PARKED — currently blocked by the STM32H7 SDMMC ACMD51 / IDMA hang documented
in `.agent/fixed/2026-05-29-stm32h7-sdmmc-acmd51-idma-hang.md`. Without that
fix, mmcsd_core aborts SD init after CSD parsing and `sd0` is never
registered, so the auto-mount in `app_drv_fs.c` cannot proceed and `/` stays
empty. This case is NOT in `CASE_ORDER` yet; re-add it once the driver fix
lands and SCR can be read.

Resets the target with an SD card present in SDMMC1 and verifies that the
ELM/FATFS filesystem on the card auto-mounts at `/`, i.e. `ls /` no longer
returns `No such directory` and produces a parsable listing.

The auto-mount is intended to run from an `INIT_APP_EXPORT` hook after the
mmcsd thread has registered `sd0`. PASS criteria:
- msh prompt reached, no `hard fault`
- `ls /` output does NOT contain `No such directory`
- The output contains the FAT-style header `Directory /` or at least one
  filename token, proving DFS resolved the path through ELM/FATFS.

Hardware-skip rules (autotools 77): no JLinkExe, no serial TTY, or no
`SD card capacity` log within 8 s of msh (card not inserted / not
power-cycled since previous fault).
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink, serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

CAPACITY_RE = re.compile(rb"SD card capacity\s+\d+\s*KB")


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
            # let mmcsd_de detect + auto-mount run
            buf += term.read(5.0)

            ls_buf = b""
            ls_returned = False
            if msh_ok and b"hard fault" not in buf and CAPACITY_RE.search(buf):
                term.send_line("ls /")
                ls_buf, ls_returned = term.expect(rb"msh\s*/>", timeout=6.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- boot capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    if ls_buf:
        sys.stdout.write("\n--- ls / capture ---\n")
        sys.stdout.write(ls_buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not msh_ok:
        print("FAIL: msh prompt not seen within 6 s")
        return EXIT_FAIL
    if b"hard fault" in buf:
        print("FAIL: hard fault during/after SDIO bring-up")
        return EXIT_FAIL
    if not CAPACITY_RE.search(buf):
        print("SKIP: no `SD card capacity` log — card likely not inserted")
        return EXIT_SKIP

    if not ls_returned:
        print("FAIL: `ls /` never returned to msh prompt within 6 s")
        return EXIT_FAIL
    if b"No such directory" in ls_buf:
        print("FAIL: `/` not mounted — auto-mount did not run or failed")
        return EXIT_FAIL
    # DFS V1 prints `Directory /`; V2 prints entries directly. Either way the
    # response must contain at least one non-error token between the echoed
    # command and the next prompt — and must not be only error text.
    payload = ls_buf.split(b"ls /", 1)[-1].rsplit(b"msh", 1)[0]
    if b"[E/" in payload and b"Directory" not in payload:
        print("FAIL: `ls /` returned only error text, not a listing")
        return EXIT_FAIL
    if not re.search(rb"(Directory\s*/|[A-Za-z0-9_][A-Za-z0-9_.-]{0,254})", payload):
        print("FAIL: `ls /` produced empty payload, no listing visible")
        return EXIT_FAIL

    print("PASS: SD card auto-mounted at /, ls / returned a directory listing")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
