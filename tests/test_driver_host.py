#!/usr/bin/env python3
"""MAX30102 驱动主机端单测运行器（编译 + 运行 tests/max30102_host_main.c）。

    ./.venv/bin/python tests/test_driver_host.py
"""

from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    exe = "/tmp/biosignal_max30102_host"
    cmd = ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
           "-I", os.path.join(ROOT, "firmware", "app"),
           os.path.join(ROOT, "tests", "max30102_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "max30102.c"),
           "-o", exe]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print("编译失败：")
        print(build.stderr)
        return 1
    run = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.stderr:
        sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
