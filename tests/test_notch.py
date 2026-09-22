#!/usr/bin/env python3
"""工频陷波验证：C 实现 vs Python（scipy）参考 + 支持范围检查。

断言（都能直接对上面试追问）：
  1. 陷波频率处衰减 **≤ −60 dB**（实测 −95…−110 dB）；
  2. QRS 频段（10 Hz）几乎无衰减 **≥ −0.5 dB**（实测 −0.00 dB）；
  3. 不支持的组合（如 200 Hz + 50 Hz）**明确失败**（ready=0）并直通，不静默用近似系数；
  4. **实测对比**：只看 QRS 检测用的 5–15 Hz 带通本身在 50 Hz 已有约 −23 dB —— 说明陷波不是必需品，
     而是"看原始波形/干扰过大"时才上（这条写进 docs/03，避免为了堆滤波器而上滤波器）。

跑法：
    ./.venv/bin/python tests/test_notch.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

import numpy as np
from scipy.signal import butter, iirnotch, lfilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = "/tmp/biosignal_notch_host"

COMBOS = [(250.0, 50.0), (250.0, 60.0), (125.0, 50.0), (125.0, 60.0)]


def build() -> None:
    cmd = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "firmware", "app"),
           os.path.join(ROOT, "tests", "notch_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "notch.c"),
           os.path.join(ROOT, "firmware", "app", "biquad.c"),
           "-lm", "-o", EXE]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit("C 侧编译失败：\n" + res.stderr)


def run_c(fs: float, f0: float) -> dict[str, float]:
    res = subprocess.run([EXE, str(fs), str(f0)], capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit(f"C 侧运行失败：{res.stderr}")
    for line in res.stdout.splitlines():
        if line.startswith("RESULT"):
            out: dict[str, float] = {}
            for tok in line.split()[1:]:
                k, _, v = tok.partition("=")
                out[k] = float(v)
            return out
    raise SystemExit("C 侧输出无法解析：" + res.stdout)


def py_attenuation(fs: float, f: float, f0: float, q: float = 20.0) -> float:
    """Python 参考：iirnotch + lfilter，用稳态 RMS 比得到衰减（dB）。"""
    b, a = iirnotch(f0, q, fs)
    n = int(4.0 * fs)
    skip = int(1.0 * fs)
    t = np.arange(n) / fs
    x = np.sin(2 * np.pi * f * t)
    y = lfilter(b, a, x)
    rms_in = float(np.sqrt(np.mean(x[skip:] ** 2)))
    rms_out = float(np.sqrt(np.mean(y[skip:] ** 2)))
    return 20.0 * np.log10(rms_out / rms_in)


def bandpass_attenuation(fs: float, f: float) -> float:
    """只看 ECG 的 5–15 Hz 带通（不含陷波）在 f 处的衰减。"""
    bh, ah = butter(2, 5 / (fs / 2), btype="high")
    bl, al = butter(2, 15 / (fs / 2), btype="low")
    n = int(4.0 * fs)
    skip = int(1.0 * fs)
    t = np.arange(n) / fs
    x = np.sin(2 * np.pi * f * t)
    y = lfilter(bl, al, lfilter(bh, ah, x))
    rms_in = float(np.sqrt(np.mean(x[skip:] ** 2)))
    rms_out = float(np.sqrt(np.mean(y[skip:] ** 2)))
    return 20.0 * np.log10(rms_out / rms_in)


def main() -> int:
    build()
    fails = 0

    print("== 1. 陷波深度与通带保持（C 实现） ==")
    for fs, f0 in COMBOS:
        r = run_c(fs, f0)
        att = r[f"atten_{int(f0)}_db"]
        inband = r["atten_10_db"]
        ok = r["ready"] == 1 and att <= -60.0 and inband >= -0.5
        print(f"{'PASS' if ok else 'FAIL'}  fs={fs:.0f} f0={f0:.0f}: "
              f"陷波 {att:.2f} dB　10 Hz {inband:.2f} dB　ready={int(r['ready'])}")
        fails += 0 if ok else 1

    print("\n== 2. C 与 Python 参考一致（都必须是深零点） ==")
    for fs, f0 in COMBOS:
        c_att = run_c(fs, f0)[f"atten_{int(f0)}_db"]
        p_att = py_attenuation(fs, f0, f0)
        ok = c_att <= -60.0 and p_att <= -60.0
        print(f"{'PASS' if ok else 'FAIL'}  fs={fs:.0f} f0={f0:.0f}: C={c_att:.2f} dB  Python={p_att:.2f} dB"
              f"　（两者测量方法不同：C 用 Goertzel，Python 用稳态 RMS，只要求同为深零点）")
        fails += 0 if ok else 1

    print("\n== 3. 不支持的组合必须明确失败并直通 ==")
    r = run_c(200.0, 50.0)
    ok = r["ready"] == 0 and abs(r["atten_50_db"]) < 1e-9
    print(f"{'PASS' if ok else 'FAIL'}  fs=200 f0=50: ready={int(r['ready'])} "
          f"衰减={r['atten_50_db']:.2f} dB（直通）")
    fails += 0 if ok else 1

    print("\n== 4. 对照：5–15 Hz 带通本身在 50 Hz 的衰减（说明陷波不是必需品） ==")
    for fs in (125.0, 250.0):
        bp = bandpass_attenuation(fs, 50.0)
        print(f"      fs={fs:.0f}: 仅带通 {bp:.2f} dB　→ 陷波只在原始波形/干扰过大时才需要")

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
