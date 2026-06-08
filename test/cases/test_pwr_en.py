#!/usr/bin/env python3
"""PWR_EN (app_drv_gpio) closed-loop test.

Drives the `pwr_en` msh command over serial and verifies the *physical* GPIO
output-data register via J-Link SWD — not just the firmware's own readback.

For each channel under test we:
  - `pwr_en <n> 1`  → expect the pin's GPIOx_ODR bit set
  - `pwr_en <n> 0`  → expect the pin's GPIOx_ODR bit clear

Channel coverage spans the 14 GPIO-owned PWR_EN outputs. PWR_EN7/PD2 and
PWR_EN15/PE2 are deliberately excluded because the board reserves those nets for
SDMMC_CMD and QSPI/XIP respectively.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import jlink, serial_term  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

# STM32H7 GPIO: GPIOA_BASE = 0x5802_0000, +0x400 per port, ODR at offset 0x14.
_GPIO_BASE = 0x58020000
_ODR_OFF = 0x14


def _odr(port: str) -> int:
    return _GPIO_BASE + (ord(port) - ord("A")) * 0x400 + _ODR_OFF


# (PWR_EN channel number, port letter, pin bit). The 2 reserved channels are
# excluded because app_drv_gpio intentionally refuses to drive them:
#   PWR_EN7/PD2 = SDMMC1_CMD, PWR_EN15/PE2 = QUADSPI_BK1_IO2 / XIP.
CHANNELS = [
    (1, "E", 1),    # PWR_EN1  PE1
    (2, "B", 9),    # PWR_EN2  PB9
    (3, "B", 7),    # PWR_EN3  PB7
    (4, "B", 5),    # PWR_EN4  PB5
    (5, "B", 3),    # PWR_EN5  PB3
    (6, "D", 6),    # PWR_EN6  PD6
    (8, "D", 4),    # PWR_EN8  PD4
    (9, "E", 7),    # PWR_EN9  PE7
    (10, "E", 9),   # PWR_EN10 PE9
    (11, "E", 13),  # PWR_EN11 PE13
    (12, "E", 11),  # PWR_EN12 PE11
    (13, "E", 12),  # PWR_EN13 PE12
    (14, "E", 10),  # PWR_EN14 PE10
    (16, "E", 8),   # PWR_EN16 PE8
]


def _read_bits(term: serial_term.Term) -> dict[str, int]:
    addrs = sorted({_odr(p) for _, p, _ in CHANNELS})
    return jlink.read32_many(addrs)


def _check(level: int) -> tuple[bool, list[str]]:
    """Drive every channel to `level`, then assert each ODR bit matches."""
    fails: list[str] = []
    with serial_term.Term() as term:
        term.read(0.2)
        for n, _, _ in CHANNELS:
            term.send_line(f"pwr_en {n} {level}")
        term.read(0.3)  # let the last command settle before halting the core
        odr = jlink.read32_many(sorted({_odr(p) for _, p, _ in CHANNELS}))

    for n, port, bit in CHANNELS:
        actual = (odr[_odr(port)] >> bit) & 1
        if actual != level:
            fails.append(f"PWR_EN{n} (P{port}{bit}): ODR bit={actual}, want {level}")
    return (not fails), fails


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not present")
        return EXIT_SKIP
    if not serial_term.device_present():
        print(f"SKIP: serial device {serial_term.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        for level in (1, 0):
            ok, fails = _check(level)
            tag = "HIGH" if level else "LOW"
            if not ok:
                print(f"FAIL: drive {tag} mismatch:")
                for f in fails:
                    print(f"  {f}")
                return EXIT_FAIL
            print(f"PASS: {len(CHANNELS)} channels read back {tag} in GPIO ODR")
    except jlink.JLinkUnavailable as exc:
        print(f"SKIP: J-Link could not access target: {exc}")
        return EXIT_SKIP
    except OSError as exc:
        print(f"SKIP: serial open failed: {exc}")
        return EXIT_SKIP

    # Leave the fixture in a safe state: all channels off.
    try:
        with serial_term.Term() as term:
            term.send_line("pwr_en all 0")
            term.read(0.2)
    except OSError:
        pass

    print(f"PASS: PWR_EN GPIO control verified on {len(CHANNELS)}/16 channels (PD2/EN7 and PE2/EN15 excluded)")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
