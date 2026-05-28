"""J-Link SWD harness for STM32H750VB.

Wraps `JLinkExe -CommandFile` so closed-loop tests can probe the target without
spawning shell pipelines themselves. Single-word `read32` is the workhorse —
multi-word reads are batched by submitting multiple `mem32 <addr>,1` commands
in one J-Link session to keep total connect cost low.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile

DEVICE = "STM32H750VB"
SPEED_KHZ = 4000
CONNECT_TIMEOUT_S = 20


class JLinkUnavailable(RuntimeError):
    """Raised when JLinkExe is not installed or cannot connect to the target."""


def have_jlink() -> bool:
    return shutil.which("JLinkExe") is not None


def _run(body_cmds: list[str]) -> str:
    if not have_jlink():
        raise JLinkUnavailable("JLinkExe not found in PATH")
    body = "\n".join(
        ["si SWD", f"speed {SPEED_KHZ}", f"device {DEVICE}", "connect", *body_cmds, "exit"]
    )
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as fh:
        fh.write(body + "\n")
        cmd_path = fh.name
    try:
        proc = subprocess.run(
            ["JLinkExe", "-AutoConnect", "1", "-ExitOnError", "1", "-CommandFile", cmd_path],
            capture_output=True,
            text=True,
            timeout=CONNECT_TIMEOUT_S,
        )
    finally:
        os.unlink(cmd_path)
    if proc.returncode != 0:
        raise JLinkUnavailable(
            f"JLinkExe exited {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout


_MEM32_RE = re.compile(r"^([0-9A-Fa-f]{8})\s*=\s*([0-9A-Fa-f]{8})", re.M)


def read32_many(addrs: list[int]) -> dict[int, int]:
    """Halt → read each address as one mem32 → resume. Returns {addr: value}."""
    cmds = ["h"]
    for a in addrs:
        cmds.append(f"mem32 0x{a:08X},1")
    cmds.append("g")
    out = _run(cmds)
    found: dict[int, int] = {}
    for m in _MEM32_RE.finditer(out):
        a = int(m.group(1), 16)
        v = int(m.group(2), 16)
        if a in addrs and a not in found:
            found[a] = v
    missing = [f"0x{a:08X}" for a in addrs if a not in found]
    if missing:
        raise JLinkUnavailable(f"failed to read addresses: {', '.join(missing)}\nraw output:\n{out}")
    return found


def read32(addr: int) -> int:
    return read32_many([addr])[addr]


_REG_RE = re.compile(r"([A-Z][A-Z0-9_]*)\s*=\s*([0-9A-Fa-f]{8})")


def halt_and_regs() -> dict[str, int]:
    """Halt the core and return CPU register snapshot ({R0..R15, MSP, PSP, xPSR, ...} → value)."""
    out = _run(["h", "regs", "g"])
    regs: dict[str, int] = {}
    for m in _REG_RE.finditer(out):
        regs[m.group(1)] = int(m.group(2), 16)
    if not regs:
        raise JLinkUnavailable(f"no registers parsed\nraw output:\n{out}")
    return regs


def reset_run() -> None:
    """Issue a reset+resume cycle (equivalent to `./run.sh reset`)."""
    _run(["r", "g"])
