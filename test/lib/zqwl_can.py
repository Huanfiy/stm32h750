"""ZQWL UCANFD-100C CAN box driver (vendor binary protocol over USB-CDC).

The device is NOT a SocketCAN interface — config and CAN frames travel over
`/dev/ttyACM*` as fixed-format vendor packets:

  config: 49 3B <func> <rw> <16B data> 45 2E
  CAN tx: 5A <info1> <info2> <4B id> <data...> A5
  heartbeat (skip in parser): info1 = 0xFF (1ch) or 0xFE (4ch)

Bitrate code is `(arbitration_nibble << 4) | data_nibble`. Classic CAN ignores
the data-bitrate field; we set both to 500k → `0x25`.

See `/home/huan/tool/ZQWL-UCANFD-100C/UCANFD/MANUAL/...二次开发通讯协议_V1.05.pdf`
for the authoritative protocol spec.
"""

from __future__ import annotations

import glob
import errno
import fcntl
import os
import select
import struct
import termios
import time
from typing import Iterable

BITRATE_500K_CLASSIC = 0x25
DEFAULT_DEV = "/dev/ttyACM1"
DEFAULT_SERIAL_BAUDRATE = 6_000_000
TCGETS2 = 0x802C542A
TCSETS2 = 0x402C542B
BOTHER = 0o010000
CBAUD = 0o010017
TERMIOS2_FMT = "IIIIB19BII"


def find_devices() -> list[str]:
    """Return ZQWL UCANFD device nodes via /dev/serial/by-id symlinks (most stable)."""
    return sorted(
        os.path.realpath(p)
        for p in glob.glob("/dev/serial/by-id/*ZQWL-CANFD*")
    )


def device_present(path: str = DEFAULT_DEV) -> bool:
    return os.path.exists(path)


def _set_baudrate(fd: int, baudrate: int) -> None:
    if hasattr(termios, f"B{baudrate}"):
        speed = getattr(termios, f"B{baudrate}")
        attrs = termios.tcgetattr(fd)
        attrs[4] = speed
        attrs[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return

    size = struct.calcsize(TERMIOS2_FMT)
    raw = fcntl.ioctl(fd, TCGETS2, bytes(size))
    values = list(struct.unpack(TERMIOS2_FMT, raw))
    values[2] &= ~CBAUD
    values[2] |= BOTHER | termios.CS8 | termios.CREAD | termios.CLOCAL
    values[-2] = baudrate
    values[-1] = baudrate
    fcntl.ioctl(fd, TCSETS2, struct.pack(TERMIOS2_FMT, *values))


def _open(path: str, baudrate: int = DEFAULT_SERIAL_BAUDRATE) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B38400
    attrs[5] = termios.B38400
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    _set_baudrate(fd, baudrate)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def _write_all(fd: int, data: bytes, timeout_s: float = 2.0) -> None:
    end = time.time() + timeout_s
    offset = 0
    while offset < len(data):
        try:
            written = os.write(fd, data[offset:])
        except BlockingIOError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise
            written = 0
        except InterruptedError:
            written = 0
        if written > 0:
            offset += written
            continue
        remaining = end - time.time()
        if remaining <= 0:
            raise TimeoutError("ZQWL serial write timeout")
        _r, writable, _e = select.select([], [fd], [], remaining)
        if not writable and time.time() >= end:
            raise TimeoutError("ZQWL serial write timeout")


def _cfg_packet(func: int, rw: int, data: bytes = b"") -> bytes:
    return bytes([0x49, 0x3B, func, rw]) + bytes(data).ljust(16, b"\x00") + bytes([0x45, 0x2E])


def _system_control_payload(channel: int, enabled: bool) -> bytes:
    data = bytearray(16)
    data[0] = 0x01  # apply settings without writing them to adapter flash
    data[2 + channel] = 0x01 if enabled else 0x00
    return bytes(data)


def _frame_packet(can_id: int, payload: bytes, ext: bool, fd_proto: bool) -> bytes:
    if not (0 <= can_id <= (0x1FFFFFFF if ext else 0x7FF)):
        raise ValueError(f"can_id out of range for ext={ext}: 0x{can_id:X}")
    if len(payload) > 8 and not fd_proto:
        raise ValueError("classic CAN payload limited to 8 bytes")
    info1 = len(payload) & 0x0F  # channel low bit = 0
    info2 = 0x00                  # standard frame, data frame, no BRS, channel high2 = 0
    proto_flag = 0x80 if fd_proto else 0x00
    # BYTE3 bit7 = FDCAN flag; ID is big-endian over BYTE3..6
    raw_id = can_id.to_bytes(4, "big")
    id_bytes = bytes([raw_id[0] | proto_flag]) + raw_id[1:]
    return bytes([0x5A, info1, info2]) + id_bytes + bytes(payload) + bytes([0xA5])


def _drain(fd: int, seconds: float) -> bytes:
    end = time.time() + seconds
    buf = bytearray()
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0.0, end - time.time()))
        if r:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                buf.extend(chunk)
    return bytes(buf)


def _drain_until_frame(fd: int, seconds: float) -> bytes:
    end = time.time() + seconds
    buf = bytearray()
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0.0, end - time.time()))
        if r:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                buf.extend(chunk)
                if parse_frames(bytes(buf)):
                    break
    return bytes(buf)


def _extract_frames(buf: bytearray) -> list[tuple[int, bytes, bool]]:
    """Extract complete CAN frames from a persistent USB stream buffer."""
    out: list[tuple[int, bytes, bool]] = []
    while buf:
        if buf[0] != 0x5A:
            try:
                next_start = buf.index(0x5A)
            except ValueError:
                buf.clear()
                break
            del buf[:next_start]
            continue

        if len(buf) < 3:
            break
        info1 = buf[1]
        if info1 in (0xFF, 0xFE):
            hb_len = 17 if info1 == 0xFF else 32
            if len(buf) < hb_len:
                next_start = buf.find(0x5A, 1)
                if next_start > 0:
                    del buf[:next_start]
                    continue
                break
            if buf[hb_len - 1] == 0xA5:
                del buf[:hb_len]
                continue
            del buf[0]
            continue

        dlc = info1 & 0x0F
        frame_len = 1 + 1 + 1 + 4 + dlc + 1
        if len(buf) < frame_len:
            next_start = buf.find(0x5A, 1)
            if next_start > 0:
                del buf[:next_start]
                continue
            break
        if buf[frame_len - 1] != 0xA5:
            del buf[0]
            continue

        raw = bytes(buf[:frame_len])
        is_fd = bool(raw[3] & 0x80)
        can_id = int.from_bytes(bytes([raw[3] & 0x7F]) + raw[4:7], "big")
        out.append((can_id, bytes(raw[7:-1]), is_fd))
        del buf[:frame_len]
    return out


def parse_frames(buf: bytes) -> list[tuple[int, bytes, bool]]:
    """Walk USB buffer, return list of (can_id, payload, is_fd). Heartbeats skipped."""
    return _extract_frames(bytearray(buf))


class ZqwlCan:
    """Context-managed ZQWL CAN box. One channel (CAN0) brought up at the given bitrate."""

    def __init__(self, path: str = DEFAULT_DEV, bitrate_code: int = BITRATE_500K_CLASSIC) -> None:
        self.path = path
        self.fd = _open(path)
        self._rx_buf = bytearray()
        try:
            self._init(bitrate_code)
        except Exception:
            os.close(self.fd)
            raise

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self) -> "ZqwlCan":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _init(self, bitrate_code: int) -> None:
        _write_all(self.fd, _cfg_packet(0x40, 0x52))                                  # device info (probes link)
        _drain(self.fd, 0.15)
        _write_all(self.fd, _cfg_packet(0x42, 0x57, bytes([0x00, 0x00, bitrate_code]))) # CAN0 bitrate
        _drain(self.fd, 0.05)
        _write_all(self.fd, _cfg_packet(0x44, 0x57, _system_control_payload(0, True))) # apply (no flash) + open CAN0
        _drain(self.fd, 0.3)

    def send(self, can_id: int, payload: bytes | Iterable[int], ext: bool = False, fd_proto: bool = False) -> None:
        payload = bytes(payload)
        _write_all(self.fd, _frame_packet(can_id, payload, ext, fd_proto))

    def recv(self, seconds: float) -> list[tuple[int, bytes, bool]]:
        end = time.time() + seconds
        while True:
            frames = _extract_frames(self._rx_buf)
            if frames or time.time() >= end:
                return frames

            r, _, _ = select.select([self.fd], [], [], max(0.0, end - time.time()))
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                self._rx_buf.extend(chunk)

    def raw_drain(self, seconds: float) -> bytes:
        data = bytes(self._rx_buf)
        self._rx_buf.clear()
        return data + _drain(self.fd, seconds)
