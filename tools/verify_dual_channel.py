#!/usr/bin/env python3
"""P3 验收工具：PPG 与 ECG 双通道心率一致性对照。

为什么这是 P3 的核心验收：
    PPG 心率没有独立真值，而 ECG 有明确的 R 波。两条**独立链路**（光学 vs 电学、
    不同采样率、不同算法）在同一段时间给出相近心率，才说明整条采集链路是可靠的——
    这是现场最直观、也最难造假的证据。

做法：把两条通道各自按绝对时间切成 30 s 窗口，分别用心率算法算 HR，逐窗口对照。
不做重采样、不插值：两个通道各用自己的采样率，只对齐时间轴。

用法：
    ./.venv/bin/python tools/verify_dual_channel.py \
        --ppg data/raw/capture-ppg.csv --ecg data/raw/capture-ecg.csv \
        --ppg-fs 100 --ecg-fs 250 --window 30 --limit 5
    ./.venv/bin/python tools/verify_dual_channel.py --from-bidmc bidmc01   # 用公开配对数据自测
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from tools import ppg_hr  # noqa: E402
from ecg_qrs import ecg_qrs_detect, hr_from_peaks  # noqa: E402


def read_csv_column(path: str, column: str) -> tuple[np.ndarray, np.ndarray, float]:
    """返回 (time_s, values, fs)。fs 从时间戳中位数间隔推算。"""
    times: list[float] = []
    values: list[float] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if column not in (reader.fieldnames or []):
            raise SystemExit(f"{path} 里没有列 {column!r}，可用列：{reader.fieldnames}")
        for row in reader:
            times.append(float(row["time_s"]))
            values.append(float(row[column]))
    t = np.asarray(times, dtype=float)
    v = np.asarray(values, dtype=float)
    fs = 100.0
    if len(t) > 2:
        dt = float(np.median(np.diff(t)))
        fs = 1.0 / dt if dt > 0 else 100.0
    return t, v, fs


def windowed(t: np.ndarray, v: np.ndarray, start: float, length: float) -> np.ndarray:
    mask = (t >= start) & (t < start + length)
    return v[mask]


def compare(ppg_t: np.ndarray, ppg_v: np.ndarray, ppg_fs: float,
            ecg_t: np.ndarray, ecg_v: np.ndarray, ecg_fs: float,
            window_s: float = 30.0, limit_bpm: float = 5.0) -> tuple[list[dict], bool]:
    start = max(float(ppg_t[0]), float(ecg_t[0]))
    end = min(float(ppg_t[-1]), float(ecg_t[-1]))
    rows: list[dict] = []
    while start + window_s <= end:
        seg_ppg = windowed(ppg_t, ppg_v, start, window_s)
        seg_ecg = windowed(ecg_t, ecg_v, start, window_s)
        if len(seg_ppg) > ppg_fs and len(seg_ecg) > ecg_fs:
            hr_ppg = ppg_hr.analyse(seg_ppg, ppg_fs)["hr_bpm"]
            hr_ecg = hr_from_peaks(ecg_qrs_detect(seg_ecg, ecg_fs), ecg_fs)
            if not (np.isnan(hr_ppg) or np.isnan(hr_ecg)):
                rows.append({
                    "start_s": round(start, 1),
                    "hr_ppg": round(float(hr_ppg), 1),
                    "hr_ecg": round(float(hr_ecg), 1),
                    "diff": round(abs(float(hr_ppg) - float(hr_ecg)), 1),
                })
        start += window_s
    passed = bool(rows) and max(r["diff"] for r in rows) <= limit_bpm
    return rows, passed


def run_from_bidmc(record: str, window_s: float, limit_bpm: float) -> int:
    """用 PhysioNet BIDMC 的配对 PPG/ECG 自测本工具（同一受试者、同一时刻）。"""
    from validate_bidmc import load_record

    ppg, ecg, fs = load_record(record)
    t = np.arange(len(ppg)) / fs
    rows, passed = compare(t, ppg, fs, t, ecg, fs, window_s=window_s, limit_bpm=limit_bpm)
    print(f"（自测数据源：BIDMC {record}，同一受试者同步采集的 PLETH 与 II，{fs:.0f} Hz）")
    print_rows(rows)
    return 0 if passed else 1


def print_rows(rows: list[dict]) -> None:
    if not rows:
        print("没有可用的对照窗口（数据太短或两个通道没有重叠时间段）")
        return
    print(f"{'起点(s)':>8} {'PPG(bpm)':>9} {'ECG(bpm)':>9} {'偏差':>6}")
    for r in rows:
        print(f"{r['start_s']:>8.1f} {r['hr_ppg']:>9.1f} {r['hr_ecg']:>9.1f} {r['diff']:>6.1f}")
    diffs = [r["diff"] for r in rows]
    print(f"\n窗口数 {len(rows)}  平均偏差 {np.mean(diffs):.2f} bpm  最大偏差 {max(diffs):.1f} bpm")


def main() -> int:
    ap = argparse.ArgumentParser(description="PPG / ECG 双通道心率一致性验收")
    ap.add_argument("--ppg", help="PPG 通道 CSV（time_s + ppg_ir）")
    ap.add_argument("--ecg", help="ECG 通道 CSV（time_s + ecg）")
    ap.add_argument("--ppg-col", default="ppg_ir")
    ap.add_argument("--ecg-col", default="ecg")
    ap.add_argument("--ppg-fs", type=float, default=0.0, help="不填则从时间戳推算")
    ap.add_argument("--ecg-fs", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=30.0)
    ap.add_argument("--limit", type=float, default=5.0, help="验收上限（bpm）")
    ap.add_argument("--out", default=None, help="把逐窗口结果写成 CSV")
    ap.add_argument("--from-bidmc", default=None, help="用公开配对数据自测，如 bidmc01")
    args = ap.parse_args()

    if args.from_bidmc:
        return run_from_bidmc(args.from_bidmc, args.window, args.limit)

    if not (args.ppg and args.ecg):
        ap.error("需要 --ppg 与 --ecg（或用 --from-bidmc 自测）")

    ppg_t, ppg_v, fs_ppg = read_csv_column(args.ppg, args.ppg_col)
    ecg_t, ecg_v, fs_ecg = read_csv_column(args.ecg, args.ecg_col)
    fs_ppg = args.ppg_fs or fs_ppg
    fs_ecg = args.ecg_fs or fs_ecg
    print(f"PPG: {args.ppg}  {len(ppg_v)} 样本 / {fs_ppg:.1f} Hz    "
          f"ECG: {args.ecg}  {len(ecg_v)} 样本 / {fs_ecg:.1f} Hz")

    rows, passed = compare(ppg_t, ppg_v, fs_ppg, ecg_t, ecg_v, fs_ecg,
                           window_s=args.window, limit_bpm=args.limit)
    print_rows(rows)
    print(f"\n验收上限 {args.limit:.0f} bpm → {'PASS' if passed else 'FAIL'}")

    if args.out and rows:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"逐窗口结果已写入 {args.out}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
