/*
 * power_budget.h —— 低功耗占空比与电流预算（整数 µA 运算，可在主机上单测）
 *
 * 为什么单独抽出来：
 *   「低功耗」在简历上最容易变成一句空话（"做了功耗优化"）。这里把它变成**可计算的预算 + 可验证的配置**：
 *   给定测量周期、活动时长、STOP 电流，直接算出平均电流与续航，并在配置非法时明确报错，
 *   而不是等上机测出来才发现"周期设得比活动时长还短"。
 *
 * 单位约定（全部为整数，避免浮点在大批量运算中的不一致）：
 *   * 电流：µA；配置里以 mA×100 输入的字段（run_ma_x100 等）在内部 ×10 换算成 µA
 *   * 时间：ms
 *
 * 平均电流公式：
 *   avg = [ run_ua × on_ms + sensor_ua × sample_ms + (stop_ua + rtc_ua) × (period_ms − on_ms) ] / period_ms
 *   （sensor 只在 sample_ms 内供电；CPU 的 stop 段不重叠）
 *
 * 对应的 Python 镜像：tools/power_budget.py，两者由 tests/test_power_budget.py 做逐例一致性校验。
 */

#ifndef BIOSIGNAL_POWER_BUDGET_H
#define BIOSIGNAL_POWER_BUDGET_H

#include <stdint.h>

typedef enum {
    POWER_OK = 0,
    POWER_ERR_PERIOD_ZERO = 1,      /* 周期为 0 */
    POWER_ERR_ON_GT_PERIOD = 2,     /* 活动时长 > 周期 */
    POWER_ERR_SAMPLE_GT_ON = 3      /* 采样时长 > 活动时长 */
} power_status_t;

typedef struct {
    uint32_t period_ms;        /* 测量周期，例如 60000（1 分钟一次） */
    uint32_t on_ms;            /* 每次唤醒后的活动时长 */
    uint32_t sample_ms;        /* 活动期内传感器/LED 供电时长（≤ on_ms） */
    uint32_t run_ma_x100;      /* MCU 活动电流，mA×100（如 2000 = 20.00 mA） */
    uint32_t sensor_ma_x100;   /* 采样期间传感器+LED 平均电流，mA×100 */
    uint32_t stop_ua;          /* STOP 模式电流，µA */
    uint32_t rtc_ua;           /* RTC/LSE 电流，µA */
} power_cfg_t;

/* 配置合法性检查 */
power_status_t power_budget_validate(const power_cfg_t *cfg);

/* 平均电流（µA）；配置非法时返回 0 */
uint32_t power_budget_average_ua(const power_cfg_t *cfg);

/* 占空比（‰，即 on_ms/period_ms ×1000，向下取整）；配置非法时返回 0 */
uint32_t power_budget_duty_permille(const power_cfg_t *cfg);

/* 由电池容量（mAh）估算续航小时数；配置非法或平均电流为 0 时返回 0 */
uint32_t power_budget_life_hours(const power_cfg_t *cfg, uint32_t capacity_mah);

/* 反解：给定平均电流目标（µA），求每个周期最多能醒多久（ms）。
 * 保守假设：醒来期间传感器/LED 一直供电（sample_ms = on_ms）。
 *   推导：avg = sleep + on × (run + sensor − sleep) / period
 *   由 target 反解 on。
 * 返回 0 表示目标不可达（低于睡眠电流）或配置非法。 */
uint32_t power_budget_max_on_ms(const power_cfg_t *cfg, uint32_t target_avg_ua);

#endif /* BIOSIGNAL_POWER_BUDGET_H */
