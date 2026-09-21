/*
 * max30102.c —— MAX30102 驱动实现（与 HAL 解耦，可在主机上单测）
 *
 * 容易出错、且本文件重点处理的三个点：
 *   1. FIFO 指针回绕：WR/RD 指针是 5 位（0–31），可用样本数必须按模 32 计算；
 *   2. 溢出：OVF_COUNTER 非 0 说明有样本被覆盖，读之前必须先清指针，否则会把新旧样本混在一起；
 *   3. 采样率与脉宽的组合限制：手册规定 SPO2_CONFIG 的 SR 与 LED_PW 有最大积分时间约束。
 */

#include "max30102.h"

#define REG_MODE_RESET 0x40u
#define REG_MODE_SHDN 0x80u
#define REG_FIFO_ROLLOVER_EN 0x10u
#define REG_FIFO_A_FULL_MASK 0xF0u
#define REG_SPO2_ADC_RGE_4096 0x20u   /* 4096 nA 量程 */
#define REG_SPO2_SR_SHIFT 2u
#define REG_SPO2_PW_MASK 0x03u
#define REG_TEMP_EN_START 0x01u
#define REG_TEMP_INT_SIGN 0x80u

static uint16_t sr_to_hz(max30102_sr_t sr)
{
    static const uint16_t table[8] = {50u, 100u, 200u, 400u, 800u, 1000u, 1600u, 3200u};
    return table[(uint8_t)sr & 0x07u];
}

/* 手册约束：100 sps 时最大积分时间 1181 µs，200 sps 时 591 µs…… 这里做一次最小校验，
 * 避免配置出「脉宽 > 采样周期」的非法组合（表现为 FIFO 永远溢出）。 */
static bool pw_fits_sr(max30102_sr_t sr, max30102_pw_t pw)
{
    static const uint16_t pw_us[4] = {69u, 118u, 215u, 411u};
    const uint16_t period_us = (uint16_t)(1000000u / sr_to_hz(sr));
    return pw_us[(uint8_t)pw & 0x03u] < period_us;
}

static bool write_reg(max30102_t *dev, uint8_t reg, uint8_t value)
{
    return dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, reg, &value, 1u);
}

static bool read_reg(max30102_t *dev, uint8_t reg, uint8_t *value)
{
    return dev->io.read(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, reg, value, 1u);
}

bool max30102_read_part_id(max30102_t *dev, uint8_t *part_id)
{
    return read_reg(dev, MAX30102_REG_PART_ID, part_id);
}

bool max30102_reset(max30102_t *dev)
{
    if (!write_reg(dev, MAX30102_REG_MODE_CONFIG, REG_MODE_RESET)) {
        return false;
    }
    /* 复位后 RESET 位自清；主机测试里 io.now_ms 由外部推进 */
    for (int i = 0; i < 100; ++i) {
        uint8_t v = 0;
        if (!read_reg(dev, MAX30102_REG_MODE_CONFIG, &v)) {
            return false;
        }
        if ((v & REG_MODE_RESET) == 0u) {
            return true;
        }
    }
    return false;
}

bool max30102_set_led_current(max30102_t *dev, uint8_t value)
{
    if (!write_reg(dev, MAX30102_REG_LED1_PA, value)) {
        return false;
    }
    if (!write_reg(dev, MAX30102_REG_LED2_PA, value)) {
        return false;
    }
    dev->led_current = value;
    return true;
}

bool max30102_init(max30102_t *dev, const max30102_io_t *io, uint8_t led_current,
                   max30102_sr_t sr, max30102_mode_t mode)
{
    if (dev == 0 || io == 0 || io->read == 0 || io->write == 0) {
        return false;
    }
    dev->io = *io;
    dev->fifo_overflows = 0;
    dev->samples_read = 0;
    dev->spo2_mode = (mode == MAX30102_MODE_SPO2);
    dev->sample_rate_hz = sr_to_hz(sr);

    if (!pw_fits_sr(sr, MAX30102_PW_411US)) {
        return false;      /* 非法组合：直接拒绝，别让它在真机上表现为「数据全乱」 */
    }
    if (!max30102_reset(dev)) {
        return false;
    }

    uint8_t part = 0;
    if (!max30102_read_part_id(dev, &part) || part != MAX30102_PART_ID) {
        return false;
    }

    /* FIFO：4 样本平均 ×1（不做平均，保留原始波形）+ A_FULL 中断阈值 + 回绕使能 */
    const uint8_t fifo_cfg = (uint8_t)((0u << 5) | (MAX30102_A_FULL << 0) | REG_FIFO_ROLLOVER_EN);
    if (!write_reg(dev, MAX30102_REG_FIFO_CONFIG, fifo_cfg)) {
        return false;
    }

    /* SpO2 配置：ADC 量程 4096nA，SR，脉宽 411µs（18 位分辨率） */
    const uint8_t spo2_cfg = (uint8_t)(REG_SPO2_ADC_RGE_4096 |
                                       ((uint8_t)sr << REG_SPO2_SR_SHIFT) |
                                       (MAX30102_PW_411US & REG_SPO2_PW_MASK));
    if (!write_reg(dev, MAX30102_REG_SPO2_CONFIG, spo2_cfg)) {
        return false;
    }

    /* 模式：SpO2 模式同时点亮 RED 与 IR，FIFO 每样本 6 字节 */
    if (!write_reg(dev, MAX30102_REG_MODE_CONFIG, (uint8_t)mode)) {
        return false;
    }

    if (!max30102_set_led_current(dev, led_current)) {
        return false;
    }

    /* 清指针与中断状态，进入干净状态 */
    const uint8_t zero = 0u;
    if (!dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_FIFO_WR_PTR, &zero, 1u) ||
        !dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_OVF_COUNTER, &zero, 1u) ||
        !dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_FIFO_RD_PTR, &zero, 1u)) {
        return false;
    }
    uint8_t int_status = 0;
    (void)read_reg(dev, MAX30102_REG_INT_STATUS_1, &int_status);
    (void)read_reg(dev, MAX30102_REG_INT_STATUS_2, &int_status);
    return true;
}

uint8_t max30102_available(max30102_t *dev)
{
    uint8_t wr = 0, rd = 0, ovf = 0;
    if (!read_reg(dev, MAX30102_REG_FIFO_WR_PTR, &wr) ||
        !read_reg(dev, MAX30102_REG_FIFO_RD_PTR, &rd) ||
        !read_reg(dev, MAX30102_REG_OVF_COUNTER, &ovf)) {
        return 0;
    }
    wr &= 0x1Fu;
    rd &= 0x1Fu;
    if (ovf != 0u) {
        dev->fifo_overflows += ovf;
        /* 溢出后 RD 指针已失效：手册要求先把两个指针都清零，再从当前位置重新开始 */
        const uint8_t zero = 0u;
        (void)dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_FIFO_WR_PTR, &zero, 1u);
        (void)dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_OVF_COUNTER, &zero, 1u);
        (void)dev->io.write(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_FIFO_RD_PTR, &zero, 1u);
        return 0;
    }
    /* 5 位指针按模 32 求差，避免"回绕后差值为负"这个经典坑 */
    return (uint8_t)((wr + MAX30102_FIFO_DEPTH - rd) % MAX30102_FIFO_DEPTH);
}

uint8_t max30102_read_fifo(max30102_t *dev, uint32_t *red, uint32_t *ir, uint8_t n)
{
    if (n == 0u || n > MAX30102_FIFO_DEPTH) {
        return 0;
    }
    const uint8_t avail = max30102_available(dev);
    const uint8_t want = (avail < n) ? avail : n;
    if (want == 0u) {
        return 0;
    }
    uint8_t raw[3u * 2u * MAX30102_FIFO_DEPTH];
    const uint16_t len = (uint16_t)(6u * want);
    if (!dev->io.read(dev->io.ctx, MAX30102_I2C_ADDR_7BIT, MAX30102_REG_FIFO_DATA, raw, len)) {
        return 0;
    }
    for (uint8_t i = 0; i < want; ++i) {
        const uint8_t *p = &raw[6u * i];
        /* SpO2 模式下 FIFO 顺序为 RED 在前、IR 在后；HR 模式下只有 RED（-1 位无效） */
        const uint32_t r = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        const uint32_t i_val = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | (uint32_t)p[5];
        red[i] = r & 0x3FFFFu;
        ir[i] = dev->spo2_mode ? (i_val & 0x3FFFFu) : 0u;
    }
    dev->samples_read += want;
    return want;
}

bool max30102_read_temperature(max30102_t *dev, int32_t *milli_celsius)
{
    if (!write_reg(dev, MAX30102_REG_TEMP_EN, REG_TEMP_EN_START)) {
        return false;
    }
    uint8_t int_part = 0, frac = 0;
    if (!read_reg(dev, MAX30102_REG_TEMP_INT, &int_part) ||
        !read_reg(dev, MAX30102_REG_TEMP_FRAC, &frac)) {
        return false;
    }
    int32_t value = (int32_t)(int_part & 0x7Fu);
    if (int_part & REG_TEMP_INT_SIGN) {
        value = -value;
    }
    /* 小数部分在 TEMP_FRAC 的**高 4 位**（bit7:4），低 4 位保留；
     * 每 LSB = 0.0625 °C = 62.5 milli-°C。
     * 这个位置是主机端单测抓出来的 bug：最初按低 4 位解析，读出来永远是 .00 °C。 */
    const int32_t frac_milli = (int32_t)(((frac >> 4) & 0x0Fu) * 625) / 10;
    *milli_celsius = value * 1000 + ((int_part & REG_TEMP_INT_SIGN) ? -frac_milli : frac_milli);
    return true;
}
