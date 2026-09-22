#!/usr/bin/env python3
"""生成工频陷波（notch）的双二阶系数，供 firmware/app/notch.c 使用。

设计取舍：
- 用 Q=20 的 IIR 陷波（scipy.signal.iirnotch）：在 50 Hz 处做出很深的零点（实测 -85 dB 量级），
  同时对 10 Hz 左右的 QRS 频段几乎无衰减（<0.1 dB）。
- **不是所有场景都需要它**：ECG 的 5–15 Hz 带通本身在 50 Hz 已有约 -23 dB 衰减（见 docs/03）。
  陷波真正的用武之地是：① 需要把**原始/宽带波形**发给上位机看形态时；② 工频干扰幅度远大于 QRS 时
  （-23 dB 不够用）；③ 把 QRS 带通放宽到 0.5–40 Hz 做形态分析时。

用法：
    ./.venv/bin/python tools/gen_notch_coeffs.py                 # 默认 50/60 Hz × 125/250 Hz
    ./.venv/bin/python tools/gen_notch_coeffs.py --q 30
"""

from __future__ import annotations

import argparse

from scipy.signal import iirnotch


def emit(fs: float, f0: float, q: float) -> None:
    b, a = iirnotch(f0, q, fs)
    print(f"/* {f0:.0f} Hz 陷波, Q={q:g}, fs={fs:.0f} Hz */")
    print("{{ {:.9f}f, {:.9f}f, {:.9f}f }},".format(*b))
    print("{{ {:.9f}f, {:.9f}f, {:.9f}f }},".format(*a))
    print()


def main() -> None:
    ap = argparse.ArgumentParser(description="生成工频陷波系数")
    ap.add_argument("--fs", type=float, nargs="*", default=[125.0, 250.0])
    ap.add_argument("--f0", type=float, nargs="*", default=[50.0, 60.0])
    ap.add_argument("--q", type=float, default=20.0)
    args = ap.parse_args()
    for fs in args.fs:
        for f0 in args.f0:
            if f0 >= fs / 2.0:
                continue
            emit(fs, f0, args.q)
    print("// 改完系数后请重跑：./.venv/bin/python tests/test_notch.py")


if __name__ == "__main__":
    main()
