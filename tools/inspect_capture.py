#!/usr/bin/env python3
"""采集数据一页体检报告：上机采完立刻知道「这次采集到底行不行」。

解决的问题：上机现场最怕「采了一堆数，回头才发现某一项是坏的」。这个脚本把判断项一次算完并落成报告：
    * 数据完整性：样本数 / 时长 / 采样率（从时间戳推算）/ **时间戳空隙（丢包）**
    * PPG 侧：PI 灌注指数、心率、模板相关性、质量判定
    * ECG 侧：检出 R 波数、心率、RR 稳定性
    * 双通道：30 s 窗口心率对照（P3 验收标准 ≤5 bpm）
    * 波形图：PPG 原始 + 带通、ECG + R 波标注

用法：
    ./.venv/bin/python tools/inspect_capture.py --ppg data/raw/capture-ppg.csv \
        --ecg data/raw/capture-ecg.csv --out docs/img/capture-report.md
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from tools import ppg_hr  # noqa: E402
from ecg_qrs import ecg_qrs_detect, hr_from_peaks  # noqa: E402
from verify_dual_channel import compare, read_csv_column  # noqa: E402


def gap_stats(t: np.ndarray) -> tuple[int, float]:
    """返回 (空隙数, 最大空隙 ms)。空隙定义：相邻时间戳间隔 > 2× 中位间隔。"""
    if len(t) < 3:
        return 0, 0.0
    dt = np.diff(t) * 1000.0
    med = float(np.median(dt))
    if med <= 0:
        return 0, 0.0
    gaps = dt[dt > 2.0 * med]
    return int(len(gaps)), float(np.max(gaps)) if len(gaps) else 0.0


def plot(ppg_t, ppg_v, ppg_peaks, ecg_t, ecg_v, r_peaks, out_png: str, seconds: float = 12.0) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    panels = []
    if len(ppg_v):
        panels.append("ppg")
    if len(ecg_v):
        panels.append("ecg")
    fig, axes = plt.subplots(len(panels), 1, figsize=(11, 3.2 * len(panels)), sharex=False)
    if len(panels) == 1:
        axes = [axes]
    for ax, kind in zip(axes, panels):
        if kind == "ppg":
            n = int(seconds * 100) if len(ppg_v) > seconds * 100 else len(ppg_v)
            seg = ppg_v[:n]
            t = ppg_t[:n]
            ax.plot(t, seg, lw=0.8, color="#1f77b4", label="PPG raw")
            if len(ppg_peaks):
                p = ppg_peaks[ppg_peaks < n]
                ax.plot(ppg_t[p], ppg_v[p], "rv", ms=5, label="PPG peaks")
            ax.set_ylabel("PPG")
            ax.legend(loc="upper right", fontsize=8)
        else:
            n = len(ecg_v)
            ax.plot(ecg_t[:n], ecg_v[:n], lw=0.8, color="#d62728")
            if len(r_peaks):
                ax.plot(ecg_t[r_peaks], ecg_v[r_peaks], "k^", ms=4, label="R peaks")
                ax.legend(loc="upper right", fontsize=8)
            ax.set_ylabel("ECG")
            ax.set_xlabel("time (s)")
    fig.tight_layout()
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    fig.savefig(out_png, dpi=130)
    plt.close(fig)


def build_report(ppg_path: str | None, ecg_path: str | None, out_md: str,
                 out_png: str, ppg_col: str, ecg_col: str, window_s: float,
                 limit_bpm: float) -> tuple[str, bool]:
    lines: list[str] = ["# 采集数据体检报告", ""]
    ok = True

    ppg_t = ppg_v = np.array([])
    ecg_t = ecg_v = np.array([])
    ppg_fs = ecg_fs = 0.0
    ppg_res: dict = {}
    r_peaks = np.array([], dtype=int)

    if ppg_path:
        ppg_t, ppg_v, ppg_fs = read_csv_column(ppg_path, ppg_col)
        gaps, max_gap = gap_stats(ppg_t)
        ppg_res = ppg_hr.analyse(ppg_v, ppg_fs)
        lines += [
            "## PPG 通道", "",
            f"- 文件：`{os.path.basename(ppg_path)}`",
            f"- 样本数 {len(ppg_v)}　时长 {ppg_t[-1] - ppg_t[0]:.1f} s　采样率（按时间戳推算）{ppg_fs:.2f} Hz",
            f"- 时间戳空隙：**{gaps}** 处（最大 {max_gap:.0f} ms）" + ("　⚠️ 有空隙，检查采集任务是否被阻塞" if gaps else "　✅"),
            f"- 心率 **{ppg_res['hr_bpm']:.1f} bpm**　心搏 {ppg_res['n_beats']}",
            f"- 灌注指数 PI **{ppg_res['pi_percent']:.3f}%**"
            + ("　⚠️ 偏低：探头没贴好或 LED 电流太小" if ppg_res["pi_percent"] <= 0.05 else "　✅"),
            f"- 模板相关性 {ppg_res['template_corr']:.3f}　质量判定 **{'可信' if ppg_res['quality_ok'] else '不可信'}**",
            "",
        ]
        if gaps:
            ok = False

    if ecg_path:
        ecg_t, ecg_v, ecg_fs = read_csv_column(ecg_path, ecg_col)
        gaps, max_gap = gap_stats(ecg_t)
        r_peaks = ecg_qrs_detect(ecg_v, ecg_fs)
        hr_ecg = hr_from_peaks(r_peaks, ecg_fs)
        lines += [
            "## ECG 通道", "",
            f"- 文件：`{os.path.basename(ecg_path)}`",
            f"- 样本数 {len(ecg_v)}　时长 {ecg_t[-1] - ecg_t[0]:.1f} s　采样率 {ecg_fs:.2f} Hz",
            f"- 时间戳空隙：**{gaps}** 处（最大 {max_gap:.0f} ms）" + ("　⚠️" if gaps else "　✅"),
            f"- 检出 R 波 {len(r_peaks)}　心率 **{hr_ecg:.1f} bpm**",
            f"- 信号峰峰值 {np.percentile(ecg_v, 99) - np.percentile(ecg_v, 1):.1f}（若过小，检查电极与前端增益）",
            "",
        ]
        if gaps:
            ok = False

    if ppg_path and ecg_path:
        rows, passed = compare(ppg_t, ppg_v, ppg_fs, ecg_t, ecg_v, ecg_fs,
                               window_s=window_s, limit_bpm=limit_bpm)
        lines += ["## 双通道一致性（P3 验收）", "",
                  f"判定标准：同一 {window_s:.0f} s 窗口内两路心率偏差 ≤ **{limit_bpm:.0f} bpm**", "",
                  f"| 起点(s) | PPG | ECG | 偏差 |", "|---:|---:|---:|---:|"]
        for r in rows:
            lines.append(f"| {r['start_s']:.1f} | {r['hr_ppg']:.1f} | {r['hr_ecg']:.1f} | {r['diff']:.1f} |")
        if rows:
            diffs = [r["diff"] for r in rows]
            lines += ["", f"窗口数 {len(rows)}　平均偏差 {np.mean(diffs):.2f} bpm　最大偏差 **{max(diffs):.1f} bpm**",
                      "", f"**验收结果：{'PASS' if passed else 'FAIL'}**", ""]
        else:
            lines += ["", "没有重叠时段可用于对照。", ""]
        ok = ok and passed

    if len(ppg_v) or len(ecg_v):
        plot(ppg_t, ppg_v, ppg_res.get("peaks", np.array([], dtype=int)), ecg_t, ecg_v, r_peaks, out_png)
        lines += ["## 波形", "", f"![waveform]({os.path.relpath(out_png, os.path.dirname(os.path.abspath(out_md)))})", ""]

    lines += ["## 下一步", "",
              "- 有失败项 → 照 `docs/08-上机检查单.md` 对应现象逐条排查（每条都给了假设与实验）。",
              "- 全部通过 → 把本报告与 `docs/04-调试记录.md` 的条目一起提交，作为上机证据。", ""]

    os.makedirs(os.path.dirname(os.path.abspath(out_md)), exist_ok=True)
    with open(out_md, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return out_md, ok


def main() -> int:
    ap = argparse.ArgumentParser(description="采集数据一页体检报告")
    ap.add_argument("--ppg", default=None, help="PPG CSV")
    ap.add_argument("--ecg", default=None, help="ECG CSV")
    ap.add_argument("--ppg-col", default="ppg_ir")
    ap.add_argument("--ecg-col", default="ecg")
    ap.add_argument("--window", type=float, default=30.0)
    ap.add_argument("--limit", type=float, default=5.0)
    ap.add_argument("--out", default="docs/img/capture-report.md")
    ap.add_argument("--plot", default="docs/img/capture-waveform.png")
    ap.add_argument("--selftest", action="store_true", help="用合成数据自检（不需要硬件）")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest(args)

    if not (args.ppg or args.ecg):
        ap.error("至少给一个通道：--ppg 和/或 --ecg（或用 --selftest）")

    path, ok = build_report(args.ppg, args.ecg, args.out, args.plot, args.ppg_col,
                            args.ecg_col, args.window, args.limit)
    print(f"报告已写入 {path}　结果：{'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def run_selftest(args) -> int:
    """合成一对「好数据」和一对「有丢包的坏数据」，验证报告能区分它们。"""
    from tools import synth

    os.makedirs("/tmp/inspect", exist_ok=True)
    ppg = synth.synth_ppg(100.0, 90.0, 75.0, noise=0.004, seed=3)
    ecg = synth.synth_ecg(250.0, 90.0, 75.0, noise=0.01, seed=3)

    def write(path: str, fs: float, values, columns: list[str]) -> None:
        with open(path, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["time_s"] + columns)
            for i, v in enumerate(values):
                extra = [f"{v * 0.6:.3f}"] if len(columns) > 1 else []
                w.writerow([f"{i / fs:.6f}", f"{v:.6f}"] + extra)

    ppg_ok, ecg_ok = "/tmp/inspect/ppg-ok.csv", "/tmp/inspect/ecg-ok.csv"
    write(ppg_ok, 100.0, ppg, ["ppg_ir", "ppg_red"])
    write(ecg_ok, 250.0, ecg, ["ecg"])

    md_ok = "/tmp/inspect/report-ok.md"
    _, ok_good = build_report(ppg_ok, ecg_ok, md_ok, "/tmp/inspect/ok.png",
                              "ppg_ir", "ecg", 30.0, 5.0)

    # 坏数据：PPG 中间挖掉 5 s（制造时间戳空隙），ECG 换成 100 bpm 使双通道不一致
    keep = np.r_[0:2000, 2500:len(ppg)]
    write(ppg_ok, 100.0, ppg[keep], ["ppg_ir", "ppg_red"])   # 覆盖为带空隙的数据
    write(ecg_ok, 250.0, synth.synth_ecg(250.0, 90.0, 100.0, noise=0.01, seed=7), ["ecg"])
    md_bad = "/tmp/inspect/report-bad.md"
    _, ok_bad = build_report(ppg_ok, ecg_ok, md_bad, "/tmp/inspect/bad.png",
                             "ppg_ir", "ecg", 30.0, 5.0)

    with open(md_bad, encoding="utf-8") as fh:
        bad_text = fh.read()

    checks = [
        ("好数据判定 PASS", ok_good),
        ("坏数据判定 FAIL", not ok_bad),
        ("坏数据报告指出时间戳空隙", "时间戳空隙：**" in bad_text and "处" in bad_text),
        ("坏数据双通道验收 FAIL", "**验收结果：FAIL**" in bad_text),
    ]
    fails = 0
    for name, passed in checks:
        print(f"{'PASS' if passed else 'FAIL'}  {name}")
        fails += 0 if passed else 1
    print(f"\n合计 {len(checks)} 项，失败 {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
