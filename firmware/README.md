# 固件工程说明（STM32F103C8T6 + FreeRTOS + MAX30102）

## 当前状态（如实标注）

| 层次 | 文件 | 验证状态 |
|---|---|---|
| 串口帧协议 | `include/protocol.h` | ✅ **主机端已验证**：与 `tools/protocol.py` 逐字节一致（`tests/test_protocol_conformance.py`，5/5 通过） |
| MAX30102 驱动 | `app/max30102.c/.h` | ✅ **主机端已验证**：用模拟 I2C 芯片跑 10 项单测（`tests/test_driver_host.py`），含 FIFO 指针回绕、溢出恢复、18 位拼接、非法 SR/PW 组合拒绝、温度换算 |
| 任务与 HAL 传输层 | `src/main.c` | ⚠️ **未编译验证**：依赖 CubeMX 生成的 `Core/` 与 FreeRTOS 源码，需按下面步骤生成后编译；ProcTask 的算法层为 P1 待移植 |
| 低功耗（P2）、ECG（P3） | — | ⏳ 未开始 |

> 之所以把驱动写成「与 HAL 解耦、可主机单测」，是因为 FIFO 指针与溢出处理这类逻辑在真机上出问题时
> 表现是「波形偶尔错乱」，极难定位；放在主机上测，几十毫秒就能跑完 10 个用例。

## 一、生成 CubeMX 初始化层

在 STM32CubeMX 中新建工程（MCU：STM32F103C8Tx），按下表配置，生成到 `firmware/`（**不要**勾选 "Generate peripheral initialization as a pair of .c/.h files"）：

| 外设 | 配置 | 引脚 |
|---|---|---|
| RCC | HSE = 外部晶振（蓝板 8 MHz） | PD0/PD1 |
| SYS | Debug = Serial Wire | PA13/PA14 |
| I2C1 | Standard / Fast mode（100–400 kHz） | PB6 = SCL，PB7 = SDA |
| USART1 | 115200 8N1，异步 | PA9 = TX，PA10 = RX |
| TIM2 | Prescaler 使能 1 kHz 更新中断 | — |
| GPIO | 状态 LED 输出，命名为 `LED_STATUS` | PC13（蓝板板载 LED） |
| FreeRTOS | CMSIS_V1，时基用 TIM4（不要用 SysTick 与 TIM2 冲突） | — |

生成后目录应为：

```
firmware/
├── Core/Inc, Core/Src     ← CubeMX 生成（main.c 请用本仓库的 src/main.c 覆盖，
│                             或把本仓库 main.c 的内容并入 CubeMX 的 main.c 的 USER CODE 区）
├── lib/FreeRTOS/          ← CubeMX 生成的 FreeRTOS 中间件
├── app/                   ← 本仓库：驱动
├── include/               ← 本仓库：协议
├── src/                   ← 本仓库：应用层 main.c
└── STM32F103C8TX.ld       ← 链接脚本（可从 CubeMX 工程复制）
```

## 二、编译与烧录

```bash
cd firmware
pio run                  # 编译
pio run -t upload        # ST-Link 烧录
pio device monitor -b 115200   # 串口（二进制帧，需用上位机解析）
```

上位机侧解析与绘图（P1 上机后）：

```bash
# 建议先写一个串口抓包脚本，把二进制帧落成 CSV（P1 任务）
./.venv/bin/python tools/plot_capture.py data/raw/capture.csv --out docs/img/capture.png
```

## 三、P1 上机步骤（建议顺序）

1. 只接 I2C：确认 `max30102_init` 通过（否则串口会打 `max30102 init failed` 并闪灯）。
2. 接串口：能看到每秒一次的 STATUS 帧（LED 电流 / 采样率 / 溢出计数）。
3. 手指贴合：看 PPG 原始值随心跳起伏；PI 应从 <0.1% 上升到 >0.5%。
4. 用逻辑分析仪抓 I2C：确认 SCL 频率、每次读 FIFO 的字节数 = 6 × 样本数；**记录在 `docs/04-调试记录.md`**。
5. 连续采集 10 min：看 `fifo_overflows` 是否增长（这是采集链路是否跟得上的直接指标）。
6. 把 `ProcTask` 的算法层按 `tools/ppg_hr.py` 移植成 C，并用同一段数据交叉验证 C 与 Python 的心率结果是否一致。

## 四、已知待办

- [ ] `ProcTask` 的 HR/质量算法 C 实现（与 Python 侧参数保持一致）
- [ ] `ReportTask` 的 PPG_BATCH 批量打包（当前只发心跳日志）
- [ ] 串口抓包与 CSV 落盘脚本（`tools/serial_capture.py`）
- [ ] P2：STOP 模式 + RTC 唤醒的占空比采样
- [ ] P3：AD8232 + ADC/DMA 的 ECG 通道
