"""Standard-library serial driver for the board's msh console (USART1 @ PB14/PB15).

`Term.send_line()` paces characters at ~25 chars/s — the RT-Thread finsh shell
drops bytes when fed at full 115200 wire speed because its single-char input
ring runs in the (low-priority) tshell thread, not the UART IRQ.
"""

from __future__ import annotations

import os
import re
import select
import termios
import time

DEFAULT_DEV = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200
CHAR_DELAY_S = 0.04


def device_present(path: str = DEFAULT_DEV) -> bool:
    return os.path.exists(path)


def _baud_const(baud: int) -> int:
    attr = f"B{baud}"
    if not hasattr(termios, attr):
        raise ValueError(f"unsupported baud rate {baud}")
    return getattr(termios, attr)


def _open(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = _baud_const(baud)
    attrs[5] = _baud_const(baud)
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


class Term:
    """Context-managed terminal handle."""

    def __init__(self, path: str = DEFAULT_DEV, baud: int = DEFAULT_BAUD) -> None:
        self.path = path
        self.baud = baud
        self.fd = _open(path, baud)

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self) -> "Term":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def read(self, seconds: float) -> bytes:
        """Drain whatever the device sends within `seconds`."""
        end = time.time() + seconds
        buf = bytearray()
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], max(0.0, end - time.time()))
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if chunk:
                    buf.extend(chunk)
        return bytes(buf)

    def send_line(self, line: str, char_delay: float = CHAR_DELAY_S) -> None:
        """Send `line` followed by CRLF, pacing one char at a time."""
        for ch in line:
            os.write(self.fd, ch.encode())
            time.sleep(char_delay)
        os.write(self.fd, b"\r\n")

    def expect(self, pattern: str | bytes, timeout: float) -> tuple[bytes, bool]:
        """Read until `pattern` (regex, str or bytes) matches or timeout elapses."""
        if isinstance(pattern, str):
            pattern = pattern.encode()
        end = time.time() + timeout
        buf = bytearray()
        regex = re.compile(pattern)
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], max(0.0, end - time.time()))
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if chunk:
                    buf.extend(chunk)
                    if regex.search(bytes(buf)):
                        return bytes(buf), True
        return bytes(buf), False
