#!/usr/bin/env python3
"""ECG QRS 检测（Pan–Tompkins 简化版，P0 算法原型）。

为什么写它：
1. 验证 PPG 心率估计需要「真值」——同一段记录里的 ECG 就是最好的真值来源（BIDMC 数据集）。
2. 可穿戴/医疗岗位常问「你怎么判断心率算得对不对」，这套链路本身就是答案。

Pan–Tompkins 的五个步骤（1985 论文）：
    带通(5–15 Hz) → 五点差分 → 平方 → 滑动窗积分(150 ms) → 自适应双阈值 + 不应期 + 回检

简化之处（文档里如实标注）：
- 未实现完整的两级阈值自动调整与基于斜率/宽度的波形判别（真机上再补）
- 回检（search-back）用固定系数的一次回检，不是论文里的迭代方案

用法：
    python tools/ecg_qrs.py data/synth_hr72.csv --col ecg
    python tools/ecg_qrs.py data/synth_hr72.csv --col ecg --truth-hr 72
"""

from __future__ import annotations

import argparse
import csv

import numpy as np
from scipy.signal import butter, filtfilt, find_peaks


def ecg_qrs_detect(x: np.ndarray, fs: float, low: float = 5.0, high: float = 15.0,
                   refractory_ms: float = 200.0) -> np.ndarray:
    """返回 R 波在原始序列中的索引。"""
    x = np.asarray(x, dtype=float)
    nyq = fs / 2.0
    high = min(high, nyq * 0.95)
    b, a = butter(2, [low / nyq, high / nyq], btype="band")
    bp = filtfilt(b, a, x)

    deriv = np.gradient(bp) * fs                      # 差分（等价五点差分，采样率无关写法）
    squared = deriv ** 2
    win = max(3, int(0.150 * fs))
    integrated = np.convolve(squared, np.ones(win) / win, mode="same")

    refractory = max(1, int(refractory_ms / 1000.0 * fs))
    # 初值：用前 2 秒估计信号峰与噪声峰
    head = integrated[: max(win, int(2 * fs))]
    spki = float(np.max(head)) if len(head) else float(np.max(integrated))
    npki = float(np.mean(head)) if len(head) else float(np.mean(integrated))

    # ⚠️ 真实数据踩过的坑（bidmc03，2026-09-21）：
    # 初版只更新 SPKI、NPKI 固定为前 2 秒的均值 → 当记录开头恰好安静、后面噪声抬高时，
    # 阈值长期偏低，积分信号一直高于阈值，于是**每过一个不应期就误报一个峰**（间隔恒为 200 ms），
    # 心率被抬到 107 bpm（真值 ~76）。正确做法是论文里的做法：**SPKI 与 NPKI 都随局部极大值更新**。
    candidates, _ = find_peaks(integrated, distance=1)
    accepted: list[int] = []
    last = -refractory * 2
    for idx in candidates:
        value = float(integrated[idx])
        threshold = npki + 0.25 * (spki - npki)
        if value > threshold:
            spki = 0.125 * value + 0.875 * spki
            if idx - last >= refractory:
                accepted.append(int(idx))
                last = int(idx)
            elif accepted and value > float(integrated[accepted[-1]]):
                accepted[-1] = int(idx)      # 不应期内保留更高者
                last = int(idx)
        else:
            npki = 0.125 * value + 0.875 * npki
    peaks = accepted
    if not peaks:
        return np.array([], dtype=int)

    # 回检：两次心搏间隔 > 1.66×RR 均值时，在中间用更低阈值找一次
    rr = np.diff(peaks)
    if len(rr) >= 2:
        rr_med = float(np.median(rr))
        extra: list[int] = []
        for k in range(len(peaks) - 1):
            gap = peaks[k + 1] - peaks[k]
            if gap > 1.66 * rr_med:
                lo, hi = peaks[k] + refractory, peaks[k + 1] - refractory
                if hi > lo:
                    seg = integrated[lo:hi]
                    thr = npki + 0.5 * (spki - npki)
                    cand, _ = find_peaks(seg, height=thr)
                    if len(cand):
                        extra.append(lo + int(cand[np.argmax(seg[cand])]))
        if extra:
            peaks = sorted(peaks + extra)

    # 把积分域的峰位置细化回带通信号上的 R 峰
    refined: list[int] = []
    half = max(1, int(0.05 * fs))
    for p in peaks:
        lo, hi = max(0, p - half), min(len(bp), p + half + 1)
        refined.append(lo + int(np.argmax(np.abs(bp[lo:hi]))))
    return np.array(sorted(set(refined)), dtype=int)


def hr_from_peaks(peaks: np.ndarray, fs: float, min_bpm: float = 30.0,
                  max_bpm: float = 220.0) -> float:
    if len(peaks) < 2:
        return float("nan")
    rr = np.diff(peaks) / fs * 1000.0
    valid = rr[(rr > 60000.0 / max_bpm) & (rr < 60000.0 / min_bpm)]
    if len(valid) < 1:
        return float("nan")
    return 60000.0 / float(np.median(valid))


def sensitivity(reference: np.ndarray, detected: np.ndarray, fs: float,
                tol_ms: float = 150.0) -> float:
    """检测灵敏度：真值 R 波中被检出的比例（容差默认 150 ms）。"""
    if len(reference) == 0:
        return float("nan")
    tol = int(tol_ms / 1000.0 * fs)
    hits = 0
    for r in reference:
        if len(detected) and np.min(np.abs(detected - r)) <= tol:
            hits += 1
    return hits / len(reference)


def read_csv_col(path: str, col: str) -> np.ndarray:
    vals: list[float] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if col not in reader.fieldnames:
            raise SystemExit(f"列 {col!r} 不存在，可用列：{reader.fieldnames}")
        for row in reader:
            vals.append(float(row[col]))
    return np.asarray(vals, dtype=float)


def main() -> None:
    ap = argparse.ArgumentParser(description="ECG QRS 检测（Pan–Tompkins 简化版）")
    ap.add_argument("csv")
    ap.add_argument("--col", default="ecg")
    ap.add_argument("--fs", type=float, default=125.0)
    ap.add_argument("--truth-hr", type=float, default=None, help="已知真值心率，用于对照")
    args = ap.parse_args()

    x = read_csv_col(args.csv, args.col)
    peaks = ecg_qrs_detect(x, args.fs)
    hr = hr_from_peaks(peaks, args.fs)
    print(f"文件: {args.csv}  列: {args.col}  fs={args.fs} Hz")
    print(f"检出 R 波: {len(peaks)} 个    心率(中位数): {hr:.1f} bpm")
    if args.truth_hr:
        print(f"真值 {args.truth_hr:.1f} bpm    误差 {hr - args.truth_hr:+.1f} bpm")


if __name__ == "__main__":
    main()
