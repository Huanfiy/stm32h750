#!/usr/bin/env python3
"""Boot-to-msh closed-loop test.

Resets the target, then captures the serial console and expects to see the
RT-Thread banner followed by an `msh />` prompt within 6 s. Catches bootloader
silence, hangs in `rt_components_init()`, and finsh-thread startup failures.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink, serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77


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
            buf, ok = term.expect(rb"msh\s*/>", timeout=6.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print("--- boot capture ---")
    sys.stdout.write(buf.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if not ok:
        print("FAIL: msh prompt not seen within 6 s")
        return EXIT_FAIL

    needed = [b"[BOOT]", b"RT", b"Thread Operating System", b"msh"]
    missing = [t.decode() for t in needed if t not in buf]
    if missing:
        print(f"FAIL: missing tokens in boot log: {missing}")
        return EXIT_FAIL

    print("PASS: bootloader → app → RT-Thread → msh prompt")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
