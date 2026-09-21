#!/usr/bin/env python3
"""采集数据可视化：把 CSV 画成「PPG/ECG + 检测到的峰」图，作为调试与面试的波形证据。

用法：
    ./.venv/bin/python tools/plot_capture.py data/synth_hr72.csv --out data/synth_hr72.png
    ./.venv/bin/python tools/plot_capture.py capture.csv --col-ppg ppg_ir --col-ecg ecg
"""

from __future__ import annotations

import argparse
import os

import matplotlib
matplotlib.use("Agg")  # 无窗口环境（服务器/脚本）也能出图
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from ecg_qrs import ecg_qrs_detect  # noqa: E402
from ppg_hr import analyse as ppg_analyse, read_csv  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description="PPG/ECG 波形与峰检测绘图")
    ap.add_argument("csv")
    ap.add_argument("--col-ppg", default="ppg_ir")
    ap.add_argument("--col-ecg", default="ecg")
    ap.add_argument("--fs", type=float, default=0.0, help="不填则从 time_s 推算")
    ap.add_argument("--out", default="data/capture.png")
    ap.add_argument("--start", type=float, default=0.0, help="起始秒")
    ap.add_argument("--seconds", type=float, default=10.0, help="绘图时长")
    args = ap.parse_args()

    try:
        ppg, fs = read_csv(args.csv, args.col_ppg)
        fs = args.fs or fs
    except SystemExit as exc:
        raise SystemExit(f"读取失败：{exc}")

    lo, hi = int(args.start * fs), int((args.start + args.seconds) * fs)
    ppg_seg = ppg[lo:hi]

    res = ppg_analyse(ppg_seg, fs)
    peaks = res["peaks"]
    t = np.arange(len(ppg_seg)) / fs + args.start

    fig, axes = plt.subplots(3, 1, figsize=(11, 8), sharex=True)

    axes[0].plot(t, ppg_seg, lw=0.8, color="#1f77b4")
    if len(peaks):
        axes[0].plot(t[peaks], ppg_seg[peaks], "rv", ms=5, label="PPG peaks")
    axes[0].set_ylabel("PPG raw")
    axes[0].legend(loc="upper right", fontsize=8)
    axes[0].set_title(f"{os.path.basename(args.csv)}  HR≈{res['hr_bpm']:.1f} bpm  "
                      f"PI={res['pi_percent']:.2f}%  quality_ok={res['quality_ok']}")

    axes[1].plot(t, res["ac"], lw=0.8, color="#2ca02c")
    axes[1].set_ylabel("PPG band-passed")

    ecg_ok = True
    try:
        ecg, _ = read_csv(args.csv, args.col_ecg)
        ecg_seg = ecg[lo:hi]
        r_peaks = ecg_qrs_detect(ecg_seg, fs)
        axes[2].plot(t, ecg_seg, lw=0.8, color="#d62728")
        if len(r_peaks):
            axes[2].plot(t[r_peaks], ecg_seg[r_peaks], "k^", ms=5, label="R peaks")
            axes[2].legend(loc="upper right", fontsize=8)
        axes[2].set_ylabel("ECG")
    except SystemExit:
        ecg_ok = False
        axes[2].text(0.02, 0.5, f"no column {args.col_ecg}, ECG skipped", transform=axes[2].transAxes)

    axes[2].set_xlabel("time (s)")
    fig.tight_layout()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    fig.savefig(args.out, dpi=130)
    print(f"saved {args.out}" + ("" if ecg_ok else " (PPG channel only)"))


if __name__ == "__main__":
    main()
