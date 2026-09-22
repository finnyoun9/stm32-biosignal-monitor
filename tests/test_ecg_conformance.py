#!/usr/bin/env python3
"""C / Python ECG QRS 检测一致性测试。

为什么做：P3（AD8232 ECG 通道）上机后跑的是 C 实现，而 P0 验证过的结论来自 Python 实现。
两侧差异比 PPG 更大：**C 侧为实时性只做因果滤波，且暂不实现 RR 回检**（Python 有回检）。
所以这里不仅比对数字，还要把「差在哪、为什么」量化出来。

跑法：
    ./.venv/bin/python tests/test_ecg_conformance.py
"""

from __future__ import annotations

import csv
import os
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from tools import synth  # noqa: E402
from ecg_qrs import ecg_qrs_detect, hr_from_peaks  # noqa: E402
from validate_bidmc import load_record  # noqa: E402

EXE = "/tmp/biosignal_ecg_algo_host"


def build() -> None:
    cmd = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "firmware", "app"),
           "-I", os.path.join(ROOT, "tests"),
           os.path.join(ROOT, "tests", "ecg_algo_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "ecg_algo.c"),
           os.path.join(ROOT, "firmware", "app", "biquad.c"),
           "-lm", "-o", EXE]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit("C 侧编译失败：\n" + res.stderr)


def run_c(csv_path: str, column: str, fs: float) -> dict[str, float]:
    res = subprocess.run([EXE, csv_path, column, str(fs)], capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit(f"C 侧运行失败({res.returncode})：{res.stderr}")
    for line in res.stdout.splitlines():
        if line.startswith("RESULT"):
            out: dict[str, float] = {}
            for tok in line.split()[1:]:
                k, _, v = tok.partition("=")
                out[k] = float(v)
            return out
    raise SystemExit("C 侧输出无法解析：" + res.stdout)


def write_csv(path: str, fs: float, samples: np.ndarray, column: str = "ecg") -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", column])
        for i, v in enumerate(samples):
            w.writerow([f"{i / fs:.6f}", f"{v:.6f}"])


def compare(name: str, samples: np.ndarray, fs: float, tol: float, tmp_csv: str,
            column: str = "ecg") -> bool:
    write_csv(tmp_csv, fs, samples, column)
    c = run_c(tmp_csv, column, fs)
    peaks = ecg_qrs_detect(samples, fs)
    py_hr = hr_from_peaks(peaks, fs)
    diff = abs(c["hr"] - py_hr)
    ok = diff <= tol
    print(f"{'PASS' if ok else 'FAIL'}  {name}: C={c['hr']:.1f} bpm  Python={py_hr:.1f} bpm  "
          f"偏差={diff:.1f} (容差 {tol:.0f})  C检出 {int(c['beats'])} 搏 / Python {len(peaks)} 个 R 波")
    return ok


def main() -> int:
    build()
    fails = 0
    tmp = "/tmp/biosignal_ecg_conformance.csv"

    print("== 合成 ECG（真值已知，容差 3 bpm）==")
    for truth in (60.0, 75.0, 100.0):
        sig = synth.synth_ecg(125.0, 30.0, truth, noise=0.01, seed=int(truth))
        if not compare(f"合成 {truth:.0f} bpm", sig, 125.0, 3.0, tmp):
            fails += 1

    print("== 真实 ICU 数据（BIDMC 的 II 导联，容差 8 bpm）==")
    print("   说明：C 侧无 RR 回检 + 因果滤波，遇到伪影/异位搏动时可能漏检，")
    print("   因此容差比 PPG 宽；这里要的是「同一量级」，不是逐搏一致。")
    record = os.path.join(ROOT, "data", "raw", "bidmc01")
    if not (os.path.exists(record + ".hea") and os.path.exists(record + ".dat")):
        print("   跳过：data/raw/bidmc01 不存在（下载方法见 docs/05）")
    else:
        _ppg, ecg, fs = load_record("bidmc01")
        win = int(30 * fs)
        for start_s in (60, 180, 300, 420):
            # 给 C 侧 5 s 预热，模拟连续采集（冷启动会丢滤波器瞬态）
            seg = ecg[max(0, int(start_s * fs) - int(5 * fs)): int(start_s * fs) + win]
            if not compare(f"BIDMC {start_s}s 窗口（含 5s 预热）", seg, fs, 8.0, tmp):
                fails += 1

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
