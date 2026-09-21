#!/usr/bin/env python3
"""合成 PPG / ECG 信号生成器（P0 算法验证用，无需硬件）。

为什么需要它：算法在真机上出问题很难判断是「算法错」还是「采集链路错」。
先用**已知心率真值**的合成信号把算法本身的正确性钉死，上机后才有对照基准。

信号模型（都是公开教科书式的近似，不是生理模型）：
- ECG：每个心搏由 P、QRS（Q/R/S 三个高斯）、T 波叠加而成，加基线漂移与白噪声
- PPG：每个心搏由「收缩期主峰 + 舒张期切迹次峰」两个高斯叠加，加呼吸引起的基线起伏与噪声
- RED/IR 双通道：IR 幅度归一化为 1，RED 的 AC/DC 比例按目标 R 值构造（用于 SpO2 方法验证）

用法：
    python tools/synth.py --hr 72 --seconds 60 --out data/synth_hr72.csv
    python tools/synth.py --hr 90 --fs 125 --noise 0.02 --out data/synth_hr90.csv
"""

from __future__ import annotations

import argparse
import csv
import math
import os

import numpy as np


def _gauss(t: np.ndarray, center: float, width: float) -> np.ndarray:
    return np.exp(-0.5 * ((t - center) / width) ** 2)


def synth_ecg(fs: float, seconds: float, hr_bpm: float, noise: float = 0.01,
              wander: float = 0.05, seed: int = 7) -> np.ndarray:
    """生成单导联 ECG（近似形态）。返回幅度约 ±1 的数组。"""
    rng = np.random.default_rng(seed)
    n = int(round(fs * seconds))
    t = np.arange(n) / fs
    period = 60.0 / hr_bpm
    sig = np.zeros(n)

    beat = 0
    while beat * period < seconds:
        t0 = beat * period
        # P 波、Q、R、S、T（相对 R 峰的时间偏移，单位秒）
        sig += 0.12 * _gauss(t, t0 - 0.16, 0.025)     # P
        sig += -0.10 * _gauss(t, t0 - 0.022, 0.008)   # Q
        sig += 1.00 * _gauss(t, t0, 0.009)            # R
        sig += -0.22 * _gauss(t, t0 + 0.022, 0.010)   # S
        sig += 0.28 * _gauss(t, t0 + 0.20, 0.045)     # T
        beat += 1

    sig += wander * np.sin(2 * math.pi * 0.25 * t)          # 基线漂移 0.25 Hz
    sig += noise * rng.standard_normal(n)                    # 白噪声
    return sig


def synth_ppg(fs: float, seconds: float, hr_bpm: float, noise: float = 0.004,
              respiration: float = 0.03, seed: int = 11) -> np.ndarray:
    """生成 PPG（IR 通道，含 DC 分量）。返回带 DC 的原始值，单位近似「传感器计数」。"""
    rng = np.random.default_rng(seed)
    n = int(round(fs * seconds))
    t = np.arange(n) / fs
    period = 60.0 / hr_bpm

    ac = np.zeros(n)
    beat = 0
    while beat * period < seconds:
        t0 = beat * period
        ac += 1.00 * _gauss(t, t0, 0.055)              # 收缩期主峰
        ac += 0.32 * _gauss(t, t0 + 0.22, 0.070)       # 舒张期切迹
        beat += 1

    ac += respiration * np.sin(2 * math.pi * 0.3 * t)  # 呼吸调制
    ac += noise * rng.standard_normal(n)

    dc = 100000.0                                      # 传感器直流分量（大）
    return dc + 3000.0 * ac


def synth_ppg_pair(fs: float, seconds: float, hr_bpm: float, target_r: float = 0.6,
                   noise: float = 0.004, seed: int = 13) -> tuple[np.ndarray, np.ndarray]:
    """生成 IR / RED 双通道，使 ratio-of-ratios R ≈ target_r。

    R = (AC_red/DC_red) / (AC_ir/DC_ir)；这里固定 IR 的 AC/DC，再按 target_r 反推 RED 的 AC。
    """
    ir = synth_ppg(fs, seconds, hr_bpm, noise=noise, seed=seed)
    dc_ir = float(np.mean(ir))
    ac_ir = float(np.percentile(ir, 99) - np.percentile(ir, 1))
    ac_red_ratio = target_r * (ac_ir / dc_ir)          # 需要的 AC_red/DC_red
    red = synth_ppg(fs, seconds, hr_bpm, noise=noise, seed=seed + 1)
    dc_red = float(np.mean(red))
    red_ac = (red - dc_red) / max(ac_ir, 1e-9) * (ac_red_ratio * dc_red)
    return ir, dc_red + red_ac


def write_csv(path: str, fs: float, ecg: np.ndarray, ir: np.ndarray, red: np.ndarray) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ecg", "ppg_ir", "ppg_red"])
        for i in range(len(ecg)):
            w.writerow([f"{i / fs:.6f}", f"{ecg[i]:.6f}", f"{ir[i]:.3f}", f"{red[i]:.3f}"])


def main() -> None:
    ap = argparse.ArgumentParser(description="合成 PPG/ECG 信号")
    ap.add_argument("--hr", type=float, default=72.0, help="目标心率 bpm")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--fs", type=float, default=125.0, help="采样率 Hz（默认与 BIDMC 一致）")
    ap.add_argument("--noise", type=float, default=0.004, help="PPG 白噪声标准差")
    ap.add_argument("--target-r", type=float, default=0.6, help="SpO2 验证用的目标 R 值")
    ap.add_argument("--out", default="data/synth.csv")
    args = ap.parse_args()

    ecg = synth_ecg(args.fs, args.seconds, args.hr, noise=max(args.noise, 0.01))
    ir, red = synth_ppg_pair(args.fs, args.seconds, args.hr, target_r=args.target_r, noise=args.noise)
    write_csv(args.out, args.fs, ecg, ir, red)
    print(f"wrote {args.out}: fs={args.fs} Hz, {args.seconds}s, 真值 HR={args.hr} bpm, "
          f"目标 R={args.target_r}")


if __name__ == "__main__":
    main()
