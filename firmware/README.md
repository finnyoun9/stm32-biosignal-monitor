# 固件工程说明（STM32F103C8T6 + FreeRTOS + MAX30102）

## 一、当前状态（如实标注）

| 层次 | 文件 | 验证状态 |
|---|---|---|
| 串口帧协议 | `include/protocol.h` | ✅ **主机端已验证**：与 `tools/protocol.py` 逐字节一致（四类帧，`tests/test_protocol_conformance.py` 7/7） |
| MAX30102 驱动 | `app/max30102.c/.h` | ✅ **主机端已验证**：模拟 I2C 芯片跑 10 项单测（FIFO 5 位指针回绕、溢出恢复、18 位拼接、非法 SR/PW 拒绝、温度换算） |
| PPG 心率/质量算法 | `app/ppg_algo.c/.h` | ✅ **主机端已验证**：与 Python 同一批数据偏差 0.0 bpm（合成）/ 0.5–1.1 bpm（BIDMC 真实数据） |
| ECG QRS 检测 | `app/ecg_algo.c/.h` | ✅ **主机端已验证**：合成偏差 0.0 bpm、BIDMC 真实 II 导联 0.0–1.1 bpm |
| 工频陷波 | `app/notch.c/.h` | ✅ 深度 −95…−110 dB；强干扰下从「检出失败」救回正确心率（`tests/test_notch_ecg.py`） |
| I2C 抽象 / 串口通道 | `app/env_i2c.c`、`app/uart_link.c` | ✅ **随固件编译通过**（移植自 `stm32-smart-home-ota`，另加 MAX30102 需要的寄存器语义读写） |
| 板级初始化（时钟/LED/时基） | `src/board.c` | ✅ **随固件编译通过**（64 MHz、PB6/PB7 = I2C1、PA9/PA10 = USART1、TIM2 = 1 kHz 时间戳） |
| 任务与主流程 | `src/main.c` | ✅ **ARM GCC 编译通过**：RAM 48.7%（9,972 B / 20 KB）、Flash 32.3%（21,144 B / 64 KB）。⏳ 真机采集待模块到手 |
| ECG 的 ADC/DMA 采集 | — | ⏳ P3 待做（算法与陷波已就绪，缺 ADC+TIM+DMA 采样与接线） |
| 低功耗（STOP+RTC 占空比） | — | ⏳ P2 待做（预算与反算已验证，缺上机实测） |

> 之所以把驱动写成「与 HAL 解耦、可主机单测」，是因为 FIFO 指针与溢出处理这类逻辑在真机上出问题时
> 表现是「波形偶尔错乱」，极难定位；放在主机上测，几十毫秒就能跑完 10 个用例。

## 二、与前身项目的关系（复用与移植）

本工程复用了 `stm32-smart-home-ota`（同为 STM32F103C8T6 蓝板）里已验证的基础设施，**两个项目保持独立仓库**：

| 复用/移植的内容 | 来源 | 改动 |
|---|---|---|
| FreeRTOS 内核 V10.3.1（`lib/FreeRTOS/`，MIT） | 前身项目 vendored 副本 | 仅调小堆：14 KB → 7 KB（本项目任务栈更小，20 KB SRAM 需留余量） |
| `FreeRTOSConfig.h` | 前身项目 | 同上调整 + 注释说明 |
| I2C1 初始化与 GPIO 配置（PB6/PB7，100 kHz） | `env_i2c.c` | 增加寄存器语义 `mem_read/mem_write`（MAX30102 需要 repeated-start）与错误码记录 |
| 时钟配置（8 MHz HSE → 64 MHz）、LED、SysTick/FreeRTOS 中断别名写法 | `application/Core/Src/main.c` | 抽出到 `src/board.c`，另加 TIM2 毫秒时间戳 |
| USART1 初始化与 MspInit（PA9/PA10） | `uart_comm.c` | 简化为发送方向（P1 只需要设备→上位机），去掉 DMA 接收 |
| 工具链与工程约定（PlatformIO + `framework=stm32cube` + ST-Link 上传） | `platformio.ini` | 见下：不再需要 CubeMX 生成物 |

**不再需要 STM32CubeMX**：HAL 初始化全部在本仓库代码里，HAL 配置头用 PlatformIO `stm32cube` 框架自带的默认版本。
这样「能不能编译」成了仓库里可复现的一件事，而不是依赖本机某个 CubeMX 工程。

## 三、编译与烧录

```bash
cd firmware
pio run              # 编译（本仓库实测：RAM 48.7% / Flash 32.3%）
pio run -t upload     # ST-Link 烧录
pio device monitor -b 115200   # 串口（二进制帧，请用下面的上位机解析）
```

CI 里也有这一步（`.github/workflows/p0-verify.yml` 的 `firmware-build` 任务），
所以「固件可编译」是被持续验证的，而不是一次性结论。

## 四、上位机侧

```bash
# 采集双通道（PPG / ECG）→ 两个 CSV，含丢包率统计
./.venv/bin/python tools/serial_capture.py --port /dev/tty.usbserial-XXXX --seconds 600 \
    --out data/raw/capture-ppg.csv --out-ecg data/raw/capture-ecg.csv

# 一页体检报告（丢包空隙 / PI / 心率 / 双通道偏差 / 波形图）
./.venv/bin/python tools/inspect_capture.py --ppg data/raw/capture-ppg.csv \
    --ecg data/raw/capture-ecg.csv --out docs/img/capture-report.md
```

## 五、P1 上机步骤（模块到手后照做）

1. 只接 I2C：串口应看到 `boot: biosignal monitor`，随后每秒一帧 STATUS（LED 电流/采样率/溢出计数）。
   若打印 `max30102 init failed: i2c_err=…` → 照 `docs/08-上机检查单.md` 现象 1 的 7 条逐项排查。
2. 手指贴合：看 PI（灌注指数）从 <0.1% 上升到 >0.5%，心率日志开始稳定输出。
3. 用逻辑分析仪抓 I2C：确认 SCL 频率、每次读 FIFO 的字节数 = 6 × 样本数；记进 `docs/04-调试记录.md`。
4. 连续采集 10 min：看 STATUS 帧里的 `fifo_overflows` 是否增长（FIFO 深度 32 @100 Hz → 320 ms 必须读一次，
   当前 100 ms 读一次，留了 3 倍余量）。
5. 按 `docs/09-实测回填.md` 填 9 个数字。

## 六、已知待办

- [ ] USART1 的 CMD 帧接收（上位机 → 设备：开始/停止/改采样率）
- [ ] ECG 的 ADC + TIM 触发 + DMA 采集（算法与陷波已就绪）
- [ ] P2：STOP 模式 + RTC 唤醒的占空比采样（设计见 `docs/06-P2低功耗设计.md`）
- [ ] PPG_BATCH 原始波形上报（当前只上报心率/质量与状态；原始波形上报用于上位机画图与体检报告）
