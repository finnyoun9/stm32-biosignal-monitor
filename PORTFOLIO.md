# 作品集摘要：STM32 生理信号采集与信号处理

> 一页看完：做了什么、证据在哪、边界在哪。完整仓库见 <https://github.com/finnyoun9/stm32-biosignal-monitor>。

## 一句话定位

在 STM32（Cortex-M3）+ FreeRTOS 上实现 **PPG（光电容积脉搏波）与 ECG（心电）两条生理信号链路的采集与端侧信号处理**：驱动 → 时序 → 数据质量 → 心率/血氧/HRV → 低功耗 → 上位机验证与验收，每一层都有可复现的测试。

## 关键数字（全部可由仓库内命令复现）

| 指标 | 结果 | 复现命令 |
|---|---|---|
| PPG 心率（合成信号，真值已知） | 60/72/90/120 bpm 误差 **≤0.4 bpm** | `tests/test_algorithms.py` |
| PPG 心率（公开真实数据） | PhysioNet BIDMC **8 例 125 窗口：MAE 0.45 bpm**（用同段 ECG 当独立真值） | `tools/validate_bidmc.py --records bidmc01..08` |
| ECG QRS 检测（合成） | 60/75/100 bpm，R 波灵敏度 **1.000** | `tests/test_algorithms.py` |
| 固件 C 实现 vs Python 参考 | PPG 偏差 0.0–1.1 bpm；ECG 偏差 0.0–1.1 bpm | `tests/test_algo_conformance.py`、`test_ecg_conformance.py` |
| 双通道一致性（P3 验收） | 合成 0.0 bpm；**BIDMC 真实配对数据 1.1 bpm**（限值 5）；**含负向对照**（错开 25 bpm 必须判 FAIL） | `tests/test_dual_channel_acceptance.py` |
| 串口协议 | 四类帧 C↔Python **逐字节一致** | `tests/test_protocol_conformance.py` |
| MAX30102 驱动 | 主机端（模拟 I2C 芯片）**10/10**；曾抓到 `TEMP_FRAC` 小数位解析 bug | `tests/test_driver_host.py` |
| 低功耗预算 | 1 分钟测量一次 → **887 µA**、200 mAh 约 **9.4 天**；连续采样对照 22 mA / 9 h；给定 500 µA 反解出每周期最多醒 **896 ms** | `tools/power_budget.py --sweep`、`tests/test_power_budget.py` |
| 工频陷波 | 深度 **−95…−110 dB**、10 Hz 保持 −0.00 dB；**50 Hz 干扰 ≥2×R 波时不陷波检出失败、开陷波恢复正常** | `tests/test_notch.py`、`test_notch_ecg.py` |
| 固件可编译 | `pio run` 通过：RAM **49.1%** / Flash **32.8%**（ARM GCC）；CI 每次 push 重新验证 | `cd firmware && pio run` |
| CI | GitHub Actions 每次 push 自动跑 **11 组**离线验证 + 1 个固件编译任务 | [actions](https://github.com/finnyoun9/stm32-biosignal-monitor/actions) |

## 工程判断（比数字更能说明问题）

1. **用独立真值校验自己的实现，包括校验「真值」本身**：BIDMC 某条记录上 PPG 比 ECG「真值」低 19 bpm，我没有改 PPG，而是先确认 PPG 自相关周期是真的，再查出 ECG 检测器**只更新信号峰估计、噪声峰估计固定**，导致每过一个不应期就误报一个峰；改成 Pan–Tompkins 的双自适应后该记录 MAE 19.33 → 2.49 bpm。
2. **驱动与 HAL 解耦**，用模拟芯片在主机上单测，把「真机上要靠一边加热探头一边读温度才能发现」的 bug 变成毫秒级用例。
3. **验收标准要能证伪**：双通道验收加了负向对照，体检报告用「好数据/坏数据」两组验证——永远 PASS 的验收等于没有验收。
4. **按干扰强度决定要不要上滤波器**：先算清 5–15 Hz 带通本身在 50 Hz 有 −23…−36 dB，再用干扰扫幅证明「强干扰时才必须上陷波」，而不是无脑堆滤波。
5. **明确失败优于静默将就**：不支持的采样率、不支持的陷波组合一律 `ready=false`，因为它们曾经表现为「波形有点怪但结果还能出」这种极难定位的问题。

## 技术栈

C（STM32 HAL）、FreeRTOS V10.3.1、I2C/ADC/DMA/定时器、PlatformIO（`pio run` 可复现编译）；
基础工程约定（时钟/中断别名/串口与 I2C 初始化写法、FreeRTOS 内核）移植自本人的 STM32 环境终端项目（同板型）。
Python（numpy/scipy/matplotlib/wfdb）做算法原型、协议实现、数据验证与上位机。

## 边界（如实说明）

- **真机采集（P1/P2/P3 实测）尚未完成**：目前完成的是算法层（含公开真实数据验证）、驱动主机端验证、协议、上位机与验收工具，以及**可编译的固件**（采集任务/上报链路已就绪）。真机需要 MAX30102（约 10–20 元）与可选 AD8232（约 25 元）模块。
- 血氧只做**方法链路验证，未用参考血氧仪标定**，数值不可信。
- BIDMC 是 **ICU 静息数据**（卧床、心率范围集中、运动伪影少），不等于可穿戴在运动场景下的表现。
- 本项目**不是医疗设备**，不下任何医学结论。

## 目录速览

| 想看什么 | 去哪 |
|---|---|
| 结论与数字 | `README.md`、`docs/05-P0验证结果.md` |
| 参考了哪些开源项目、许可证边界 | `docs/00-参考项目调研.md` |
| 信号处理参数来由 | `docs/03-信号处理笔记.md` |
| 低功耗设计与实测要求 | `docs/06-P2低功耗设计.md` |
| ECG 通道设计 | `docs/07-P3-ECG通道设计.md` |
| 上机排查清单（四类现象 24 条假设） | `docs/08-上机检查单.md` |
| 固件怎么编译、CubeMX 怎么配 | `firmware/README.md`、`hardware/wiring.md` |
