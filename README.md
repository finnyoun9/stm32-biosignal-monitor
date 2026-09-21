# STM32 Biosignal Monitor（PPG + ECG 采集与信号处理）

> 目标：把「生理信号采集链路」从简历上的空白变成**有代码、有测试、有实测数据、可复现**的工程证据。
> 定位：可穿戴 / 家用健康检测设备的**端侧采集与信号处理**——不是医疗诊断设备，不做算法创新，重点是把**驱动 → 时序 → 数据质量 → 低功耗 → 上位机验证**这条链路自己走通并留下证据。

## 一、这个项目解决什么问题

招聘方在可穿戴/健康类嵌入式岗位（如安克 RTOS 嵌入式、鱼跃医疗穿戴、藏识 AI 穿戴）反复要求：

| JD 要求 | 本项目对应产物 |
|---|---|
| RTOS 多任务、任务间通信、低功耗 | FreeRTOS 任务划分 + 采集/处理/上报三层 + 低功耗模式切换（阶段 2） |
| PPG / IMU 等传感器驱动开发 | MAX30102（I2C）寄存器配置、FIFO 读取、中断/DMA 采集 |
| 采集链路与时序同步、信号质量调优 | 采样率与丢包统计、时间戳对齐、信号质量指标（PI/模板相关） |
| 心率 / 血氧算法端侧落地 | `tools/` 中的 PPG 心率估计与 ratio-of-ratios SpO2 估计，逐行可解释 |
| 示波器 / 逻辑分析仪定位问题 | `docs/04-调试记录模板.md`：每条问题的波形证据与结论 |
| 量产侧问题闭环 | 采集丢包率、长时间稳定性、上电自检与产线测试项（阶段 3） |

## 二、硬件（BOM 与接线见 `hardware/wiring.md`）

| 部件 | 型号 | 用途 | 备注 |
|---|---|---|---|
| 主控 | STM32F103C8T6（蓝板，已有） | 采集与算法 | 也可用现有 F407 最小系统板 |
| PPG 传感器 | MAX30102 模块 | 心率 / 血氧原始 IR+RED | 约 10-20 元 |
| ECG 前端（阶段 3） | AD8232 模块 | 单导联心电 | 约 25 元，需电极 |
| 调试 | ST-Link + USB-TTL | 下载 / 串口 | 已有 |

## 三、分阶段与验收证据

| 阶段 | 内容 | 验收证据 | 状态 |
|---|---|---|---|
| **P0 算法原型（无需硬件）** | 合成信号 + 真实公开数据集上的心率/QRS 算法实现与误差统计 | **28 项算法单测 + 5 项 C/Python 协议一致性 + 10 项驱动主机端单测全部通过；BIDMC 真实数据上 PPG 心率 vs ECG 真值 MAE 0.96 bpm（3 例 45 窗口）** | ✅ **已完成**（详见 `docs/05-P0验证结果.md`） |
| **P1 采集链路真机** | MAX30102 驱动 + FreeRTOS 任务 + 串口协议，上位机实时绘图 | 实机波形截图、采集丢包率、`docs/04` 调试记录（≥3 条问题闭环） | ⏳ 待 Finn 上机 |
| **P2 低功耗与稳定性** | 采样占空比、STOP 模式唤醒、连续运行 2h 数据 | 功耗实测（mA）+ 掉线/丢包统计 | ⏳ |
| **P3 ECG 通道** | AD8232 采集 + QRS 检测，PPG 与 ECG 的 HR 对照 | 双通道同时采集截图 + 两条链路 HR 一致性 | ⏳ |

### P0 实测结果（可复现）

| 验证 | 结果 | 复现命令 |
|---|---|---|
| 算法单测（合成信号，真值已知） | 28/28 通过：PPG 心率误差 ≤0.4 bpm、ECG R 波灵敏度 1.000、SpO2 的 R 恢复误差 ≤0.0008、协议健壮性全通过 | `./.venv/bin/python tests/test_algorithms.py` |
| C / Python 协议字节级一致性 | 3 类帧逐字节一致 + 2 项解析健壮性，5/5 通过 | `./.venv/bin/python tests/test_protocol_conformance.py` |
| MAX30102 驱动（模拟 I2C 芯片） | 10/10 通过；**抓出并修掉温度小数位解析 bug** | `./.venv/bin/python tests/test_driver_host.py` |
| 真实数据（PhysioNet BIDMC，PPG vs 同段 ECG 真值） | **3 例 45 窗口：MAE 0.96 bpm**、RMSE 3.14、P95 3.42，质量门限 45/45；**真实数据暴露并修掉 ECG 阈值自适应 bug（单条记录 MAE 19.33 → 2.49 bpm）** | `./.venv/bin/python tools/validate_bidmc.py --records bidmc01,bidmc02,bidmc03 --quiet` |
| CI | GitHub Actions 每次 push 自动跑前 3 组，首次 success（32 s） | [actions](https://github.com/finnyoun9/stm32-biosignal-monitor/actions) |

> **诚实边界**：P0 证明的是算法与协议的正确性；BIDMC 为 ICU 静息数据，不等价于可穿戴在运动场景下的表现。
> 真机采集（P1）未完成前，简历与对外沟通中不写「采集链路已跑通」。


> **诚实边界（写进简历前必须满足）**：本仓库区分「算法在合成/公开数据上验证」与「真机跑通」。**未上机验证的部分不会出现在简历与招呼里**；上机后由 Finn 回填 `docs/04` 的实测记录，再更新简历。

## 四、目录结构

```
docs/        调研、范围与验收标准、硬件接线、信号处理笔记、调试记录模板
tools/       算法原型、合成信号、串口协议、数据集验证、绘图（Python）
tests/       算法单元测试（合成信号，含已知真值）
firmware/    PlatformIO 固件工程（STM32F103 + FreeRTOS + MAX30102）
hardware/    接线表与供电/安全注意
data/        本地数据（原始数据集不提交，见 .gitignore）
```

## 五、快速开始

```bash
python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt
./.venv/bin/python tests/test_algorithms.py          # 算法自测（无需硬件）
./.venv/bin/python tools/synth.py --hr 72 --seconds 60 --out data/synth_hr72.csv
./.venv/bin/python tools/ppg_hr.py data/synth_hr72.csv
./.venv/bin/python tools/validate_bidmc.py --record bidmc01   # 需联网下载 PhysioNet 数据
```

## 六、参考与致谢

本项目**借鉴而非复制**了以下开源工作，具体借鉴点、许可证与「哪些代码是原创」见 `docs/00-参考项目调研.md`。核心算法均为按论文与公开参考自行实现，未直接拷贝无许可证仓库的代码。

- Maxim/ADI **MAXREFDES117#** 参考设计（心率/血氧算法思路来源）
- TI **TIDA-01580** 可穿戴 ECG+PPG 参考设计（模拟前端与信号链设计思路）
- Robert Fraczkiewicz 的 MAX30102 心率算法（`MAX30102_by_RF`，思路参考）
- `mintisan/awesome-ppg`（PPG 资源索引）
- MilosRasic98/**OpenHRStrap**（MIT，Pan–Tompkins 实时实现参考）
- PhysioNet **BIDMC PPG and ECG** 数据集（配对 PPG/ECG，用于心率估计验证）

## 七、许可证

MIT，见 `LICENSE`。引用的第三方数据集与设计资料遵循其原始许可，不随本仓库分发。
