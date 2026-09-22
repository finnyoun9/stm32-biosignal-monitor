#!/usr/bin/env python3
"""C / Python 协议一致性测试（跨语言字节级比对）。

为什么值得单独测：
    真机采集时上位机与固件是两套独立实现，协议一旦不一致，表现是「波形偶尔错乱」这种极难定位的现象。
    这里用同一组输入，让 C 实现（`firmware/include/protocol.h`，最终会烧进 MCU）与 Python 实现
    （`tools/protocol.py`，上位机）输出**逐字节相同**的帧，从源头消灭这类问题。

跑法：
    ./.venv/bin/python tests/test_protocol_conformance.py
"""

from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools import protocol  # noqa: E402


def build_and_run() -> dict[str, str]:
    """编译并运行 C 侧自测，返回 {标签: 十六进制串}。"""
    src = os.path.join(ROOT, "tests", "protocol_host_main.c")
    exe = "/tmp/biosignal_proto_host"
    cmd = ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
           "-I", os.path.join(ROOT, "firmware", "include"), src, "-o", exe]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    parsed: dict[str, str] = {}
    for line in out.strip().splitlines():
        parts = line.split()
        if parts:
            parsed[parts[0]] = " ".join(parts[1:])
    return parsed


def py_frames() -> dict[str, str]:
    """Python 侧生成同样内容的帧。"""
    samples = [(131071, 65535), (0, 1), (123456, 54321)]
    frames = {
        "PPG_BATCH": protocol.ppg_batch(seq=7, ts_ms=123456, samples=samples).encode().hex(),
        "STATUS": protocol.status(seq=9, led_current=200, fs_hz=50, quality_ok=False,
                                  dropped=17).encode().hex(),
        "LOG": protocol.log(seq=2, text="boot ok").encode().hex(),
        "ECG_BATCH": protocol.ecg_batch(seq=5, ts_ms=1000, samples=[0, 2048, 4095, 65535]).encode().hex(),
    }
    return frames


def main() -> int:
    c = build_and_run()
    p = py_frames()
    failures = 0
    for tag in ("PPG_BATCH", "ECG_BATCH", "STATUS", "LOG"):
        same = c.get(tag) == p.get(tag)
        print(f"{'PASS' if same else 'FAIL'}  {tag}  C={c.get(tag)}  PY={p.get(tag)}")
        failures += 0 if same else 1

    # C 侧解析回归：半帧 + 前导噪声应重同步，CRC 破坏应被拒绝
    resync_ok = c.get("RESYNC", "").startswith("frames=1")
    crc_ok = "crc_errors=1" in c.get("CRC_REJECT", "")
    print(f"{'PASS' if resync_ok else 'FAIL'}  C 半帧+噪声重同步  [{c.get('RESYNC')}]")
    print(f"{'PASS' if crc_ok else 'FAIL'}  C CRC 错误拒绝  [{c.get('CRC_REJECT')}]")
    failures += 0 if resync_ok else 1
    failures += 0 if crc_ok else 1

    print(f"\n合计 7 项，失败 {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
