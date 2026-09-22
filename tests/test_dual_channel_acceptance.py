#!/usr/bin/env python3
"""P3 双通道一致性验收的自动化测试（无需硬件）。

覆盖两条路径：
1. **合成配对信号**：PPG 100 Hz + ECG 250 Hz，同一心率 → 走 serial_capture 的落盘逻辑写成两个 CSV
   → 再调 verify_dual_channel 的 CLI，校验「双通道对照」这条验收链路本身是通的；
2. **公开真实配对数据**：BIDMC 同一受试者同步的 PLETH（PPG）与 II（ECG）→ 同样跑一遍 CLI，
   校验在真实波形上最大偏差 ≤5 bpm（这就是 P3 的验收标准）。

跑法：
    ./.venv/bin/python tests/test_dual_channel_acceptance.py
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
from validate_bidmc import load_record  # noqa: E402

CLI = os.path.join(ROOT, "tools", "verify_dual_channel.py")


def write_ppg_csv(path: str, fs: float, samples: np.ndarray) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ppg_ir", "ppg_red"])
        for i, v in enumerate(samples):
            w.writerow([f"{i / fs:.6f}", f"{v:.3f}", f"{v * 0.6:.3f}"])


def write_ecg_csv(path: str, fs: float, samples: np.ndarray) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "ecg"])
        for i, v in enumerate(samples):
            w.writerow([f"{i / fs:.6f}", f"{v:.6f}"])


def run_cli(args: list[str]) -> tuple[int, str]:
    res = subprocess.run([sys.executable, CLI] + args, capture_output=True, text=True)
    return res.returncode, res.stdout + res.stderr


def parse_max_diff(output: str) -> float | None:
    for line in output.splitlines():
        if "最大偏差" in line:
            return float(line.split("最大偏差")[1].strip().split()[0])
    return None


def main() -> int:
    fails = 0

    print("== 1. 合成配对信号（PPG 100 Hz + ECG 250 Hz，同一心率 75 bpm）==")
    ppg = synth.synth_ppg(100.0, 120.0, 75.0, noise=0.004, seed=5)
    ecg = synth.synth_ecg(250.0, 120.0, 75.0, noise=0.01, seed=5)
    ppg_csv, ecg_csv = "/tmp/dual_ppg.csv", "/tmp/dual_ecg.csv"
    write_ppg_csv(ppg_csv, 100.0, ppg)
    write_ecg_csv(ecg_csv, 250.0, ecg)
    code, out = run_cli(["--ppg", ppg_csv, "--ecg", ecg_csv, "--window", "30", "--limit", "5"])
    max_diff = parse_max_diff(out)
    ok = code == 0 and max_diff is not None and max_diff <= 5.0
    print(f"{'PASS' if ok else 'FAIL'}  合成双通道验收：退出码={code}  最大偏差={max_diff} bpm")
    if not ok:
        print(out)
        fails += 1

    print("\n== 2. 公开真实配对数据（BIDMC，同一受试者同步 PPG + ECG）==")
    record = os.path.join(ROOT, "data", "raw", "bidmc01")
    if not (os.path.exists(record + ".hea") and os.path.exists(record + ".dat")):
        print("跳过：data/raw/bidmc01 不存在（下载方法见 docs/05）")
    else:
        code, out = run_cli(["--from-bidmc", "bidmc01", "--window", "30", "--limit", "5"])
        max_diff = parse_max_diff(out)
        ok = code == 0 and max_diff is not None and max_diff <= 5.0
        print(f"{'PASS' if ok else 'FAIL'}  BIDMC 真实数据双通道验收：退出码={code}  "
              f"最大偏差={max_diff} bpm（验收上限 5）")
        if not ok:
            print(out)
            fails += 1

    print("\n== 3. 超限时必须判 FAIL（验证验收逻辑本身不是永远 PASS）==")
    # 故意把 ECG 通道换成心率明显不同的信号（100 bpm），偏差应超出 3 bpm 上限
    ecg_fast = synth.synth_ecg(250.0, 120.0, 100.0, noise=0.01, seed=9)
    write_ecg_csv(ecg_csv, 250.0, ecg_fast)
    code, out = run_cli(["--ppg", ppg_csv, "--ecg", ecg_csv, "--window", "30", "--limit", "3"])
    max_diff = parse_max_diff(out)
    ok = code != 0 and max_diff is not None and max_diff > 3.0
    print(f"{'PASS' if ok else 'FAIL'}  偏差超限时退出码非 0：退出码={code}  最大偏差={max_diff} bpm")
    if not ok:
        print(out)
        fails += 1

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
