#!/usr/bin/env python3
"""低功耗预算计算（Python 镜像 / 命令行工具）。

与 C 侧 `firmware/app/power_budget.c` **逐例一致**（整数运算，同一套公式），
一致性由 `tests/test_power_budget.py` 校验——固件里做的取舍，上位机能算出一模一样的数。

用法：
    ./.venv/bin/python tools/power_budget.py --period 60000 --on 2000 --sample 1000
    ./.venv/bin/python tools/power_budget.py --period 60000 --on 2000 --sample 1000 --battery 200
    ./.venv/bin/python tools/power_budget.py --sweep          # 扫描几组典型占空比
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, asdict

POWER_OK = 0
POWER_ERR_PERIOD_ZERO = 1
POWER_ERR_ON_GT_PERIOD = 2
POWER_ERR_SAMPLE_GT_ON = 3


@dataclass
class PowerCfg:
    period_ms: int = 60_000
    on_ms: int = 2_000
    sample_ms: int = 1_000
    run_ma_x100: int = 2_000        # 20.00 mA
    sensor_ma_x100: int = 1_200     # 12.00 mA（含 LED）
    stop_ua: int = 20
    rtc_ua: int = 2


def validate(cfg: PowerCfg) -> int:
    if cfg.period_ms == 0:
        return POWER_ERR_PERIOD_ZERO
    if cfg.on_ms > cfg.period_ms:
        return POWER_ERR_ON_GT_PERIOD
    if cfg.sample_ms > cfg.on_ms:
        return POWER_ERR_SAMPLE_GT_ON
    return POWER_OK


def average_ua(cfg: PowerCfg) -> int:
    """平均电流（µA）。整数除法的截断行为与 C 一致。"""
    if validate(cfg) != POWER_OK:
        return 0
    run_ua = cfg.run_ma_x100 * 10
    sensor_ua = cfg.sensor_ma_x100 * 10
    sleep_ua = cfg.stop_ua + cfg.rtc_ua
    sleep_ms = cfg.period_ms - cfg.on_ms
    num = run_ua * cfg.on_ms + sensor_ua * cfg.sample_ms + sleep_ua * sleep_ms
    return num // cfg.period_ms


def duty_permille(cfg: PowerCfg) -> int:
    if validate(cfg) != POWER_OK:
        return 0
    return (cfg.on_ms * 1000) // cfg.period_ms


def life_hours(cfg: PowerCfg, capacity_mah: int) -> int:
    avg = average_ua(cfg)
    if avg == 0:
        return 0
    return (capacity_mah * 1000) // avg


def max_on_ms(cfg: PowerCfg, target_avg_ua: int) -> int:
    """反解：给定平均电流目标，每个周期最多能醒多久（保守假设采样期 = 活动期）。"""
    if validate(cfg) != POWER_OK:
        return 0
    run_ua = cfg.run_ma_x100 * 10
    sensor_ua = cfg.sensor_ma_x100 * 10
    sleep_ua = cfg.stop_ua + cfg.rtc_ua
    if target_avg_ua <= sleep_ua:
        return 0
    active_extra = run_ua + sensor_ua - sleep_ua
    if active_extra == 0:
        return 0
    on_ms = ((target_avg_ua - sleep_ua) * cfg.period_ms) // active_extra
    return min(on_ms, cfg.period_ms)


def describe(cfg: PowerCfg, capacity_mah: int = 200) -> str:
    status = validate(cfg)
    if status != POWER_OK:
        return f"配置非法（错误码 {status}）：period={cfg.period_ms} on={cfg.on_ms} sample={cfg.sample_ms}"
    avg = average_ua(cfg)
    life = life_hours(cfg, capacity_mah)
    return (f"周期 {cfg.period_ms} ms / 活动 {cfg.on_ms} ms / 采样 {cfg.sample_ms} ms  "
            f"占空比 {duty_permille(cfg) / 10:.1f}%  "
            f"平均电流 {avg / 1000:.3f} mA  续航(按 {capacity_mah} mAh) {life} h ≈ {life / 24:.1f} 天")


def main() -> None:
    ap = argparse.ArgumentParser(description="低功耗占空比预算")
    ap.add_argument("--period", type=int, default=60_000)
    ap.add_argument("--on", type=int, default=2_000)
    ap.add_argument("--sample", type=int, default=1_000)
    ap.add_argument("--run-ma-x100", type=int, default=2_000, help="MCU 活动电流 mA×100")
    ap.add_argument("--sensor-ma-x100", type=int, default=1_200, help="采样期传感器+LED 电流 mA×100")
    ap.add_argument("--stop-ua", type=int, default=20)
    ap.add_argument("--rtc-ua", type=int, default=2)
    ap.add_argument("--battery", type=int, default=200, help="电池容量 mAh")
    ap.add_argument("--sweep", action="store_true", help="扫描几组典型配置")
    args = ap.parse_args()

    if args.sweep:
        print("典型占空比对比（200 mAh 电池）：")
        # 与 docs/06-P2低功耗设计.md 的表格一一对应（文档里的命令能复现文档里的数字）
        for period, on, sample in [(60_000, 60_000, 10_000),      # 连续（对照）
                                   (60_000, 2_000, 1_000),        # 1 分钟一次
                                   (300_000, 5_000, 3_000),       # 5 分钟一次
                                   (600_000, 1_000, 500)]:        # 10 分钟一次
            cfg = PowerCfg(period_ms=period, on_ms=on, sample_ms=sample)
            print("  " + describe(cfg, args.battery))
        print("\n注：连续模式（on=period）用于对照——它说明「不省电」到底贵多少。")
        return

    cfg = PowerCfg(period_ms=args.period, on_ms=args.on, sample_ms=args.sample,
                   run_ma_x100=args.run_ma_x100, sensor_ma_x100=args.sensor_ma_x100,
                   stop_ua=args.stop_ua, rtc_ua=args.rtc_ua)
    print(describe(cfg, args.battery))
    d = asdict(cfg)
    print(f"输入: {d}")


if __name__ == "__main__":
    main()
