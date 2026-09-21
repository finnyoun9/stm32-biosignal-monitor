#!/usr/bin/env python3
"""PPG 心率 / HRV 估计与信号质量评估（P0，纯算法，不依赖硬件）。

链路（每一步都能在面试里讲清为什么）：
    原始 PPG(含 DC) → 去 DC + 带通(0.5–4 Hz) → 自适应阈值峰值检测
    → RR 间期 → 中位数出心率（抗单次误检）+ HRV 指标 + 信号质量指标

关键取舍：
- 用**中位数**而不是均值算心率：运动伪影会造成个别漏检/多检，中位数对离群值不敏感。
- 峰值检测的阈值取「滑动窗口内 中位数 + k·MAD」而不是固定高度：PPG 幅度随佩戴压力、肤色、
  灌注情况变化很大，固定阈值在真机上必然失效（这是 MAXREFDES117/MAX30102_by_RF 的共同做法）。
- 信号质量用两个指标：灌注指数 PI（AC/DC，反映信号强度）与逐搏模板相关性（反映形态一致性）。

用法：
    python tools/ppg_hr.py data/synth_hr72.csv
    python tools/ppg_hr.py capture.csv --col ppg_ir --fs 100 --window 10
"""

from __future__ import annotations

import argparse
import csv
import math

import numpy as np
from scipy.signal import butter, filtfilt, find_peaks


def bandpass(x: np.ndarray, fs: float, low: float = 0.5, high: float = 4.0,
             order: int = 3) -> np.ndarray:
    """零相位带通滤波。上限默认 4 Hz（240 bpm），下限 0.5 Hz 去掉呼吸与基线漂移。"""
    nyq = fs / 2.0
    high = min(high, nyq * 0.95)
    b, a = butter(order, [low / nyq, high / nyq], btype="band")
    return filtfilt(b, a, x)


def perfusion_index(raw: np.ndarray) -> float:
    """灌注指数 PI = AC/DC（%）。真机上这是判断「手指有没有放好」的第一指标。"""
    dc = float(np.mean(raw))
    ac = float(np.percentile(raw, 99) - np.percentile(raw, 1))
    if dc == 0:
        return 0.0
    return 100.0 * ac / abs(dc)


def detect_ppg_peaks(ac: np.ndarray, fs: float, k: float = 0.6,
                     window_s: float = 5.0, min_bpm: float = 30.0,
                     max_bpm: float = 220.0) -> np.ndarray:
    """滑动窗口自适应阈值峰值检测。

    k 越大越保守（漏检换误检）；min_distance 由最大心率推出，避免把舒张期切迹当峰。
    """
    min_distance = max(1, int(fs * 60.0 / max_bpm))
    win = max(3, int(fs * window_s))
    half = win // 2
    peaks: list[int] = []
    for i in range(0, len(ac), half):
        seg = ac[i:i + win]
        if len(seg) < min_distance * 2:
            continue
        med = float(np.median(seg))
        mad = float(np.median(np.abs(seg - med))) + 1e-9
        thr = med + k * 1.4826 * mad
        idx, _ = find_peaks(seg, height=thr, distance=min_distance)
        peaks.extend(int(i + j) for j in idx)
    if not peaks:
        return np.array([], dtype=int)
    # 合并重叠窗口重复检出的峰
    peaks = sorted(set(peaks))
    merged: list[int] = []
    for p in peaks:
        if merged and p - merged[-1] < min_distance:
            if ac[p] > ac[merged[-1]]:
                merged[-1] = p
            continue
        merged.append(p)
    return np.array(merged, dtype=int)


def template_correlation(ac: np.ndarray, peaks: np.ndarray, fs: float,
                         pre_s: float = 0.25, post_s: float = 0.4) -> float:
    """逐搏模板相关性：所有心搏与平均模板的相关系数均值，反映形态一致性（运动伪影会拉低）。"""
    pre, post = int(fs * pre_s), int(fs * post_s)
    beats = [ac[p - pre:p + post] for p in peaks if p - pre >= 0 and p + post < len(ac)]
    if len(beats) < 3:
        return float("nan")
    m = min(len(b) for b in beats)
    mat = np.vstack([b[:m] for b in beats])
    tpl = mat.mean(axis=0)
    tpl_n = tpl - tpl.mean()
    corrs = []
    for b in mat:
        bn = b - b.mean()
        denom = float(np.linalg.norm(bn) * np.linalg.norm(tpl_n))
        corrs.append(float(bn @ tpl_n / denom) if denom > 1e-9 else 0.0)
    return float(np.mean(corrs))


def rr_metrics(peaks: np.ndarray, fs: float) -> dict:
    """RR 间期与 HRV（SDNN/RMSSD）以及心率（中位数）。"""
    if len(peaks) < 3:
        return {"hr_bpm": float("nan"), "rr_ms": np.array([]), "sdnn_ms": float("nan"),
                "rmssd_ms": float("nan"), "n_beats": int(len(peaks))}
    rr = np.diff(peaks) / fs * 1000.0
    # 生理范围内的 RR 才参与统计（0.27–2.0 s 对应 30–220 bpm）
    valid = rr[(rr > 270) & (rr < 2000)]
    if len(valid) < 2:
        return {"hr_bpm": float("nan"), "rr_ms": rr, "sdnn_ms": float("nan"),
                "rmssd_ms": float("nan"), "n_beats": int(len(peaks))}
    hr = 60000.0 / float(np.median(valid))
    return {
        "hr_bpm": hr,
        "rr_ms": valid,
        "sdnn_ms": float(np.std(valid, ddof=1)),
        "rmssd_ms": float(np.sqrt(np.mean(np.diff(valid) ** 2))) if len(valid) > 2 else float("nan"),
        "n_beats": int(len(peaks)),
    }


def analyse(raw: np.ndarray, fs: float, low: float = 0.5, high: float = 4.0,
            k: float = 0.6) -> dict:
    """完整链路：原始 PPG → 心率/HRV/质量指标。"""
    ac = bandpass(np.asarray(raw, dtype=float), fs, low=low, high=high)
    peaks = detect_ppg_peaks(ac, fs, k=k)
    out = rr_metrics(peaks, fs)
    out.update({
        "peaks": peaks,
        "ac": ac,
        "pi_percent": perfusion_index(np.asarray(raw, dtype=float)),
        "template_corr": template_correlation(ac, peaks, fs),
        "fs": fs,
    })
    # 简易可信度：PI 太低或模板相关性太差时，心率不该被采信
    out["quality_ok"] = bool(
        out["pi_percent"] > 0.05
        and (math.isnan(out["template_corr"]) or out["template_corr"] > 0.6)
        and out["n_beats"] >= 3
    )
    return out


def read_csv(path: str, col: str) -> tuple[np.ndarray, float]:
    """读取采集/合成 CSV，返回 (信号列, 采样率)。"""
    times: list[float] = []
    vals: list[float] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if col not in reader.fieldnames:
            raise SystemExit(f"列 {col!r} 不存在，可用列：{reader.fieldnames}")
        for row in reader:
            if row.get("time_s"):
                times.append(float(row["time_s"]))
            vals.append(float(row[col]))
    if len(times) > 2:
        dt = float(np.median(np.diff(times)))
        fs = 1.0 / dt if dt > 0 else 125.0
    else:
        fs = 125.0
    return np.asarray(vals, dtype=float), fs


def main() -> None:
    ap = argparse.ArgumentParser(description="PPG 心率/HRV/信号质量")
    ap.add_argument("csv", help="CSV 文件（含 time_s 与信号列）")
    ap.add_argument("--col", default="ppg_ir")
    ap.add_argument("--fs", type=float, default=0.0, help="不填则从 time_s 推算")
    ap.add_argument("--k", type=float, default=0.6, help="峰值阈值系数（越大越保守）")
    args = ap.parse_args()

    raw, fs = read_csv(args.csv, args.col)
    fs = args.fs or fs
    res = analyse(raw, fs, k=args.k)
    print(f"文件: {args.csv}  列: {args.col}  fs={fs:.2f} Hz  样本={len(raw)}")
    print(f"心率(中位数): {res['hr_bpm']:.1f} bpm   心搏数: {res['n_beats']}")
    print(f"HRV: SDNN={res['sdnn_ms']:.1f} ms  RMSSD={res['rmssd_ms']:.1f} ms")
    print(f"信号质量: PI={res['pi_percent']:.3f}%  模板相关性={res['template_corr']:.3f}  "
          f"可信={res['quality_ok']}")


if __name__ == "__main__":
    main()
