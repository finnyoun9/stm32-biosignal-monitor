#!/usr/bin/env python3
"""生成固件用的双二阶（biquad）滤波系数，直接可粘贴进 firmware/app/ppg_algo.c。

为什么把系数固定成表：MCU 上运行时算 Butterworth 系数需要 tan/cos 与对数，成本高且易错；
嵌入式工程里更常见的做法是「支持的采样率 × 预生成系数」，并在代码里明确拒绝不支持的采样率。

用法：
    ./.venv/bin/python tools/gen_biquad_coeffs.py            # 默认 50/100/200 Hz
    ./.venv/bin/python tools/gen_biquad_coeffs.py --fs 128 --low 0.5 --high 4
    ./.venv/bin/python tools/gen_biquad_coeffs.py --fs 125 250 --low 5 --high 15   # ECG QRS 带通
"""

from __future__ import annotations

import argparse

from scipy.signal import butter


def emit(fs: float, low: float, high: float) -> None:
    bh, ah = butter(2, low / (fs / 2.0), btype="high")
    bl, al = butter(2, high / (fs / 2.0), btype="low")
    tag = f"{int(fs)}"
    print(f"/* fs = {fs:.0f} Hz, 带通 {low}–{high} Hz = 2 阶高通 + 2 阶低通级联 */")
    print(f"static const float HP_B_{tag}[3] = {{{', '.join(f'{v:.9f}f' for v in bh)}}};")
    print(f"static const float HP_A_{tag}[3] = {{{', '.join(f'{v:.9f}f' for v in ah)}}};")
    print(f"static const float LP_B_{tag}[3] = {{{', '.join(f'{v:.9f}f' for v in bl)}}};")
    print(f"static const float LP_A_{tag}[3] = {{{', '.join(f'{v:.9f}f' for v in al)}}};")
    print()


def main() -> None:
    ap = argparse.ArgumentParser(description="生成 ppg_algo.c 的滤波系数表")
    ap.add_argument("--fs", type=float, nargs="*", default=[50.0, 100.0, 200.0])
    ap.add_argument("--low", type=float, default=0.5, help="高通截止（Hz）")
    ap.add_argument("--high", type=float, default=4.0, help="低通截止（Hz）")
    args = ap.parse_args()
    for fs in args.fs:
        emit(fs, args.low, args.high)
    print("// 改完系数后请重跑：./.venv/bin/python tests/test_algo_conformance.py")


if __name__ == "__main__":
    main()
