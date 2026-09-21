/*
 * max30102.h —— MAX30102（PPG / 血氧）驱动，**与 HAL 解耦**
 *
 * 设计取舍：驱动不直接调用 HAL_I2C_*，而是通过 `max30102_io_t` 注入读写函数。
 * 好处是这一层可以在主机上用「模拟芯片」做单元测试（tests/max30102_host_main.c），
 * 真机上再注入 HAL 实现即可——FIFO 指针、溢出处理这类最容易出错的逻辑，不需要真机就能验证。
 *
 * 寄存器与位定义来自 MAX30102 数据手册（Rev.1, 2016）。
 */

#ifndef BIOSIGNAL_MAX30102_H
#define BIOSIGNAL_MAX30102_H

#include <stdbool.h>
#include <stdint.h>

/* ── 寄存器地址 ─────────────────────────────────────────────────────── */
#define MAX30102_REG_INT_STATUS_1 0x00u
#define MAX30102_REG_INT_STATUS_2 0x01u
#define MAX30102_REG_INT_ENABLE_1 0x02u
#define MAX30102_REG_INT_ENABLE_2 0x03u
#define MAX30102_REG_FIFO_WR_PTR 0x04u
#define MAX30102_REG_OVF_COUNTER 0x05u
#define MAX30102_REG_FIFO_RD_PTR 0x06u
#define MAX30102_REG_FIFO_DATA 0x07u
#define MAX30102_REG_FIFO_CONFIG 0x08u
#define MAX30102_REG_MODE_CONFIG 0x09u
#define MAX30102_REG_SPO2_CONFIG 0x0Au
#define MAX30102_REG_LED1_PA 0x0Cu   /* RED */
#define MAX30102_REG_LED2_PA 0x0Du   /* IR  */
#define MAX30102_REG_TEMP_INT 0x1Fu
#define MAX30102_REG_TEMP_FRAC 0x20u
#define MAX30102_REG_TEMP_EN 0x21u
#define MAX30102_REG_REV_ID 0xFEu
#define MAX30102_REG_PART_ID 0xFFu

#define MAX30102_PART_ID 0x15u
#define MAX30102_I2C_ADDR_7BIT 0x57u
#define MAX30102_FIFO_DEPTH 32u
#define MAX30102_A_FULL 17u          /* FIFO 剩余可写空间的中断阈值（手册推荐范围 17–32） */

/* ── 模式 / 采样率 / 脉宽 ───────────────────────────────────────────── */
typedef enum {
    MAX30102_MODE_HR_ONLY = 0x02u,
    MAX30102_MODE_SPO2 = 0x03u,      /* RED + IR */
    MAX30102_MODE_MULTI_LED = 0x07u
} max30102_mode_t;

typedef enum {
    MAX30102_SR_50 = 0x00u,
    MAX30102_SR_100 = 0x01u,
    MAX30102_SR_200 = 0x02u,
    MAX30102_SR_400 = 0x03u,
    MAX30102_SR_800 = 0x04u,
    MAX30102_SR_1000 = 0x05u,
    MAX30102_SR_1600 = 0x06u,
    MAX30102_SR_3200 = 0x07u
} max30102_sr_t;

typedef enum {
    MAX30102_PW_69US = 0x00u,
    MAX30102_PW_118US = 0x01u,
    MAX30102_PW_215US = 0x02u,
    MAX30102_PW_411US = 0x03u
} max30102_pw_t;

/* ── 传输抽象（真机注入 HAL，主机构测试注入模拟芯片） ─────────────── */
typedef struct {
    /* 写寄存器块：addr 为 7 位地址，reg 为首寄存器 */
    bool (*write)(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t len);
    /* 读寄存器块 */
    bool (*read)(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len);
    void *ctx;
    uint32_t now_ms;                 /* 由调用方维护的时间基准，便于主机测试注入假时间 */
} max30102_io_t;

typedef struct {
    max30102_io_t io;
    uint8_t led_current;             /* LED1/LED2 电流寄存器值（0.2 mA / LSB） */
    uint16_t sample_rate_hz;
    uint32_t fifo_overflows;         /* 累计溢出次数（质量指标的一部分） */
    uint32_t samples_read;
    bool spo2_mode;
} max30102_t;

/* ── API ───────────────────────────────────────────────────────────── */

/* 复位 → 校验 PART_ID → 配置 FIFO/模式/采样率/脉宽 → 设置 LED 电流 → 清中断 */
bool max30102_init(max30102_t *dev, const max30102_io_t *io, uint8_t led_current,
                   max30102_sr_t sr, max30102_mode_t mode);

/* 软复位（写 RESET 位后等待自清） */
bool max30102_reset(max30102_t *dev);

bool max30102_read_part_id(max30102_t *dev, uint8_t *part_id);

/* 依据 FIFO_WR/RD 指针差计算可读样本数；同时读取溢出计数并累加 */
uint8_t max30102_available(max30102_t *dev);

/* 读 n 个样本（每样本 RED 与 IR 各 18 位，右对齐）。返回实际读到的样本数。 */
uint8_t max30102_read_fifo(max30102_t *dev, uint32_t *red, uint32_t *ir, uint8_t n);

/* 读片内温度（milli-°C）；分辨率 0.0625 °C */
bool max30102_read_temperature(max30102_t *dev, int32_t *milli_celsius);

/* LED 电流设置（0–255，步进 0.2 mA；写 0x0C/0x0D） */
bool max30102_set_led_current(max30102_t *dev, uint8_t value);

#endif /* BIOSIGNAL_MAX30102_H */
