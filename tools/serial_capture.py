#!/usr/bin/env python3
"""串口采集上位机：把固件发来的二进制帧落成 CSV（P1 上机时要用的那一环）。

设计要点：
- **解析用 tools/protocol.py 的同一份实现**（与固件 C 侧已做字节级一致性测试），避免上位机另写一套解析。
- 采集过程中**实时统计**：帧数、样本数、CRC 错误、丢弃字节、丢包率——这些数字直接进 `docs/04-调试记录.md`，
  是「采集链路是否健康」的客观依据，而不是靠肉眼看波形。
- 无硬件也能自检：`--selftest` 用合成帧走完整链路（解析 → 落 CSV → 回读校验）。

用法：
    ./.venv/bin/python tools/serial_capture.py --selftest
    ./.venv/bin/python tools/serial_capture.py --port /dev/tty.usbserial-XXXX --seconds 30 --out data/raw/capture.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools import protocol  # noqa: E402


class CaptureStats:
    def __init__(self, fs_hz: float = 100.0) -> None:
        self.fs_hz = fs_hz
        self.frames = 0
        self.samples = 0
        self.crc_errors = 0
        self.discarded = 0
        self.first_ts_ms: int | None = None
        self.last_ts_ms: int | None = None
        self.last_sample_ms: float = 0.0

    def add(self, frame: protocol.Frame) -> list[tuple[float, int, int]]:
        self.frames += 1
        if frame.type != protocol.TYPE_PPG_BATCH:
            return []
        ts_ms, pairs = frame.decode_ppg()
        if self.first_ts_ms is None:
            self.first_ts_ms = ts_ms
        self.last_ts_ms = ts_ms
        self.samples += len(pairs)
        # 时间戳按采样间隔线性展开（固件每批只带首个样本时间戳，间隔由采样率决定）
        step_ms = 1000.0 / self.fs_hz
        if pairs:
            self.last_sample_ms = ts_ms + (len(pairs) - 1) * step_ms
        return [((ts_ms + i * step_ms) / 1000.0, ir, red) for i, (ir, red) in enumerate(pairs)]

    def expected_samples(self, fs_hz: float) -> int:
        """按「首个样本时间戳 → 最后一个样本时间戳」推算应有样本数（用于丢包率）。"""
        if self.first_ts_ms is None or self.last_ts_ms is None:
            return 0
        span_ms = self.last_sample_ms - self.first_ts_ms
        # round 而不是 int：0.29*100 这种浮点误差会让样本数少 1（丢包率出现虚假 3%）
        return int(round(span_ms * fs_hz / 1000.0)) + 1

    def report(self, fs_hz: float) -> str:
        exp = self.expected_samples(fs_hz)
        loss = 0.0 if exp <= 0 else max(0.0, 100.0 * (exp - self.samples) / exp)
        return (f"帧数={self.frames}  样本={self.samples}  期望={exp}  "
                f"丢包率={loss:.3f}%  CRC错误={self.crc_errors}  丢弃字节={self.discarded}")


def write_rows(out: str, rows: list[tuple[float, int, int]]) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ppg_ir", "ppg_red"])
        for t, ir, red in rows:
            w.writerow([f"{t:.6f}", ir, red])


def run_selftest(out: str) -> int:
    """无硬件自检：合成 3 帧 → 逐块喂给解析器（故意切碎 + 插入噪声）→ 落 CSV → 回读校验。"""
    parser = protocol.Parser()
    stats = CaptureStats(fs_hz=100.0)
    rows: list[tuple[float, int, int]] = []

    payload_frames = []
    for seq in range(3):
        samples = [(1000 + seq * 10 + i, 2000 + seq * 10 + i) for i in range(10)]
        payload_frames.append(protocol.ppg_batch(seq, seq * 100, samples).encode())
    payload_frames.append(protocol.log(9, "selftest").encode())

    stream = b"\x00\xAA" + b"".join(payload_frames)          # 前面塞噪声
    for i in range(0, len(stream), 7):                       # 按 7 字节切碎，模拟串口分片
        for frame in parser.feed(stream[i:i + 7]):
            rows.extend(stats.add(frame))
    stats.crc_errors = parser.crc_errors
    stats.discarded = parser.bytes_discarded

    write_rows(out, rows)
    with open(out, newline="", encoding="utf-8") as fh:
        count = sum(1 for _ in csv.reader(fh)) - 1

    ok = count == 30 and stats.frames == 4 and stats.samples == 30
    print(f"自检: 写入 {count} 行（期望 30）  {stats.report(100.0)}")
    print(("PASS" if ok else "FAIL") + "  串口采集链路自检（解析 → CSV → 回读）")
    return 0 if ok else 1


def run_port(port: str, seconds: float, out: str, fs_hz: float) -> int:
    try:
        import serial  # 延迟导入：无硬件时不必安装
    except ImportError:
        raise SystemExit("需要 pyserial：./.venv/bin/pip install pyserial")

    parser = protocol.Parser()
    stats = CaptureStats(fs_hz=fs_hz)
    rows: list[tuple[float, int, int]] = []
    import time
    deadline = time.time() + seconds
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        print(f"采集 {seconds:.0f}s：{port} @115200 …")
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            for frame in parser.feed(chunk):
                rows.extend(stats.add(frame))
    stats.crc_errors = parser.crc_errors
    stats.discarded = parser.bytes_discarded
    write_rows(out, rows)
    print(f"已写入 {out}")
    print(stats.report(fs_hz))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="串口采集上位机（二进制帧 → CSV）")
    ap.add_argument("--port", help="串口设备，如 /dev/tty.usbserial-XXXX")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--fs", type=float, default=100.0, help="固件采样率（用于丢包率计算）")
    ap.add_argument("--out", default="data/raw/capture.csv")
    ap.add_argument("--selftest", action="store_true", help="无硬件自检")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest(args.out)
    if not args.port:
        ap.error("需要 --port，或用 --selftest")
    return run_port(args.port, args.seconds, args.out, args.fs)


if __name__ == "__main__":
    raise SystemExit(main())
