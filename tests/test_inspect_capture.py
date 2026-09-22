#!/usr/bin/env python3
"""采集体检报告工具的自检（无需硬件）。

验证的是「报告能把好坏数据区分开」这件事本身：
  1. 好数据（PPG 100 Hz + ECG 250 Hz，同一心率）→ 判定 PASS；
  2. 坏数据（PPG 中间挖掉 5 s 造成时间戳空隙 + ECG 换成不同心率）→ 判定 FAIL，
     且报告文本里确实指出了「时间戳空隙」与「双通道验收 FAIL」。

跑法：
    ./.venv/bin/python tests/test_inspect_capture.py
"""

from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "inspect_capture.py")


def main() -> int:
    res = subprocess.run([sys.executable, TOOL, "--selftest"], capture_output=True, text=True)
    sys.stdout.write(res.stdout)
    if res.stderr:
        sys.stderr.write(res.stderr)
    if res.returncode != 0:
        print("inspect_capture --selftest 失败")
        return 1

    # 额外检查：报告文件确实被写出来了（避免"打印 PASS 但没产出文件"）
    md = "/tmp/inspect/report-ok.md"
    ok = os.path.exists(md) and os.path.getsize(md) > 200
    print(f"{'PASS' if ok else 'FAIL'}  报告文件已生成：{md}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
