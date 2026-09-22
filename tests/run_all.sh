#!/usr/bin/env bash
# 一键跑全部 P0 验证（无需硬件）。任何一项失败即返回非零。
#
#   ./tests/run_all.sh
#
# 覆盖：
#   1. 算法单测（合成信号真值）        tests/test_algorithms.py
#   2. C/Python 协议字节级一致性       tests/test_protocol_conformance.py
#   3. MAX30102 驱动主机端单测（模拟芯片）tests/test_driver_host.py
#   4. 真实数据验证（PhysioNet BIDMC，需要 data/raw/ 有数据）  tools/validate_bidmc.py
set -euo pipefail

cd "$(dirname "$0")/.."
PY="./.venv/bin/python"
if [ ! -x "$PY" ]; then
  echo "未找到 .venv，请先：python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt" >&2
  exit 1
fi

echo "== 1/6 算法单测（Python） =="
"$PY" tests/test_algorithms.py

echo
echo "== 2/6 C/Python 协议一致性 =="
"$PY" tests/test_protocol_conformance.py

echo
echo "== 3/6 MAX30102 驱动单测（模拟芯片） =="
"$PY" tests/test_driver_host.py

echo
echo "== 4/6 C/Python PPG 算法一致性（固件侧实现） =="
"$PY" tests/test_algo_conformance.py

echo
echo "== 5/6 C/Python 低功耗预算一致性 =="
"$PY" tests/test_power_budget.py

echo
echo "== 6/6 真实数据验证（BIDMC） =="
RECORDS=""
for f in data/raw/bidmc*.hea; do
  [ -e "$f" ] || continue
  RECORDS="${RECORDS}${RECORDS:+,}$(basename "$f" .hea)"
done
if [ -z "$RECORDS" ]; then
  echo "跳过：data/raw/ 下没有 BIDMC 数据（下载方法见 README）"
else
  "$PY" tools/validate_bidmc.py --records "$RECORDS" --quiet --out data/validation_bidmc.csv
fi

echo
echo "全部 P0 验证通过 ✅"
