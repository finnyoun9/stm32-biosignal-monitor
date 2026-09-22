/*
 * power_budget.c —— 低功耗预算的整数实现（无浮点，便于在 MCU 上直接调用与断言）
 *
 * 设计取舍：
 *   - 用整数 µA 而不是浮点 mA：MCU 上无 FPU 时浮点走软浮点，且跨平台舍入行为不一致；
 *     整数运算在 C 与 Python 之间可以做到**逐例完全相同**（见 tests/test_power_budget.py）。
 *   - 只做「预算」，不做「测量」：真实电流必须万用表/功耗探针实测，实测值回填 docs/04 调试记录，
 *     再与这里的预算对照——差异本身就是最好的面试素材（预算 vs 实测为什么差 30%）。
 */

#include "power_budget.h"

power_status_t power_budget_validate(const power_cfg_t *cfg)
{
    if (cfg == 0 || cfg->period_ms == 0u) {
        return POWER_ERR_PERIOD_ZERO;
    }
    if (cfg->on_ms > cfg->period_ms) {
        return POWER_ERR_ON_GT_PERIOD;
    }
    if (cfg->sample_ms > cfg->on_ms) {
        return POWER_ERR_SAMPLE_GT_ON;
    }
    return POWER_OK;
}

uint32_t power_budget_average_ua(const power_cfg_t *cfg)
{
    if (power_budget_validate(cfg) != POWER_OK) {
        return 0u;
    }
    /* mA×100 → µA：除以 100 得 mA，再乘 1000；等价于 ×10 */
    const uint64_t run_ua = (uint64_t)cfg->run_ma_x100 * 10u;
    const uint64_t sensor_ua = (uint64_t)cfg->sensor_ma_x100 * 10u;
    const uint64_t sleep_ua = (uint64_t)cfg->stop_ua + (uint64_t)cfg->rtc_ua;
    const uint64_t sleep_ms = (uint64_t)cfg->period_ms - (uint64_t)cfg->on_ms;

    const uint64_t num = run_ua * cfg->on_ms
                       + sensor_ua * cfg->sample_ms
                       + sleep_ua * sleep_ms;
    return (uint32_t)(num / cfg->period_ms);
}

uint32_t power_budget_duty_permille(const power_cfg_t *cfg)
{
    if (power_budget_validate(cfg) != POWER_OK) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)cfg->on_ms * 1000u) / cfg->period_ms);
}

uint32_t power_budget_life_hours(const power_cfg_t *cfg, uint32_t capacity_mah)
{
    const uint32_t avg_ua = power_budget_average_ua(cfg);
    if (avg_ua == 0u) {
        return 0u;
    }
    /* mAh → µAh：×1000；续航小时 = µAh / µA */
    const uint64_t uah = (uint64_t)capacity_mah * 1000u;
    return (uint32_t)(uah / avg_ua);
}

uint32_t power_budget_max_on_ms(const power_cfg_t *cfg, uint32_t target_avg_ua)
{
    if (power_budget_validate(cfg) != POWER_OK) {
        return 0u;
    }
    const uint64_t run_ua = (uint64_t)cfg->run_ma_x100 * 10u;
    const uint64_t sensor_ua = (uint64_t)cfg->sensor_ma_x100 * 10u;
    const uint64_t sleep_ua = (uint64_t)cfg->stop_ua + (uint64_t)cfg->rtc_ua;

    if (target_avg_ua <= sleep_ua) {
        return 0u;                    /* 目标低于睡眠电流：物理上不可达 */
    }
    const uint64_t active_extra = run_ua + sensor_ua - sleep_ua;
    if (active_extra == 0u) {
        return 0u;
    }
    const uint64_t num = ((uint64_t)target_avg_ua - sleep_ua) * cfg->period_ms;
    const uint64_t on_ms = num / active_extra;
    return (on_ms > cfg->period_ms) ? cfg->period_ms : (uint32_t)on_ms;
}
