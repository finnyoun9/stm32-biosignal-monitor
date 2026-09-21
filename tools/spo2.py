#!/usr/bin/env python3
"""SpO2（血氧）估计：ratio-of-ratios 方法（P0 方法验证，**未标定**）。

原理（Beer–Lambert 近似下的经典做法）：
    每个波长通道都分解成 DC（组织/静脉/骨头）与 AC（搏动性动脉血）两部分，
    定义 R = (AC_red / DC_red) / (AC_ir / DC_ir)，再用经验标定曲线把 R 映射到 SpO2。

⚠️ 边界（写进简历前必须理解）：
- 这里的标定系数来自**公开参考实现**（线性式与 Maxim 二次式），**本项目没有用血氧仪做标定**，
  所以输出只能证明「方法链路可复现」，不能声称是准确的血氧值。
- 真机上还要处理：LED 电流/增益自动调节、环境光扣除、运动伪影、探头压力导致的灌注下降。

用法：
    python tools/spo2.py data/synth_hr72.csv            # 合成数据：验证 R 与期望值一致
    python tools/spo2.py capture.csv --window 4 --fs 100
"""

from __future__ import annotations

import argparse
import csv

import numpy as np
from scipy.signal import butter, filtfilt


def _ac_dc(raw: np.ndarray, fs: float, low: float = 0.5, high: float = 4.0) -> tuple[float, float]:
    """返回 (AC 峰峰值, DC 均值)。AC 在带通信号上量取，避免基线漂移污染。"""
    nyq = fs / 2.0
    high = min(high, nyq * 0.95)
    b, a = butter(3, [low / nyq, high / nyq], btype="band")
    ac = filtfilt(b, a, np.asarray(raw, dtype=float))
    ac_pp = float(np.percentile(ac, 99) - np.percentile(ac, 1))
    dc = float(np.mean(raw))
    return ac_pp, dc


def ratio_of_ratios(ir: np.ndarray, red: np.ndarray, fs: float,
                    window_s: float = 4.0, step_s: float | None = None
                    ) -> tuple[float, np.ndarray]:
    """逐窗计算 R 值，返回 (R 中位数, 每窗 R 序列)。窗口 4 s 是精度与响应速度的折中。"""
    ir = np.asarray(ir, dtype=float)
    red = np.asarray(red, dtype=float)
    win = max(8, int(window_s * fs))
    step = int((step_s or window_s / 2.0) * fs)
    rs: list[float] = []
    for start in range(0, max(1, len(ir) - win + 1), max(1, step)):
        seg_ir, seg_red = ir[start:start + win], red[start:start + win]
        if len(seg_ir) < win:
            break
        ac_ir, dc_ir = _ac_dc(seg_ir, fs)
        ac_red, dc_red = _ac_dc(seg_red, fs)
        if dc_ir <= 0 or dc_red <= 0 or ac_ir <= 0:
            continue
        rs.append((ac_red / dc_red) / (ac_ir / dc_ir))
    arr = np.asarray(rs, dtype=float)
    return (float(np.median(arr)) if len(arr) else float("nan")), arr


def spo2_from_r_linear(r: float) -> float:
    """线性标定式 SpO2 = 110 - 25R（公开参考实现常用，未标定）。"""
    return 110.0 - 25.0 * r


def spo2_from_r_maxim(r: float) -> float:
    """Maxim 参考设计的二次标定式（公开参考实现，未标定）。"""
    return -45.060 * r * r + 30.354 * r + 94.845


def read_pair(path: str, col_ir: str, col_red: str) -> tuple[np.ndarray, np.ndarray, float]:
    ir: list[float] = []
    red: list[float] = []
    times: list[float] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            ir.append(float(row[col_ir]))
            red.append(float(row[col_red]))
            if row.get("time_s"):
                times.append(float(row["time_s"]))
    fs = 125.0
    if len(times) > 2:
        dt = float(np.median(np.diff(times)))
        fs = 1.0 / dt if dt > 0 else 125.0
    return np.asarray(ir), np.asarray(red), fs


def main() -> None:
    ap = argparse.ArgumentParser(description="SpO2 ratio-of-ratios（未标定）")
    ap.add_argument("csv")
    ap.add_argument("--col-ir", default="ppg_ir")
    ap.add_argument("--col-red", default="ppg_red")
    ap.add_argument("--fs", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=4.0)
    ap.add_argument("--expect-r", type=float, default=None, help="合成数据的期望 R 值")
    args = ap.parse_args()

    ir, red, fs_auto = read_pair(args.csv, args.col_ir, args.col_red)
    fs = args.fs or fs_auto
    r, arr = ratio_of_ratios(ir, red, fs, window_s=args.window)
    print(f"文件: {args.csv}  fs={fs:.2f} Hz  窗口={args.window}s  有效窗={len(arr)}")
    print(f"R(中位数) = {r:.4f}   波动(std) = {np.std(arr):.4f}" if len(arr) else "R 无法计算")
    print(f"SpO2 线性式 = {spo2_from_r_linear(r):.1f}%   二次式 = {spo2_from_r_maxim(r):.1f}%   （未标定）")
    if args.expect_r is not None:
        print(f"期望 R = {args.expect_r:.4f}   误差 = {r - args.expect_r:+.4f}")


if __name__ == "__main__":
    main()
