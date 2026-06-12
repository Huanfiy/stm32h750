#!/usr/bin/env python3
"""Aging-test business CAN protocol closed-loop test.

Exercises host/MCU protocol paths over the real ZQWL CAN adapter:

1. Config / bind / start ACKs and multi-channel periodic reports.
2. Completion state after configured duration.
3. STOP and RESET command ACKs.
4. Forced low-current event path (0x210) using an intentionally high minimum.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / ".." / "smart_bp_ag_app"))
from lib import jlink, zqwl_can  # noqa: E402
from smart_bp_ag.canbus import protocol  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77


def _wait_frames(zq: zqwl_can.ZqwlCan, seconds: float):
    """Fixed observation window — for negative checks (nothing may arrive)."""
    deadline = time.time() + seconds
    out = []
    while time.time() < deadline:
        out.extend(zq.recv(min(0.2, max(0.0, deadline - time.time()))))
    return out


def _wait_until(zq: zqwl_can.ZqwlCan, rx: list, cond, timeout: float) -> bool:
    """Extend `rx` until `cond(rx)` holds or `timeout` elapses.

    recv() wakes per received frame, so this returns within ms of the
    condition becoming true — the timeout is a cap, not a fixed wait."""
    deadline = time.time() + timeout
    while not cond(rx) and time.time() < deadline:
        rx.extend(zq.recv(deadline - time.time()))
    return cond(rx)


def _print_frames(label: str, frames) -> None:
    print(f"--- {label} ---")
    for can_id, payload, is_fd in frames:
        decoded = protocol.decode(can_id, payload)
        print(f"rx id=0x{can_id:03X} fd={int(is_fd)} data={payload.hex(' ').upper()} decoded={decoded}")


def _decoded(frames):
    for can_id, payload, _is_fd in frames:
        msg = protocol.decode(can_id, payload)
        if msg is not None:
            yield msg


def _has_ack(frames, ref_low: int, channel: int | None = None) -> bool:
    for msg in _decoded(frames):
        if isinstance(msg, protocol.Ack) and msg.ref_id_low == ref_low and msg.ok:
            if channel is None or msg.channel_no == channel:
                return True
    return False


def _has_nack(frames, ref_low: int, channel: int | None = None) -> bool:
    for msg in _decoded(frames):
        if isinstance(msg, protocol.Ack) and msg.ref_id_low == ref_low and not msg.ok:
            if channel is None or msg.channel_no == channel:
                return True
    return False


def _reports(frames, channel: int | None = None) -> list[protocol.Report]:
    out = []
    for msg in _decoded(frames):
        if isinstance(msg, protocol.Report) and (channel is None or msg.channel_no == channel):
            out.append(msg)
    return out


def _events(frames, channel: int | None = None) -> list[protocol.Event]:
    out = []
    for msg in _decoded(frames):
        if isinstance(msg, protocol.Event) and (channel is None or msg.channel_no == channel):
            out.append(msg)
    return out


def _send_all(zq: zqwl_can.ZqwlCan, frames, gap: float = 0.02) -> None:
    """Send frames back-to-back. The MCU's IRQ-fed RX ring preserves order and
    absorbs the burst, so responses are collected afterwards by _wait_until."""
    for can_id, payload in frames:
        print(f"tx id=0x{can_id:03X} data={payload.hex(' ').upper()}")
        zq.send(can_id, payload)
        time.sleep(gap)


def _send_wait_ack(zq: zqwl_can.ZqwlCan, frame, ref_low: int, timeout: float = 6.0):
    rx: list = []
    can_id, payload = frame
    print(f"tx id=0x{can_id:03X} data={payload.hex(' ').upper()}")
    zq.send(can_id, payload)
    _wait_until(zq, rx, lambda r: _has_ack(r, ref_low), timeout)
    return rx


def _bind_frames(channels: list[int], batch_no: int):
    frames = []
    for ch in channels:
        sn = f"SN{ch:04d}"
        frames.append(protocol.encode_bind_header(ch, sn, batch_no=batch_no))
        frames.extend(protocol.encode_sn_fragments(ch, sn))
    return frames


def _normal_multichannel(zq: zqwl_can.ZqwlCan) -> tuple[bool, list[str]]:
    channels = [1, 2, 3]
    mask = sum(1 << (ch - 1) for ch in channels)
    frames = [
        protocol.encode_config(report_period_100ms=5, duration_s=2, current_min_ma=0, current_max_ma=5000),
        *_bind_frames(channels, batch_no=1),
        protocol.encode_control(protocol.CMD_START, batch_no=1, channel_mask=mask),
    ]
    _send_all(zq, frames)

    def _missing(rx) -> list[str]:
        out = []
        if not _has_ack(rx, 0x00):
            out.append("config ACK ref=0x00")
        for ch in channels:
            if not _has_ack(rx, 0x10, channel=ch):
                out.append(f"bind ACK channel={ch}")
            if not _reports(rx, ch):
                out.append(f"report channel={ch}")
        if not _has_ack(rx, 0x20):
            out.append("start ACK ref=0x20")
        if not any(r.mcu_state == 2 for r in _reports(rx)):
            out.append("completion report state=2")
        return out

    rx: list = []
    _wait_until(zq, rx, lambda r: not _missing(r), 10.0)
    _print_frames("normal-multichannel", rx)
    missing = _missing(rx)
    return not missing, missing


def _stop_reset(zq: zqwl_can.ZqwlCan) -> tuple[bool, list[str]]:
    rx = []
    rx.extend(_send_wait_ack(
        zq, protocol.encode_config(report_period_100ms=5, duration_s=10, current_min_ma=0, current_max_ma=5000), 0x00
    ))
    rx.extend(_send_wait_ack(zq, protocol.encode_control(protocol.CMD_START, batch_no=2, channel_mask=0x0001), 0x20))
    rx.extend(_wait_frames(zq, 1.2))
    rx.extend(_send_wait_ack(zq, protocol.encode_control(protocol.CMD_STOP, batch_no=2, channel_mask=0x0001), 0x20))
    reports_at_stop_ack = len(_reports(rx))
    rx.extend(_wait_frames(zq, 1.5))
    reports_after_wait = len(_reports(rx))
    rx.extend(_send_wait_ack(zq, protocol.encode_control(protocol.CMD_RESET, batch_no=2, channel_mask=0x0001), 0x20))
    _print_frames("stop-reset", rx)

    missing = []
    control_acks = [m for m in _decoded(rx) if isinstance(m, protocol.Ack) and m.ref_id_low == 0x20 and m.ok]
    if len(control_acks) < 3:
        missing.append("start/stop/reset ACKs")
    if reports_after_wait != reports_at_stop_ack:
        missing.append("reports stopped after STOP")
    return not missing, missing


def _forced_low_event(zq: zqwl_can.ZqwlCan) -> tuple[bool, list[str]]:
    _send_all(zq, [
        protocol.encode_config(report_period_100ms=5, duration_s=2, current_min_ma=5000, current_max_ma=6000),
        protocol.encode_control(protocol.CMD_START, batch_no=3, channel_mask=0x0001),
    ])

    def _missing(rx) -> list[str]:
        out = []
        if not _events(rx, 1):
            out.append("0x210 event channel=1")
        if not any(evt.event_type == 0x02 for evt in _events(rx, 1)):
            out.append("LOW_CURRENT event_type=0x02")
        if not any(r.mcu_state == 3 and r.error_code == 0x02 for r in _reports(rx, 1)):
            out.append("report abnormal state/error LOW_CURRENT")
        return out

    rx: list = []
    _wait_until(zq, rx, lambda r: not _missing(r), 4.0)
    _print_frames("forced-low-event", rx)
    missing = _missing(rx)
    return not missing, missing


def _disabled_channels_rejected(zq: zqwl_can.ZqwlCan) -> tuple[bool, list[str]]:
    # Expected responses here are NACKs, so wait on _has_nack — the old
    # _send_wait_ack(ok-ACK) loops could never match and burned 2 × 6 s.
    rx: list = []
    _send_all(zq, [protocol.encode_bind_header(7, "SN0007", batch_no=4)])
    _wait_until(zq, rx, lambda r: _has_nack(r, 0x10, channel=7), 3.0)
    _send_all(zq, [protocol.encode_control(protocol.CMD_START, batch_no=4, channel_mask=0x0040)])
    _wait_until(zq, rx, lambda r: _has_nack(r, 0x20), 3.0)
    rx.extend(_wait_frames(zq, 1.0))   # negative window: channel 7 must stay silent
    _print_frames("disabled-channels", rx)

    missing = []
    if not _has_nack(rx, 0x10, channel=7):
        missing.append("bind NACK channel=7")
    if not _has_nack(rx, 0x20):
        missing.append("start NACK when mask only contains disabled channel")
    if _reports(rx, 7):
        missing.append("disabled channel 7 unexpectedly reported")
    return not missing, missing


def main() -> int:
    if not jlink.have_jlink():
        print("SKIP: JLinkExe not in PATH")
        return EXIT_SKIP
    if not zqwl_can.device_present():
        print(f"SKIP: ZQWL device {zqwl_can.DEFAULT_DEV} not present")
        return EXIT_SKIP

    try:
        checks = []
        for name, fn in (
            ("normal multi-channel", _normal_multichannel),
            ("stop/reset", _stop_reset),
            ("forced low-current event", _forced_low_event),
            ("disabled channel rejection", _disabled_channels_rejected),
        ):
            t0 = time.time()
            jlink.reset_run()
            time.sleep(0.8)
            with zqwl_can.ZqwlCan() as zq:
                zq.flush_input()
                ok, missing = fn(zq)
                checks.append((name, ok, missing))
            print(f"[time] {name}: {time.time() - t0:.2f}s")
    except jlink.JLinkUnavailable as exc:
        print(f"SKIP: J-Link unreachable: {exc}")
        return EXIT_SKIP
    except OSError as exc:
        print(f"SKIP: device open failed: {exc}")
        return EXIT_SKIP

    failed = []
    for name, ok, missing in checks:
        if ok:
            print(f"PASS: {name}")
        else:
            failed.append(f"{name}: {', '.join(missing)}")
    if failed:
        print("FAIL: " + " | ".join(failed))
        return EXIT_FAIL

    print("PASS: business CAN protocol ACK/report/control/event paths verified")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
