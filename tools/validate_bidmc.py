#!/usr/bin/env python3
"""用公开真实数据验证 PPG 心率估计（PhysioNet BIDMC：同一受试者同步 PPG + ECG）。

为什么这条最重要：
    「算法在合成信号上对」只能证明实现没写错；**在真实 ICU 数据上，用同一段记录的 ECG 心率
    当独立真值去校验 PPG 心率**，才说明这套峰值检测在真实波形上站得住。
    这也是面试里最容易讲清、又最难造假的一段证据。

数据：BIDMC PPG and ECG（53 例，125 Hz，PLETH = PPG，II = ECG）
    https://physionet.org/files/bidmc/1.0.0/
    数据不随仓库分发；脚本优先读 data/raw/，没有就联网下载（--record 指定记录号）。

用法：
    ./.venv/bin/python tools/validate_bidmc.py --record bidmc01
    ./.venv/bin/python tools/validate_bidmc.py --record bidmc01 --window 30 --out data/validation.csv
"""

from __future__ import annotations

import argparse
import csv
import os

import numpy as np

try:
    import wfdb
except ImportError:  # pragma: no cover
    wfdb = None

from ecg_qrs import ecg_qrs_detect, hr_from_peaks
from ppg_hr import analyse as ppg_analyse


def load_record(record: str, raw_dir: str = "data/raw") -> tuple[np.ndarray, np.ndarray, float]:
    """返回 (ppg, ecg, fs)。优先本地 data/raw/，否则从 PhysioNet 下载。"""
    local = os.path.join(raw_dir, record)
    if os.path.exists(local + ".hea") and os.path.exists(local + ".dat"):
        try:
            rec = wfdb.rdrecord(local)
        except Exception as exc:      # 下载未完成/文件损坏时不要让整批验证崩掉
            raise SystemExit(f"本地文件不可读（可能是下载未完成）：{exc}")
        source = f"本地 {local}.dat"
    else:
        if wfdb is None:
            raise SystemExit("需要 wfdb：./.venv/bin/pip install wfdb")
        try:
            rec = wfdb.rdrecord(record, pn_dir="bidmc")
        except Exception as exc:
            raise SystemExit(f"PhysioNet 下载失败（网络慢/离线）：{exc}")
        source = f"PhysioNet bidmc/{record}"
    # BIDMC 头文件里的信号名带尾随逗号（'PLETH,'），统一规范化后再匹配
    names = [s.strip().strip(",").strip().upper() for s in rec.sig_name]

    def find(*candidates: str) -> int:
        for cand in candidates:
            if cand in names:
                return names.index(cand)
        raise SystemExit(f"记录 {record} 里找不到信号 {candidates}，实际信号：{names}")

    ppg_idx = find("PLETH", "PPG")
    ecg_idx = find("II", "ECG", "V")
    ppg = np.asarray(rec.p_signal[:, ppg_idx], dtype=float)
    ecg = np.asarray(rec.p_signal[:, ecg_idx], dtype=float)
    print(f"数据来源: {source}  fs={rec.fs} Hz  时长={len(ppg) / rec.fs:.1f}s  信号={rec.sig_name}")
    return ppg, ecg, float(rec.fs)


def validate(ppg: np.ndarray, ecg: np.ndarray, fs: float, window_s: float = 30.0,
             step_s: float | None = None, k: float = 0.6) -> list[dict]:
    """逐窗比较 PPG 心率与 ECG 心率。"""
    step = int((step_s or window_s) * fs)
    win = int(window_s * fs)
    rows: list[dict] = []
    for start in range(0, max(0, len(ppg) - win + 1), step):
        seg_ppg, seg_ecg = ppg[start:start + win], ecg[start:start + win]
        hr_ecg = hr_from_peaks(ecg_qrs_detect(seg_ecg, fs), fs)
        res = ppg_analyse(seg_ppg, fs, k=k)
        hr_ppg = res["hr_bpm"]
        if np.isnan(hr_ecg) or np.isnan(hr_ppg):
            continue
        rows.append({
            "start_s": round(start / fs, 1),
            "hr_ecg": round(hr_ecg, 1),
            "hr_ppg": round(hr_ppg, 1),
            "abs_err": round(abs(hr_ppg - hr_ecg), 1),
            "pi_percent": round(res["pi_percent"], 3),
            "template_corr": None if np.isnan(res["template_corr"]) else round(res["template_corr"], 3),
            "quality_ok": res["quality_ok"],
        })
    return rows


def summarise(rows: list[dict]) -> dict:
    if not rows:
        return {}
    errs = np.array([r["abs_err"] for r in rows], dtype=float)
    good = [r for r in rows if r["quality_ok"]]
    good_errs = np.array([r["abs_err"] for r in good], dtype=float) if good else np.array([])
    out = {
        "windows": len(rows),
        "mae_all": float(np.mean(errs)),
        "rmse_all": float(np.sqrt(np.mean(errs ** 2))),
        "p95_all": float(np.percentile(errs, 95)),
        "windows_quality_ok": len(good),
    }
    if len(good_errs):
        out.update({
            "mae_quality_ok": float(np.mean(good_errs)),
            "rmse_quality_ok": float(np.sqrt(np.mean(good_errs ** 2))),
            "p95_quality_ok": float(np.percentile(good_errs, 95)),
        })
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="BIDMC 数据集验证 PPG 心率估计")
    ap.add_argument("--record", default="bidmc01", help="单条记录")
    ap.add_argument("--records", default=None,
                    help="多条记录，逗号分隔（如 bidmc01,bidmc02），用于汇总统计")
    ap.add_argument("--raw-dir", default="data/raw")
    ap.add_argument("--window", type=float, default=30.0, help="窗口秒数")
    ap.add_argument("--step", type=float, default=None)
    ap.add_argument("--k", type=float, default=0.6)
    ap.add_argument("--out", default=None, help="把逐窗结果写成 CSV")
    ap.add_argument("--quiet", action="store_true", help="多条记录时只打印汇总")
    args = ap.parse_args()

    records = ([r.strip() for r in args.records.split(",") if r.strip()]
               if args.records else [args.record])
    all_rows: list[dict] = []
    per_record: list[tuple[str, dict]] = []

    for record in records:
        try:
            ppg, ecg, fs = load_record(record, args.raw_dir)
        except SystemExit as exc:
            print(f"[跳过] {record}: {exc}")
            continue
        rows = validate(ppg, ecg, fs, window_s=args.window, step_s=args.step, k=args.k)
        if not rows:
            print(f"[跳过] {record}: 没有可用窗口")
            continue
        for r in rows:
            r["record"] = record
        all_rows.extend(rows)
        s = summarise(rows)
        per_record.append((record, s))
        if not args.quiet:
            print(f"\n窗口 {args.window:.0f}s，{record} 共 {len(rows)} 个有效窗口")
            print(f"{'起止(s)':>9} {'ECG(hrm)':>9} {'PPG(est)':>9} {'|误差|':>7} {'PI%':>7} {'模板相关':>8} {'可信':>5}")
            for r in rows:
                corr = "  n/a" if r["template_corr"] is None else f"{r['template_corr']:>6.2f}"
                print(f"{r['start_s']:>9.1f} {r['hr_ecg']:>9.1f} {r['hr_ppg']:>9.1f} "
                      f"{r['abs_err']:>7.1f} {r['pi_percent']:>7.3f} {corr:>8} {'是' if r['quality_ok'] else '否':>5}")

    if not all_rows:
        raise SystemExit("没有任何可用窗口")

    print("\n== 逐记录汇总 ==")
    print(f"{'记录':>10} {'窗口':>5} {'MAE':>6} {'RMSE':>6} {'P95':>6} {'质量达标':>8}")
    for record, s in per_record:
        print(f"{record:>10} {s['windows']:>5} {s['mae_all']:>6.2f} {s['rmse_all']:>6.2f} "
              f"{s['p95_all']:>6.2f} {s['windows_quality_ok']:>5}/{s['windows']}")

    s = summarise(all_rows)
    print("\n== 全部记录合计 ==")
    print(f"窗口总数 {s['windows']}（质量达标 {s['windows_quality_ok']}）")
    print(f"全部窗口: MAE={s['mae_all']:.2f} bpm  RMSE={s['rmse_all']:.2f} bpm  P95={s['p95_all']:.2f} bpm")
    if "mae_quality_ok" in s:
        print(f"质量达标窗口: MAE={s['mae_quality_ok']:.2f} bpm  "
              f"RMSE={s['rmse_quality_ok']:.2f} bpm  P95={s['p95_quality_ok']:.2f} bpm")
    print("\n⚠️ 边界：BIDMC 为 ICU 静息数据（受试者多卧床、HR 范围集中），"
          "验证的是「采集链路 + 峰值检测算法」的正确性，不等价于可穿戴在运动伪影下的表现。")

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", newline="", encoding="utf-8") as fh:
            fields = ["record"] + [k for k in all_rows[0].keys() if k != "record"]
            w = csv.DictWriter(fh, fieldnames=fields)
            w.writeheader()
            w.writerows(all_rows)
        print(f"逐窗结果已写入 {args.out}")


if __name__ == "__main__":
    main()
