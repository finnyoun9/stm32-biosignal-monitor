#!/usr/bin/env python3
"""C / Python PPG 心率算法一致性测试。

为什么必须做这一步：
    P1 上机时跑的是 **C 实现**（固件），而 P0 验证过的结论来自 **Python 实现**。
    如果两者不一致，那么「BIDMC 上 MAE 0.96 bpm」这个结论对固件毫无意义。
    所以这里用同一批数据分别跑两边，把偏差量化出来，并明确容差与原因。

两侧的**已知差异**（不是 bug，是约束）：
    - Python 用 filtfilt（零相位，离线）；C 必须因果（只能看过去），会引入固定群延迟；
    - Python 的质量指标用逐搏模板相关性；C 用 RR 稳定性 + PI（MCU 上算相关性代价高）。
    因此容差取 3 bpm（合成信号）/ 6 bpm（真实 ICU 数据，波形更脏）。

跑法：
    ./.venv/bin/python tests/test_algo_conformance.py
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

from tools import ppg_hr, synth  # noqa: E402
from validate_bidmc import load_record  # noqa: E402

EXE = "/tmp/biosignal_ppg_algo_host"


def build() -> None:
    cmd = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "firmware", "app"),
           os.path.join(ROOT, "tests", "ppg_algo_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "ppg_algo.c"),
           os.path.join(ROOT, "firmware", "app", "biquad.c"),
           "-lm", "-o", EXE]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit("C 侧编译失败：\n" + res.stderr)


def run_c(csv_path: str, column: str, fs: float) -> dict:
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


def write_csv(path: str, fs: float, samples: np.ndarray) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ppg_ir"])
        for i, v in enumerate(samples):
            w.writerow([f"{i / fs:.6f}", f"{v:.6f}"])


def compare(name: str, samples: np.ndarray, fs: float, tol: float, tmp_csv: str) -> bool:
    write_csv(tmp_csv, fs, samples)
    c = run_c(tmp_csv, "ppg_ir", fs)
    py = ppg_hr.analyse(samples, fs)
    diff = abs(c["hr"] - py["hr_bpm"])
    ok = diff <= tol
    print(f"{'PASS' if ok else 'FAIL'}  {name}: C={c['hr']:.1f} bpm  Python={py['hr_bpm']:.1f} bpm  "
          f"偏差={diff:.1f} (容差 {tol:.0f})  C质量={int(c['quality'])}")
    return ok


def main() -> int:
    build()
    fails = 0
    tmp = "/tmp/biosignal_conformance.csv"

    print("== 合成信号（真值已知，容差 3 bpm）==")
    for truth in (60.0, 72.0, 90.0, 120.0):
        sig = synth.synth_ppg(100.0, 60.0, truth, noise=0.004, seed=int(truth))
        if not compare(f"合成 {truth:.0f} bpm", sig, 100.0, 3.0, tmp):
            fails += 1

    print("\n== 真实 ICU 数据窗口（容差 6 bpm：因果滤波 vs 零相位 + 质量指标不同）==")
    record = os.path.join(ROOT, "data", "raw", "bidmc01")
    if not (os.path.exists(record + ".hea") and os.path.exists(record + ".dat")):
        print("跳过：data/raw/bidmc01 不存在（下载方法见 docs/05）")
    else:
        ppg, _ecg, fs = load_record("bidmc01")
        win = int(30 * fs)
        for start_s in (60, 180, 300, 420):
            seg = ppg[int(start_s * fs): int(start_s * fs) + win]
            # 给 C 侧 5 s 预热，模拟真实连续采集（冷启动会丢滤波器瞬态）
            warm = ppg[max(0, int(start_s * fs) - int(5 * fs)): int(start_s * fs) + win]
            if not compare(f"BIDMC {start_s}s 窗口（含 5s 预热）", warm, fs, 6.0, tmp):
                fails += 1

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
