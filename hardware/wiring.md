# 接线表（MAX30102 + AD8232 + STM32）

> 两个板型都给：**F103C8T6 蓝板**（默认，P1 阶段）与 **F407VET6 最小系统板**（DMA/定时器更充裕）。
> 改线之前先看 `docs/02-硬件选型与接线.md` 的电源与安全注意。

## 一、MAX30102（PPG，I2C1）

| MAX30102 模块 | F103C8T6 | F407VET6 | 说明 |
|---|---|---|---|
| VIN | 3.3V | 3.3V | 模块自带稳压；**不要接 5V** |
| GND | GND | GND | 与 MCU 共地 |
| SCL | PB6 (I2C1_SCL) | PB6 (I2C1_SCL) | 模块一般自带 4.7k 上拉 |
| SDA | PB7 (I2C1_SDA) | PB7 (I2C1_SDA) | 同上 |
| INT | PA0 (EXTI0) | PA0 (EXTI0) | FIFO 几乎满/新样本中断，用于确定性读取 |
| (可选) RD | — | — | 部分模块无此脚 |

I2C 地址：7 位 **0x57**（写 0xAE / 读 0xAF）。

## 二、AD8232（ECG，模拟，P3 阶段）

| AD8232 模块 | F103C8T6 | F407VET6 | 说明 |
|---|---|---|---|
| 3.3V | 3.3V | 3.3V | |
| GND | GND | GND | |
| OUTPUT | PA1 (ADC1_IN1) | PA1 (ADC1_IN1) | 模拟输出 → ADC |
| LO+ | PA2 | PA2 | 导联脱落检测（高=脱落） |
| LO− | PA3 | PA3 | 同上 |
| SDN | 3.3V（或 PA4 控制） | 同 | 拉低为关断，省电 |
| 电极 | RA / LA / RL | 同 | 电极片贴放见模块说明；**仅电池供电** |

采集要求：**定时器触发 ADC + DMA**，采样率 250–500 Hz（QRS 带宽约 5–40 Hz，工程上取 250 Hz 以上）。

## 三、串口（上位机）

| USB-TTL | F103C8T6 | F407VET6 | 说明 |
|---|---|---|---|
| RXD | PA9 (USART1_TX) | PA9 (USART1_TX) | 交叉连接 |
| TXD | PA10 (USART1_RX) | PA10 (USART1_RX) | |
| GND | GND | GND | |

参数：**115200 8N1**，二进制帧协议（见 `tools/protocol.py` 与 `firmware/include/protocol.h`）。

## 四、供电与安全

- 开发阶段用 ST-Link 的 3.3V 或独立 LDO；**ECG 人体接触测试只用电池**。
- MAX30102 的 LED 脉冲电流会造成电源纹波：模块旁并 10 µF + 0.1 µF，地线尽量短、单点接地到 MCU 的 GND。
- 所有信号线（尤其 ADC 输入）远离 PWM/电机等大电流走线；本项目的电机相关代码不要与采集同时跑（避免共地噪声污染）。

## 五、接线完成后的自检顺序

1. 只接 MAX30102：I2C 扫描到 0x57 → 读 Part ID → 打印 STATUS 帧。
2. 加串口：上位机 `tools/plot_capture.py` 能画出随手指起伏的波形。
3. 加 AD8232（先不贴电极，用手触碰输入端看 50 Hz 干扰）：确认 ADC 通路正常。
4. 贴电极采集：确认 QRS 可辨识 → 与 PPG 心率对照。
