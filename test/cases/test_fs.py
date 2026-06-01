#!/usr/bin/env python3
"""SD-card FAT filesystem closed-loop test: auto-mount + write→reset→readback.

Two things are proven end-to-end against the real card:

1. **Mount** — after reset, mmcsd enumerates `sd0` and `app_drv_fs` auto-mounts
   it at `/` as ELM/FATFS, with no hard fault. `ls /` resolves through FATFS
   (not just the `/dev` devfs submount).

2. **Data integrity across a power-cycle** — a *fresh* nonce is written to
   `/fsprobe.txt`, then the target is **reset**. The reset drops the DFS V2
   page cache and forces a real remount, so reading the nonce back via `cat`
   proves the bytes physically landed on the card and are read back through a
   fresh FATFS mount. This is the part the old mount-only check could not do:
   it defeats the pcache and would catch mis-parsed partition geometry (the
   card enumerates with a suspicious `begin: 4194304` / size overshoot — if a
   write landed at the wrong LBA the post-reset readback would differ).

A fresh time-based nonce each run means a stale `/fsprobe.txt` left by a prior
run cannot produce a false PASS.

Hardware-skip rules (autotools 77): no JLinkExe, no serial TTY, or no
`SD card capacity` log within timeout (card not inserted / not power-cycled
since a previous fault).
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
PROMPT = rb"msh\s*/>"
PROBE_FILE = "/fsprobe.txt"


def boot_and_wait_mount(term: "serial_term.Term") -> tuple[bytes, bool, bool]:
    """Reset target, wait for the msh prompt, then drain a bit so the mmcsd
    detect thread + auto-mount finish. Returns (buf, msh_ok, capacity_seen)."""
    jlink.reset_run()
    time.sleep(0.2)
    buf, msh_ok = term.expect(PROMPT, timeout=6.0)
    buf += term.read(4.0)
    return buf, msh_ok, bool(CAPACITY_RE.search(buf))


def cmd(term: "serial_term.Term", line: str, timeout: float = 4.0,
        settle: float = 0.4) -> tuple[bytes, bool]:
    """Send one msh command and wait for the next prompt.

    finsh occasionally drops the first keystroke right after printing a prompt
    (the leading char races the shell entering its read loop), turning `echo …`
    into `cho: command not found.`. Drain + settle before sending, and retry
    once if the shell reports the mangled command as not found — so a transient
    serial glitch can't masquerade as a filesystem failure."""
    buf, ok = b"", False
    for _ in range(2):
        term.read(settle)            # let the prompt finish; drain stale bytes
        term.send_line(line)
        buf, ok = term.expect(PROMPT, timeout)
        if ok and b"command not found" not in buf:
            return buf, ok
    return buf, ok


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not in PATH")
        return EXIT_SKIP
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    nonce = f"FSPROBE{int(time.time())}END"

    try:
        with serial_term.Term() as term:
            # ---- Phase 1: first boot, confirm sd0 mounted at / ----
            try:
                boot1, msh_ok, cap = boot_and_wait_mount(term)
            except jlink.JLinkUnavailable as exc:
                print(f"SKIP: J-Link unreachable: {exc}")
                return EXIT_SKIP

            print("--- boot #1 capture ---")
            sys.stdout.write(boot1.decode(errors="replace"))

            if not msh_ok:
                print("\nFAIL: msh prompt not seen within 6 s")
                return EXIT_FAIL
            if b"hard fault" in boot1:
                print("\nFAIL: hard fault during/after SDIO bring-up")
                return EXIT_FAIL
            if not cap:
                print("\nSKIP: no `SD card capacity` log — card likely not inserted")
                return EXIT_SKIP

            ls_buf, ls_ok = cmd(term, "ls /", timeout=6.0)
            if not ls_ok or b"No such directory" in ls_buf:
                sys.stdout.write("\n--- ls / ---\n" + ls_buf.decode(errors="replace"))
                print("\nFAIL: `/` not mounted — auto-mount did not run or failed")
                return EXIT_FAIL

            # ---- Phase 2: write a fresh nonce to the card ----
            cmd(term, f"rm {PROBE_FILE}")               # clear any prior run's file (ignore)
            wbuf, _ = cmd(term, f"echo {nonce} {PROBE_FILE}")
            if b"failed" in wbuf or b"[E/" in wbuf:
                sys.stdout.write("\n--- write ---\n" + wbuf.decode(errors="replace"))
                print("\nFAIL: write to card failed (echo could not open/write file)")
                return EXIT_FAIL

            # immediate roundtrip (may be served from pcache — not yet conclusive)
            cat1, _ = cmd(term, f"cat {PROBE_FILE}")
            if nonce.encode() not in cat1:
                sys.stdout.write("\n--- cat (pre-reset) ---\n" + cat1.decode(errors="replace"))
                print("\nFAIL: nonce not read back immediately after write")
                return EXIT_FAIL

            # ---- Phase 3: reset (drop pcache) then read back from the card ----
            boot2, msh_ok2, cap2 = boot_and_wait_mount(term)
            if not msh_ok2 or not cap2:
                sys.stdout.write("\n--- boot #2 ---\n" + boot2.decode(errors="replace"))
                print("\nFAIL: second boot did not reach a mounted msh prompt")
                return EXIT_FAIL

            cat2, _ = cmd(term, f"cat {PROBE_FILE}")
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    sys.stdout.write("\n--- cat (post-reset readback from card) ---\n")
    sys.stdout.write(cat2.decode(errors="replace"))
    sys.stdout.write("\n--- end ---\n")

    if nonce.encode() not in cat2:
        print(f"FAIL: nonce {nonce} not found after reset+remount — data did not persist to card")
        return EXIT_FAIL

    print(f"PASS: wrote {nonce} to {PROBE_FILE}, survived reset, read back from physical card")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
