#!/usr/bin/env python3
"""msh up-arrow command-history closed-loop test.

The finsh shell stores recent command lines (FINSH_USING_HISTORY) and recalls
them when the terminal sends the up-arrow escape `0x1b 0x5b 0x41`. A real
terminal sends those 3 bytes back-to-back at wire speed.

This case proves the recall is *deterministic*, not probabilistic. It seeds
history with a unique marker command, then presses up-arrow `N` times — each as
a single back-to-back 3-byte write — and requires the marker to be recalled
every single time.

Regression guarded: before enabling the H7 hardware RX FIFO in drv_usart.c, the
QSPI-XIP build dropped ~50% of back-to-back UART bytes, so the 3-byte sequence
rarely arrived intact and recall succeeded only ~0/20. With the FIFO the per-byte
RXFNE ISR may fall up to 16 chars behind without overrun, so recall is 100%.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

MARKER = "version"          # valid msh command, unique on the recalled line
UP = b"\x1b[A"              # ESC [ A
TRIALS = 30


def _settle_prompt(term: "serial_term.Term") -> bool:
    """Commit any leftover line and confirm we land on an msh prompt."""
    term.send_raw(b"\r")
    buf, ok = term.expect(rb"msh\s*/?>", timeout=2.0)
    return ok


def main() -> int:
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        with serial_term.Term() as term:
            term.flush_input()

            if not _settle_prompt(term):
                print("SKIP: no msh prompt — app not running on target")
                return EXIT_SKIP

            # Seed history with the marker command (its output ends in a prompt).
            term.send_raw(MARKER.encode() + b"\r")
            term.expect(rb"msh\s*/>", timeout=2.0)

            recalled = 0
            first_failures: list[bytes] = []
            for i in range(TRIALS):
                # Commit whatever is on the line (re-runs the recalled marker
                # command); syncing on the prompt replaces the old blind reads.
                term.send_raw(b"\r")
                term.expect(rb"msh\s*/>", timeout=2.0)
                # One up-arrow, 3 bytes back-to-back like a real terminal.
                term.send_raw(UP)
                echo, ok = term.expect(MARKER.encode(), timeout=0.4)
                if ok:
                    recalled += 1
                elif len(first_failures) < 5:
                    first_failures.append(echo)

            # Leave the shell on a clean line.
            term.send_raw(b"\r")
            term.expect(rb"msh\s*/>", timeout=2.0)
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    print(f"--- up-arrow recall: {recalled}/{TRIALS} returned '{MARKER}' ---")
    for f in first_failures:
        print("   miss echo:", repr(f))

    if recalled != TRIALS:
        print(f"FAIL: recall is not deterministic ({recalled}/{TRIALS}) — "
              "UART is dropping bytes in the back-to-back escape sequence")
        return EXIT_FAIL

    print(f"PASS: up-arrow recalled history {TRIALS}/{TRIALS} times")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
