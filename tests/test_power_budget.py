#!/usr/bin/env python3
"""低功耗预算的 C / Python 一致性测试。

为什么值得测：
    低功耗预算会因为**一处单位换算**（mA×100 → µA）或**一处取整**差出 10%，
    而这类差异在真机上只表现为「续航比预期短一点」，极难定位。
    这里用固定用例把 C 固件侧与 Python 工具侧逐例对齐，并顺手校验手算期望值。

跑法：
    ./.venv/bin/python tests/test_power_budget.py
"""

from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools import power_budget as pb  # noqa: E402

EXE = "/tmp/biosignal_power_host"

# 用例与 tests/power_budget_host_main.c 一一对应（同名同参数）
CASES: dict[str, tuple[pb.PowerCfg, int]] = {
    "A_1min": (pb.PowerCfg(60_000, 2_000, 1_000, 2_000, 1_200, 20, 2), 200),
    "B_5min": (pb.PowerCfg(300_000, 5_000, 3_000, 2_000, 1_200, 20, 2), 200),
    "C_continuous": (pb.PowerCfg(60_000, 60_000, 10_000, 2_000, 1_200, 20, 2), 200),
    "D_on_gt_period": (pb.PowerCfg(1_000, 2_000, 500, 2_000, 1_200, 20, 2), 200),
    "E_sample_gt_on": (pb.PowerCfg(60_000, 1_000, 2_000, 2_000, 1_200, 20, 2), 200),
    "F_zero_period": (pb.PowerCfg(0, 0, 0, 2_000, 1_200, 20, 2), 200),
    "G_10min": (pb.PowerCfg(600_000, 1_000, 500, 2_000, 1_200, 20, 2), 200),
}

# 手算期望值（与 C 侧断言使用同一组数字）
EXPECTED = {
    "A_1min": {"avg_ua": 887, "duty": 33, "life_h": 225},
    "B_5min": {"avg_ua": 474, "duty": 16, "life_h": 421},
    "C_continuous": {"avg_ua": 22_000, "duty": 1000, "life_h": 9},
    "G_10min": {"avg_ua": 65, "duty": 1, "life_h": 3076},
}


def build() -> None:
    cmd = ["cc", "-std=c99", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "firmware", "app"),
           os.path.join(ROOT, "tests", "power_budget_host_main.c"),
           os.path.join(ROOT, "firmware", "app", "power_budget.c"),
           "-o", EXE]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit("C 侧编译失败：\n" + res.stderr)


def run_c() -> dict[str, dict[str, int]]:
    res = subprocess.run([EXE], capture_output=True, text=True)
    out: dict[str, dict[str, int]] = {}
    for line in res.stdout.splitlines():
        if not line.startswith("CASE "):
            continue
        parts = line.split()
        name = parts[1]
        values: dict[str, int] = {}
        for tok in parts[2:]:
            k, _, v = tok.partition("=")
            values[k] = int(v)
        out[name] = values
    return out


def main() -> int:
    build()
    c = run_c()
    fails = 0

    print("== C 与 Python 逐例一致性 ==")
    for name, (cfg, battery) in CASES.items():
        py = {
            "avg_ua": pb.average_ua(cfg),
            "duty": pb.duty_permille(cfg),
            "life_h": pb.life_hours(cfg, battery),
            "max_on_500": pb.max_on_ms(cfg, 500),
            "status": pb.validate(cfg),
        }
        cc = c.get(name)
        if cc is None:
            print(f"FAIL  {name}: C 侧没有输出该用例")
            fails += 1
            continue
        same = all(cc.get(k) == v for k, v in py.items())
        print(f"{'PASS' if same else 'FAIL'}  {name}: C={cc}  Python={py}")
        fails += 0 if same else 1

    print("\n== 反解校验：目标 500 µA 时每个周期最多能醒多久 ==")
    max_on = pb.max_on_ms(CASES["A_1min"][0], 500)
    ok = max_on == 896
    print(f"{'PASS' if ok else 'FAIL'}  1 分钟周期、目标 500 µA → {max_on} ms（期望 896）")
    fails += 0 if ok else 1
    unreachable = pb.max_on_ms(CASES["A_1min"][0], 10) == 0
    print(f"{'PASS' if unreachable else 'FAIL'}  目标 10 µA 低于睡眠电流(22 µA) → 返回 0")
    fails += 0 if unreachable else 1

    print("\n== 手算期望值校验（作为独立第三方对照） ==")
    for name, exp in EXPECTED.items():
        cfg, battery = CASES[name]
        actual = {"avg_ua": pb.average_ua(cfg), "duty": pb.duty_permille(cfg),
                  "life_h": pb.life_hours(cfg, battery)}
        ok = actual == exp
        print(f"{'PASS' if ok else 'FAIL'}  {name}: 期望={exp}  实际={actual}")
        fails += 0 if ok else 1

    print(f"\n合计失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
