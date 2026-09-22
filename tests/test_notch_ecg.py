#!/usr/bin/env python3
"""工频陷波在 ECG 上的**实际收益**验证：强干扰下陷波是从「测不出来」到「测得准」。

设计这个测试的动机：我在 docs/03 里写过"只跑 QRS 检测时，5–15 Hz 带通本身在 50 Hz 已有 20 dB 以上衰减，
陷波增益有限"。这句话只对**轻度干扰**成立。于是我把干扰强度扫了一遍，用数据说话：

    50 Hz 干扰幅度（相对 R 波 1.0）   无陷波      开陷波
    0.5                              75.0 bpm    75.0 bpm      ← 带通够用，陷波无差别
    2.0                              检出失败     75.0 bpm      ← 带通不够，陷波救回来
    5.0                              检出失败     75.0 bpm

结论：陷波不是"为了显得专业"加的，而是在**工频干扰幅度接近或超过 QRS 时**决定能不能出结果。

跑法：
    ./.venv/bin/python tests/test_notch_ecg.py
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

EXE = "/tmp/biosignal_ecg_algo_host"
FS = 250.0
TRUTH_BPM = 75.0
AMPLITUDES = (0.5, 2.0, 5.0)


def build() -> None:
    cmd = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "firmware", "app"),
           "-I", os.path.join(ROOT, "tests"),
           os.path.join(ROOT, "tests", "ecg_algo_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "ecg_algo.c"),
           os.path.join(ROOT, "firmware", "app", "biquad.c"),
           os.path.join(ROOT, "firmware", "app", "notch.c"),
           "-lm", "-o", EXE]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit("C 侧编译失败：\n" + res.stderr)


def write_csv(path: str, values: np.ndarray) -> None:
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ecg"])
        for i, v in enumerate(values):
            w.writerow([f"{i / FS:.6f}", f"{v:.6f}"])


def run_c(path: str, mains: float | None) -> float:
    args = [EXE, path, "ecg", str(FS)] + ([str(mains)] if mains else [])
    res = subprocess.run(args, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit(f"C 侧运行失败：{res.stderr}")
    for line in res.stdout.splitlines():
        if line.startswith("RESULT"):
            return float(line.split("hr=")[1].split()[0])
    raise SystemExit("C 侧输出无法解析：" + res.stdout)


def main() -> int:
    build()
    os.makedirs("/tmp/notch_ecg", exist_ok=True)
    clean_path = "/tmp/notch_ecg/clean.csv"
    noisy_path = "/tmp/notch_ecg/noisy.csv"

    clean = synth.synth_ecg(FS, 40.0, TRUTH_BPM, noise=0.01, seed=11)
    write_csv(clean_path, clean)
    hr_clean = run_c(clean_path, None)
    print(f"基准（无干扰）：{hr_clean:.1f} bpm（合成真值 {TRUTH_BPM:.0f}）")

    fails = 0
    print(f"\n{'干扰幅度':>8} {'无陷波':>10} {'开陷波':>10}   判定")
    t = np.arange(len(clean)) / FS
    for amp in AMPLITUDES:
        noisy = clean + amp * np.sin(2 * np.pi * 50.0 * t)
        write_csv(noisy_path, noisy)
        hr_off = run_c(noisy_path, None)
        hr_on = run_c(noisy_path, 50.0)
        err_off = abs(hr_off - hr_clean)
        err_on = abs(hr_on - hr_clean)
        # 判据：开陷波后必须接近基准；强干扰时无陷波应明显更差（证明陷波有实际价值）
        ok_on = err_on <= 3.0
        if amp >= 2.0:
            ok = ok_on and err_off > err_on
            note = "陷波救回结果" if ok else "未体现收益或陷波失效"
        else:
            ok = ok_on
            note = "带通已够用（两者无差别属正常）"
        print(f"{amp:>8.1f} {hr_off:>10.1f} {hr_on:>10.1f}   {'PASS' if ok else 'FAIL'}  {note}")
        fails += 0 if ok else 1

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
