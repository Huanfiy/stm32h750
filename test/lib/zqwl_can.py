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
import os
import select
import termios
import time
from typing import Iterable

BITRATE_500K_CLASSIC = 0x25
DEFAULT_DEV = "/dev/ttyACM1"


def find_devices() -> list[str]:
    """Return ZQWL UCANFD device nodes via /dev/serial/by-id symlinks (most stable)."""
    return sorted(
        os.path.realpath(p)
        for p in glob.glob("/dev/serial/by-id/*ZQWL-CANFD*")
    )


def device_present(path: str = DEFAULT_DEV) -> bool:
    return os.path.exists(path)


def _open(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def _cfg_packet(func: int, rw: int, data: bytes = b"") -> bytes:
    return bytes([0x49, 0x3B, func, rw]) + bytes(data).ljust(16, b"\x00") + bytes([0x45, 0x2E])


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


def parse_frames(buf: bytes) -> list[tuple[int, bytes, bool]]:
    """Walk USB buffer, return list of (can_id, payload, is_fd). Heartbeats skipped."""
    out: list[tuple[int, bytes, bool]] = []
    i = 0
    while i < len(buf):
        if buf[i] != 0x5A:
            i += 1
            continue
        if i + 2 >= len(buf):
            break
        info1 = buf[i + 1]
        if info1 in (0xFF, 0xFE):
            hb_len = 17 if info1 == 0xFF else 32
            if i + hb_len <= len(buf) and buf[i + hb_len - 1] == 0xA5:
                i += hb_len
            else:
                i += 1
            continue
        dlc = info1 & 0x0F
        frame_len = 1 + 1 + 1 + 4 + dlc + 1
        if i + frame_len <= len(buf) and buf[i + frame_len - 1] == 0xA5:
            raw = buf[i : i + frame_len]
            is_fd = bool(raw[3] & 0x80)
            can_id = int.from_bytes(bytes([raw[3] & 0x7F]) + raw[4:7], "big")
            out.append((can_id, bytes(raw[7:-1]), is_fd))
            i += frame_len
        else:
            i += 1
    return out


class ZqwlCan:
    """Context-managed ZQWL CAN box. One channel (CAN0) brought up at the given bitrate."""

    def __init__(self, path: str = DEFAULT_DEV, bitrate_code: int = BITRATE_500K_CLASSIC) -> None:
        self.path = path
        self.fd = _open(path)
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
        os.write(self.fd, _cfg_packet(0x40, 0x52))                                    # device info (probes link)
        _drain(self.fd, 0.15)
        os.write(self.fd, _cfg_packet(0x42, 0x57, bytes([0x00, 0x00, bitrate_code]))) # CAN0 bitrate
        _drain(self.fd, 0.05)
        os.write(self.fd, _cfg_packet(0x44, 0x57, bytes([0x00, 0x00, 0x01])))         # apply (no flash) + open CAN0
        _drain(self.fd, 0.3)

    def send(self, can_id: int, payload: bytes | Iterable[int], ext: bool = False, fd_proto: bool = False) -> None:
        payload = bytes(payload)
        os.write(self.fd, _frame_packet(can_id, payload, ext, fd_proto))

    def recv(self, seconds: float) -> list[tuple[int, bytes, bool]]:
        return parse_frames(_drain(self.fd, seconds))

    def raw_drain(self, seconds: float) -> bytes:
        return _drain(self.fd, seconds)
