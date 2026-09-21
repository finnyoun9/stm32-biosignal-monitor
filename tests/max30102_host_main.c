/* MAX30102 驱动主机端单测（模拟 I2C 芯片，不需要硬件）
 *
 * 用法：
 *   cc -std=c99 -Wall -Wextra -I firmware/app tests/max30102_host_main.c firmware/app/max30102.c -o /tmp/max30102_host
 *   /tmp/max30102_host
 *
 * 覆盖的是真机上最难查、又最不该靠硬件试错的逻辑：
 *   FIFO 指针模 32 回绕、溢出后的恢复、18 位样本拼接、非法采样率/脉宽组合拒绝、温度换算。
 */

#include <stdio.h>
#include <string.h>

#include "max30102.h"

static int g_fail = 0;
static int g_total = 0;

static void check(const char *name, int ok, const char *detail)
{
    g_total++;
    if (!ok) {
        g_fail++;
    }
    printf("%s  %s  [%s]\n", ok ? "PASS" : "FAIL", name, detail);
}

/* ── 模拟芯片 ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t regs[256];
    uint8_t fifo[6u * MAX30102_FIFO_DEPTH];
    uint16_t fifo_bytes;
    uint32_t reads;
    uint32_t writes;
    uint8_t part_id;
    uint8_t reset_hold;      /* >0 时保持 RESET 位为 1，模拟复位未完成 */
} fake_chip_t;

static bool fake_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
    fake_chip_t *chip = (fake_chip_t *)ctx;
    if (addr != MAX30102_I2C_ADDR_7BIT || len != 1u) {
        return false;
    }
    chip->writes++;
    if (reg == MAX30102_REG_MODE_CONFIG && (data[0] & 0x40u)) {
        /* 复位：清寄存器（保留 PART_ID），RESET 位按 reset_hold 决定是否自清 */
        memset(chip->regs, 0, sizeof(chip->regs));
        chip->regs[MAX30102_REG_PART_ID] = chip->part_id;
        chip->fifo_bytes = 0;
        chip->regs[MAX30102_REG_MODE_CONFIG] = chip->reset_hold ? 0x40u : 0x00u;
        return true;
    }
    chip->regs[reg] = data[0];
    return true;
}

static bool fake_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    fake_chip_t *chip = (fake_chip_t *)ctx;
    chip->reads++;
    if (addr != MAX30102_I2C_ADDR_7BIT) {
        return false;
    }
    if (reg == MAX30102_REG_FIFO_DATA) {
        if (len > chip->fifo_bytes) {
            return false;
        }
        memcpy(data, chip->fifo, len);
        /* 真芯片读完 FIFO 会自动推进 RD 指针（按样本 6 字节） */
        const uint8_t samples = (uint8_t)(len / 6u);
        memmove(chip->fifo, chip->fifo + len, (size_t)(chip->fifo_bytes - len));
        chip->fifo_bytes = (uint16_t)(chip->fifo_bytes - len);
        chip->regs[MAX30102_REG_FIFO_RD_PTR] =
            (uint8_t)((chip->regs[MAX30102_REG_FIFO_RD_PTR] + samples) % MAX30102_FIFO_DEPTH);
        return true;
    }
    if (reg == MAX30102_REG_PART_ID) {
        data[0] = chip->part_id;
        return true;
    }
    for (uint16_t i = 0; i < len; ++i) {
        data[i] = chip->regs[(uint8_t)(reg + i)];
    }
    return true;
}

/* 模拟芯片产出一个样本（RED/IR 各 18 位） */
static void fake_push_sample(fake_chip_t *chip, uint32_t red, uint32_t ir)
{
    uint8_t *p = &chip->fifo[chip->fifo_bytes];
    p[0] = (uint8_t)((red >> 16) & 0x03u);
    p[1] = (uint8_t)((red >> 8) & 0xFFu);
    p[2] = (uint8_t)(red & 0xFFu);
    p[3] = (uint8_t)((ir >> 16) & 0x03u);
    p[4] = (uint8_t)((ir >> 8) & 0xFFu);
    p[5] = (uint8_t)(ir & 0xFFu);
    chip->fifo_bytes = (uint16_t)(chip->fifo_bytes + 6u);
    chip->regs[MAX30102_REG_FIFO_WR_PTR] =
        (uint8_t)((chip->regs[MAX30102_REG_FIFO_WR_PTR] + 1u) % MAX30102_FIFO_DEPTH);
}

static void chip_init(fake_chip_t *chip, uint8_t part_id)
{
    memset(chip, 0, sizeof(*chip));
    chip->part_id = part_id;
    chip->regs[MAX30102_REG_PART_ID] = part_id;
}

int main(void)
{
    /* 1) 正常初始化与寄存器写入 */
    fake_chip_t chip;
    chip_init(&chip, MAX30102_PART_ID);
    max30102_io_t io = {fake_write, fake_read, &chip, 0};
    max30102_t dev;
    const bool init_ok = max30102_init(&dev, &io, 0x33u, MAX30102_SR_100, MAX30102_MODE_SPO2);
    check("初始化成功（PART_ID=0x15）", init_ok, init_ok ? "ok" : "fail");
    char detail[96];
    snprintf(detail, sizeof(detail), "mode=0x%02X sr_cfg=0x%02X led=0x%02X",
             chip.regs[MAX30102_REG_MODE_CONFIG], chip.regs[MAX30102_REG_SPO2_CONFIG],
             chip.regs[MAX30102_REG_LED1_PA]);
    check("写入 SpO2 模式与 LED 电流", chip.regs[MAX30102_REG_MODE_CONFIG] == MAX30102_MODE_SPO2 &&
          chip.regs[MAX30102_REG_LED1_PA] == 0x33u && chip.regs[MAX30102_REG_LED2_PA] == 0x33u, detail);

    /* 2) PART_ID 不匹配必须拒绝（避免把 MAX30100/30105 当 30102 用） */
    fake_chip_t other;
    chip_init(&other, 0x11u);
    max30102_io_t io2 = {fake_write, fake_read, &other, 0};
    max30102_t dev2;
    check("PART_ID 不匹配时拒绝初始化", !max30102_init(&dev2, &io2, 0x33u, MAX30102_SR_100,
                                                       MAX30102_MODE_SPO2), "expect false");

    /* 3) 非法采样率/脉宽组合（3200 sps + 411µs）必须拒绝 */
    fake_chip_t fast;
    chip_init(&fast, MAX30102_PART_ID);
    max30102_io_t io3 = {fake_write, fake_read, &fast, 0};
    max30102_t dev3;
    check("非法 SR/PW 组合被拒绝", !max30102_init(&dev3, &io3, 0x33u, MAX30102_SR_3200,
                                                 MAX30102_MODE_SPO2), "expect false");

    /* 4) 可用样本数：普通与指针回绕两种情况 */
    chip_init(&chip, MAX30102_PART_ID);
    io.ctx = &chip;
    max30102_init(&dev, &io, 0x33u, MAX30102_SR_100, MAX30102_MODE_SPO2);
    chip.regs[MAX30102_REG_FIFO_WR_PTR] = 5u;
    chip.regs[MAX30102_REG_FIFO_RD_PTR] = 0u;
    check("available = 5", max30102_available(&dev) == 5u, "wr=5 rd=0");
    chip.regs[MAX30102_REG_FIFO_WR_PTR] = 2u;
    chip.regs[MAX30102_REG_FIFO_RD_PTR] = 30u;
    const uint8_t wrapped = max30102_available(&dev);
    check("指针回绕 available = 4", wrapped == 4u, "wr=2 rd=30 → (2+32-30)%32");

    /* 5) 溢出：返回 0、累计溢出、三指针清零 */
    chip.regs[MAX30102_REG_OVF_COUNTER] = 3u;
    chip.regs[MAX30102_REG_FIFO_WR_PTR] = 20u;
    const uint8_t after_ovf = max30102_available(&dev);
    snprintf(detail, sizeof(detail), "ovf=%u wr=%u rd=%u accum=%u",
             after_ovf, chip.regs[MAX30102_REG_FIFO_WR_PTR], chip.regs[MAX30102_REG_FIFO_RD_PTR],
             dev.fifo_overflows);
    check("溢出后丢弃并清指针", after_ovf == 0u && dev.fifo_overflows == 3u &&
          chip.regs[MAX30102_REG_FIFO_WR_PTR] == 0u && chip.regs[MAX30102_REG_OVF_COUNTER] == 0u, detail);

    /* 6) FIFO 读取与 18 位拼接（含超过 16 位的值） */
    chip_init(&chip, MAX30102_PART_ID);
    io.ctx = &chip;
    max30102_init(&dev, &io, 0x33u, MAX30102_SR_100, MAX30102_MODE_SPO2);
    const uint32_t reds[3] = {131071u, 1u, 65536u};
    const uint32_t irs[3] = {262143u, 0u, 131072u};
    for (int i = 0; i < 3; ++i) {
        fake_push_sample(&chip, reds[i], irs[i]);
    }
    uint32_t got_red[3] = {0}, got_ir[3] = {0};
    const uint8_t n = max30102_read_fifo(&dev, got_red, got_ir, 3u);
    const bool decode_ok = (n == 3u) && got_red[0] == reds[0] && got_red[1] == reds[1] &&
                           got_red[2] == reds[2] && got_ir[0] == irs[0] && got_ir[2] == irs[2];
    snprintf(detail, sizeof(detail), "n=%u red=[%u,%u,%u] ir=[%u,%u,%u]", n, got_red[0], got_red[1],
             got_red[2], got_ir[0], got_ir[1], got_ir[2]);
    check("FIFO 读取与 18 位解析", decode_ok, detail);

    /* 7) FIFO 空时不返回数据 */
    chip.fifo_bytes = 0;
    chip.regs[MAX30102_REG_FIFO_WR_PTR] = chip.regs[MAX30102_REG_FIFO_RD_PTR];
    check("FIFO 空时返回 0", max30102_read_fifo(&dev, got_red, got_ir, 3u) == 0u, "expect 0");

    /* 8) 温度换算：整数 25 + 小数 0.25 → 25250 milli-°C */
    chip.regs[MAX30102_REG_TEMP_INT] = 25u;
    chip.regs[MAX30102_REG_TEMP_FRAC] = 0x40u;
    int32_t milli = 0;
    const bool temp_ok = max30102_read_temperature(&dev, &milli) && milli == 25250;
    snprintf(detail, sizeof(detail), "milli=%d", milli);
    check("温度换算 25.25°C = 25250", temp_ok, detail);

    printf("\n合计 %d 项，失败 %d\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
