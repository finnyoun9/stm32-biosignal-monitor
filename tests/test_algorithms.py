#!/usr/bin/env python3
"""P0 算法自测：全部在合成信号（已知真值）上验证，不需要任何硬件。

跑法：
    ./.venv/bin/python tests/test_algorithms.py

为什么用合成信号而不是直接上真机：
    真机一次采集里同时存在「传感器配置错、时序错、算法错」三种可能。
    先用已知真值把算法本身钉住，上机后任何偏差都能归因到采集链路。
"""

from __future__ import annotations

import math
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools import ecg_qrs, ppg_hr, protocol, spo2, synth  # noqa: E402

FS = 125.0
RESULTS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, bool(ok), detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail else ""))


def test_ppg_hr_accuracy() -> None:
    for truth in (60.0, 72.0, 90.0, 120.0):
        raw = synth.synth_ppg(FS, 60.0, truth, noise=0.004, seed=int(truth))
        res = ppg_hr.analyse(raw, FS)
        err = res["hr_bpm"] - truth
        expected_beats = truth * 60.0 / 60.0
        check(f"PPG 心率 {truth:.0f} bpm 误差 ≤3", abs(err) <= 3.0, f"est={res['hr_bpm']:.1f}, err={err:+.1f}")
        check(f"PPG {truth:.0f} bpm 心搏数合理",
              abs(res["n_beats"] - expected_beats) <= max(3, 0.15 * expected_beats),
              f"n={res['n_beats']}, 期望≈{expected_beats:.0f}")
        check(f"PPG {truth:.0f} bpm 质量门限通过", res["quality_ok"],
              f"PI={res['pi_percent']:.2f}%, corr={res['template_corr']:.2f}")


def test_ppg_rejects_flat_signal() -> None:
    flat = np.full(int(FS * 20), 100000.0) + np.random.default_rng(3).standard_normal(int(FS * 20))
    res = ppg_hr.analyse(flat, FS)
    check("PPG 纯噪声不给可信心率", not res["quality_ok"] or math.isnan(res["hr_bpm"]),
          f"hr={res['hr_bpm']}, quality_ok={res['quality_ok']}")


def test_ecg_qrs() -> None:
    for truth in (60.0, 75.0, 100.0):
        ecg = synth.synth_ecg(FS, 30.0, truth, noise=0.01, seed=int(truth))
        peaks = ecg_qrs.ecg_qrs_detect(ecg, FS)
        hr = ecg_qrs.hr_from_peaks(peaks, FS)
        period = 60.0 / truth
        reference = np.array([int(round(b * period * FS)) for b in range(int(30.0 / period))])
        sens = ecg_qrs.sensitivity(reference, peaks, FS)
        check(f"ECG {truth:.0f} bpm 心率误差 ≤2", abs(hr - truth) <= 2.0, f"est={hr:.1f}")
        check(f"ECG {truth:.0f} bpm R 波灵敏度 ≥0.98", sens >= 0.98, f"sens={sens:.3f}")


def test_spo2_ratio() -> None:
    for target_r in (0.5, 0.6, 0.8):
        ir, red = synth.synth_ppg_pair(FS, 30.0, 72.0, target_r=target_r, noise=0.004, seed=21)
        r, _ = spo2.ratio_of_ratios(ir, red, FS, window_s=4.0)
        check(f"SpO2 R 恢复 ({target_r})", abs(r - target_r) <= 0.06, f"R={r:.4f}")
    check("SpO2 标定式单调（R 越小 SpO2 越高）",
          spo2.spo2_from_r_maxim(0.5) > spo2.spo2_from_r_maxim(1.0),
          f"{spo2.spo2_from_r_maxim(0.5):.1f}% > {spo2.spo2_from_r_maxim(1.0):.1f}%")


def test_protocol_roundtrip() -> None:
    samples = [(131071, 65535), (0, 1), (123456, 54321)]
    frame = protocol.ppg_batch(seq=7, ts_ms=123456, samples=samples)
    raw = frame.encode()
    parser = protocol.Parser()
    frames = parser.feed(raw)
    check("协议：整帧往返", len(frames) == 1 and frames[0].seq == 7)
    if frames:
        ts, pairs = frames[0].decode_ppg()
        check("协议：载荷一致", ts == 123456 and pairs == samples, f"ts={ts}, pairs={pairs}")


def test_protocol_robustness() -> None:
    a = protocol.ppg_batch(1, 100, [(10, 20), (30, 40)]).encode()
    b = protocol.log(2, "LED 电流自检失败").encode()
    c = protocol.status(3, 0x1F, 100, True, 4).encode()

    parser = protocol.Parser()
    out = parser.feed(b"\x00\x11\x22" + a[:3])
    out += parser.feed(a[3:] + b + c)
    check("协议：半帧续传 + 前导噪声重同步", len(out) == 3 and [f.seq for f in out] == [1, 2, 3],
          f"frames={[f.seq for f in out]}, discarded={parser.bytes_discarded}")

    bad = bytearray(a)
    bad[8] ^= 0xFF                      # 破坏载荷
    parser2 = protocol.Parser()
    got = parser2.feed(bytes(bad) + b)
    check("协议：CRC 错误被丢弃且后续帧仍可解析",
          len(got) == 1 and got[0].seq == 2 and parser2.crc_errors == 1,
          f"crc_errors={parser2.crc_errors}, frames={[f.seq for f in got]}")

    parser3 = protocol.Parser()
    got3 = parser3.feed(protocol.status(9, 200, 50, False, 17).encode())
    ok = len(got3) == 1 and got3[0].decode_ppg.__self__ is got3[0]
    check("协议：状态帧解析", ok and got3[0].type == protocol.TYPE_STATUS)


def main() -> int:
    print(f"== P0 算法自测（fs={FS} Hz，合成信号真值已知）==")
    test_ppg_hr_accuracy()
    test_ppg_rejects_flat_signal()
    test_ecg_qrs()
    test_spo2_ratio()
    test_protocol_roundtrip()
    test_protocol_robustness()

    failed = [r for r in RESULTS if not r[1]]
    print(f"\n合计 {len(RESULTS)} 项，通过 {len(RESULTS) - len(failed)}，失败 {len(failed)}")
    for name, _, detail in failed:
        print(f"  FAIL: {name} [{detail}]")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
