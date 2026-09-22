#!/usr/bin/env python3
"""串口采集上位机：把固件发来的二进制帧落成 CSV（P1/P3 上机时要用的那一环）。

设计要点：
- **解析用 tools/protocol.py 的同一份实现**（与固件 C 侧已做字节级一致性测试：PPG/ECG/STATUS/LOG 四类帧逐字节相同），
  避免上位机另写一套解析。
- **双通道**：PPG（MAX30102，100 Hz）与 ECG（AD8232 + ADC，250 Hz）**各自独立成文件**，
  按各自的时间戳记录，不做重采样——重采样会引入假象，而两条链路的一致性本来就该按绝对时间对齐来比。
- 采集过程中**实时统计**帧数、样本数、CRC 错误、丢弃字节、丢包率：这些数字直接进 `docs/04-调试记录.md`。
- 无硬件也能自检：`--selftest` 用合成帧走完整链路（双通道解析 → 落两个 CSV → 回读校验）。

用法：
    ./.venv/bin/python tools/serial_capture.py --selftest
    ./.venv/bin/python tools/serial_capture.py --port /dev/tty.usbserial-XXXX --seconds 60 \\
        --out data/raw/capture-ppg.csv --out-ecg data/raw/capture-ecg.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools import protocol  # noqa: E402


class ChannelStats:
    """单个采集通道的统计（丢包率 = 1 − 实收样本 / 时间跨度应有样本）。"""

    def __init__(self, name: str, fs_hz: float) -> None:
        self.name = name
        self.fs_hz = fs_hz
        self.frames = 0
        self.samples = 0
        self.first_ts_ms: int | None = None
        self.last_sample_ms: float = 0.0

    def note(self, ts_ms: int, count: int) -> None:
        if self.first_ts_ms is None:
            self.first_ts_ms = ts_ms
        self.frames += 1
        self.samples += count
        if count:
            self.last_sample_ms = ts_ms + (count - 1) * (1000.0 / self.fs_hz)

    def expected_samples(self) -> int:
        if self.first_ts_ms is None:
            return 0
        span_ms = self.last_sample_ms - self.first_ts_ms
        # round 而不是 int：浮点误差会让样本数少 1，丢包率出现虚假的 3%
        return int(round(span_ms * self.fs_hz / 1000.0)) + 1

    def report(self) -> str:
        exp = self.expected_samples()
        loss = 0.0 if exp <= 0 else max(0.0, 100.0 * (exp - self.samples) / exp)
        return (f"{self.name}: 帧={self.frames} 样本={self.samples} 期望={exp} "
                f"丢包率={loss:.3f}%")


def write_csv(path: str, header: list[str], rows: list[tuple]) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        for row in rows:
            w.writerow(row)


def collect(stream: bytes, parser: protocol.Parser, ppg: ChannelStats, ecg: ChannelStats,
            ppg_rows: list, ecg_rows: list) -> None:
    """把字节流喂给解析器，按帧类型分通道落行。"""
    for frame in parser.feed(stream):
        if frame.type == protocol.TYPE_PPG_BATCH:
            ts_ms, pairs = frame.decode_ppg()
            ppg.note(ts_ms, len(pairs))
            step_ms = 1000.0 / ppg.fs_hz
            for i, (ir, red) in enumerate(pairs):
                ppg_rows.append((f"{(ts_ms + i * step_ms) / 1000.0:.6f}", ir, red))
        elif frame.type == protocol.TYPE_ECG_BATCH:
            ts_ms, samples = frame.decode_ecg()
            ecg.note(ts_ms, len(samples))
            step_ms = 1000.0 / ecg.fs_hz
            for i, value in enumerate(samples):
                ecg_rows.append((f"{(ts_ms + i * step_ms) / 1000.0:.6f}", value))


def run_selftest(out: str, out_ecg: str) -> int:
    """无硬件自检：合成双通道帧 → 切碎喂入（含前导噪声）→ 落 CSV → 回读校验。"""
    frames = []
    for seq in range(3):
        ppg_samples = [(1000 + seq * 10 + i, 2000 + seq * 10 + i) for i in range(10)]
        frames.append(protocol.ppg_batch(seq, seq * 100, ppg_samples).encode())
    for seq in range(3):
        ecg_samples = [1000 + seq * 10 + i for i in range(25)]     # 250 Hz：每帧 25 样本
        frames.append(protocol.ecg_batch(seq, seq * 100, ecg_samples).encode())
    frames.append(protocol.log(9, "selftest").encode())

    stream = b"\x00\xAA" + b"".join(frames)
    parser = protocol.Parser()
    ppg = ChannelStats("PPG", 100.0)
    ecg = ChannelStats("ECG", 250.0)
    ppg_rows: list[tuple] = []
    ecg_rows: list[tuple] = []
    for i in range(0, len(stream), 7):                             # 7 字节切片，模拟串口分片
        collect(stream[i:i + 7], parser, ppg, ecg, ppg_rows, ecg_rows)

    write_csv(out, ["time_s", "ppg_ir", "ppg_red"], ppg_rows)
    write_csv(out_ecg, ["time_s", "ecg"], ecg_rows)
    with open(out, newline="", encoding="utf-8") as fh:
        ppg_lines = sum(1 for _ in csv.reader(fh)) - 1
    with open(out_ecg, newline="", encoding="utf-8") as fh:
        ecg_lines = sum(1 for _ in csv.reader(fh)) - 1

    ok = (ppg_lines == 30 and ecg_lines == 75 and parser.crc_errors == 0)
    print(f"自检: PPG {ppg_lines} 行（期望 30）  ECG {ecg_lines} 行（期望 75）  "
          f"CRC错误={parser.crc_errors} 丢弃字节={parser.bytes_discarded}")
    print("      " + ppg.report())
    print("      " + ecg.report())
    print(("PASS" if ok else "FAIL") + "  双通道采集链路自检（解析 → 两个 CSV → 回读）")
    return 0 if ok else 1


def run_port(port: str, seconds: float, out: str, out_ecg: str | None,
             ppg_fs: float, ecg_fs: float) -> int:
    try:
        import serial  # 延迟导入：无硬件时不必安装
    except ImportError:
        raise SystemExit("需要 pyserial：./.venv/bin/pip install pyserial")
    import time

    parser = protocol.Parser()
    ppg = ChannelStats("PPG", ppg_fs)
    ecg = ChannelStats("ECG", ecg_fs)
    ppg_rows: list[tuple] = []
    ecg_rows: list[tuple] = []

    deadline = time.time() + seconds
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        print(f"采集 {seconds:.0f}s：{port} @115200 …")
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                collect(chunk, parser, ppg, ecg, ppg_rows, ecg_rows)

    if ppg_rows:
        write_csv(out, ["time_s", "ppg_ir", "ppg_red"], ppg_rows)
        print(f"PPG → {out}  ({ppg.report()})")
    if ecg_rows and out_ecg:
        write_csv(out_ecg, ["time_s", "ecg"], ecg_rows)
        print(f"ECG → {out_ecg}  ({ecg.report()})")
    if not ppg_rows and not ecg_rows:
        print("没有解析到任何数据帧：确认固件在跑、波特率 115200、接线 TX/RX 交叉")
        return 1
    print(f"CRC错误={parser.crc_errors} 丢弃字节={parser.bytes_discarded}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="串口采集上位机（二进制帧 → CSV，支持 PPG+ECG 双通道）")
    ap.add_argument("--port", help="串口设备，如 /dev/tty.usbserial-XXXX")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--fs", type=float, default=100.0, help="PPG 采样率（算丢包率用）")
    ap.add_argument("--ecg-fs", type=float, default=250.0, help="ECG 采样率（算丢包率用）")
    ap.add_argument("--out", default="data/raw/capture-ppg.csv")
    ap.add_argument("--out-ecg", default="data/raw/capture-ecg.csv")
    ap.add_argument("--selftest", action="store_true", help="无硬件自检（双通道）")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest(args.out, args.out_ecg)
    if not args.port:
        ap.error("需要 --port，或用 --selftest")
    return run_port(args.port, args.seconds, args.out, args.out_ecg, args.fs, args.ecg_fs)


if __name__ == "__main__":
    raise SystemExit(main())
