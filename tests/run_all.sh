#!/usr/bin/env bash
# 一键跑全部可离线验证的项（无需硬件）。任何一项失败即返回非零。
#
#   ./tests/run_all.sh
#
# 覆盖 11 组：
#   1. 算法单测（合成信号真值）                    tests/test_algorithms.py
#   2. C/Python 协议字节级一致性（PPG/ECG/STATUS/LOG） tests/test_protocol_conformance.py
#   3. MAX30102 驱动主机端单测（模拟芯片）          tests/test_driver_host.py
#   4. C/Python PPG 算法一致性（固件侧实现）        tests/test_algo_conformance.py
#   5. C/Python 低功耗预算一致性                   tests/test_power_budget.py
#   6. C/Python ECG QRS 一致性（固件侧实现）        tests/test_ecg_conformance.py
#   7. 双通道采集自检 + P3 一致性验收               tools/serial_capture.py --selftest
#                                                tests/test_dual_channel_acceptance.py
#   8. 采集体检报告自检（好坏数据区分）            tests/test_inspect_capture.py
#   9. 工频陷波验证（深度/通带/支持范围）          tests/test_notch.py
#  10. 工频陷波在 ECG 上的实际收益                tests/test_notch_ecg.py
#  11. 真实数据验证（PhysioNet BIDMC，需要 data/raw/ 有数据） tools/validate_bidmc.py
set -euo pipefail

cd "$(dirname "$0")/.."
PY="./.venv/bin/python"
if [ ! -x "$PY" ]; then
  echo "未找到 .venv，请先：python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt" >&2
  exit 1
fi

echo "== 1/11 算法单测（Python，合成信号） =="
"$PY" tests/test_algorithms.py

echo
echo "== 2/11 C/Python 协议一致性（四类帧） =="
"$PY" tests/test_protocol_conformance.py

echo
echo "== 3/11 MAX30102 驱动单测（模拟芯片） =="
"$PY" tests/test_driver_host.py

echo
echo "== 4/11 C/Python PPG 算法一致性（固件侧实现） =="
"$PY" tests/test_algo_conformance.py

echo
echo "== 5/11 C/Python 低功耗预算一致性 =="
"$PY" tests/test_power_budget.py

echo
echo "== 6/11 C/Python ECG QRS 一致性（固件侧实现） =="
"$PY" tests/test_ecg_conformance.py

echo
echo "== 7/11 双通道采集自检 + P3 一致性验收 =="
"$PY" tools/serial_capture.py --selftest --out data/selftest-ppg.csv --out-ecg data/selftest-ecg.csv
"$PY" tests/test_dual_channel_acceptance.py

echo
echo "== 8/11 采集体检报告自检 =="
"$PY" tests/test_inspect_capture.py

echo
echo "== 9/11 工频陷波验证（深度/通带/支持范围） =="
"$PY" tests/test_notch.py

echo
echo "== 10/11 工频陷波在 ECG 上的实际收益（强干扰下救回结果） =="
"$PY" tests/test_notch_ecg.py

echo
echo "== 11/11 真实数据验证（BIDMC） =="
RECORDS=""
for f in data/raw/bidmc*.hea; do
  [ -e "$f" ] || continue
  RECORDS="${RECORDS}${RECORDS:+,}$(basename "$f" .hea)"
done
if [ -z "$RECORDS" ]; then
  echo "跳过：data/raw/ 下没有 BIDMC 数据（下载方法见 docs/05）"
else
  "$PY" tools/validate_bidmc.py --records "$RECORDS" --quiet --out data/validation_bidmc.csv
fi

echo
echo "全部离线验证通过 ✅"
