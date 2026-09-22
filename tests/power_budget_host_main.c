/* 低功耗预算的主机端单测：手算期望值 + 非法配置拒绝 + 边界情况
 *
 * 用法：
 *   cc -std=c99 -Wall -Wextra -I firmware/app tests/power_budget_host_main.c firmware/app/power_budget.c -o /tmp/power_host
 *   /tmp/power_host
 *
 * 输出给 tests/test_power_budget.py：
 *   CASE <name> avg_ua=<n> duty=<n> life_h=<n> status=<n>
 */

#include <stdio.h>
#include <string.h>

#include "power_budget.h"

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

/* 输出机器可读行，供 Python 侧比对 */
static void emit(const char *name, const power_cfg_t *cfg, uint32_t battery_mah)
{
    printf("CASE %s avg_ua=%u duty=%u life_h=%u max_on_500=%u status=%d\n",
           name,
           power_budget_average_ua(cfg),
           power_budget_duty_permille(cfg),
           power_budget_life_hours(cfg, battery_mah),
           power_budget_max_on_ms(cfg, 500u),
           (int)power_budget_validate(cfg));
}

int main(void)
{
    /* 用例 A：1 分钟一次、活动 2 s、采样 1 s（本项目的目标配置）
     * 手算：run 20000 µA×2000 ms + sensor 12000 µA×1000 ms + sleep 22 µA×58000 ms
     *       = 40,000,000 + 12,000,000 + 1,276,000 = 53,276,000 → /60000 = 887 µA */
    const power_cfg_t a = {60000u, 2000u, 1000u, 2000u, 1200u, 20u, 2u};
    emit("A_1min", &a, 200u);
    check("A 平均电流 = 887 µA", power_budget_average_ua(&a) == 887u,
          "手算 53,276,000/60,000 = 887.93 → 887");
    check("A 占空比 = 33‰", power_budget_duty_permille(&a) == 33u, "2000/60000×1000 = 33.3");
    check("A 续航(200 mAh) = 225 h", power_budget_life_hours(&a, 200u) == 225u,
          "200 mAh = 200,000 µAh / 887 µA = 225 h ≈ 9.4 天");
    check("A 配置合法", power_budget_validate(&a) == POWER_OK, "ok");

    /* 用例 B：5 分钟一次、活动 5 s、采样 3 s */
    const power_cfg_t b = {300000u, 5000u, 3000u, 2000u, 1200u, 20u, 2u};
    emit("B_5min", &b, 200u);
    check("B 平均电流 = 474 µA", power_budget_average_ua(&b) == 474u,
          "100,000,000+36,000,000+6,490,000=142,490,000 → /300,000 = 474");

    /* 用例 C：连续模式（对照） */
    const power_cfg_t c = {60000u, 60000u, 10000u, 2000u, 1200u, 20u, 2u};
    emit("C_continuous", &c, 200u);
    check("C 连续模式平均 > 15 mA（说明省电的必要性）",
          power_budget_average_ua(&c) > 15000u, "连续采样的代价");

    /* 用例 D：非法配置——活动时长 > 周期 */
    const power_cfg_t d = {1000u, 2000u, 500u, 2000u, 1200u, 20u, 2u};
    emit("D_on_gt_period", &d, 200u);
    check("D 活动 > 周期被拒绝",
          power_budget_validate(&d) == POWER_ERR_ON_GT_PERIOD && power_budget_average_ua(&d) == 0u,
          "错误码 2，平均返回 0");

    /* 用例 E：非法配置——采样时长 > 活动时长 */
    const power_cfg_t e = {60000u, 1000u, 2000u, 2000u, 1200u, 20u, 2u};
    emit("E_sample_gt_on", &e, 200u);
    check("E 采样 > 活动被拒绝",
          power_budget_validate(&e) == POWER_ERR_SAMPLE_GT_ON && power_budget_life_hours(&e, 200u) == 0u,
          "错误码 3，续航返回 0");

    /* 用例 F：周期为 0 */
    const power_cfg_t f = {0u, 0u, 0u, 2000u, 1200u, 20u, 2u};
    emit("F_zero_period", &f, 200u);
    check("F 周期为 0 被拒绝", power_budget_validate(&f) == POWER_ERR_PERIOD_ZERO, "错误码 1");

    /* 用例 G：极低功耗配置（大量睡眠时间） */
    const power_cfg_t g = {600000u, 1000u, 500u, 2000u, 1200u, 20u, 2u};
    emit("G_10min", &g, 200u);
    check("G 平均电流 = 65 µA", power_budget_average_ua(&g) == 65u,
          "20,000,000+6,000,000+13,178,000=39,178,000 → /600,000 = 65");

    /* 用例 I：反解——目标平均 500 µA 时，1 分钟周期最多能醒多久
     * on = (500 − 22) × 60000 / (20000 + 12000 − 22) = 478 × 60000 / 31978 = 896.7 → 896 ms */
    const power_cfg_t i_cfg = {60000u, 2000u, 1000u, 2000u, 1200u, 20u, 2u};
    emit("I_target500", &i_cfg, 200u);
    check("I 目标 500 µA → 最多醒 896 ms/分钟", power_budget_max_on_ms(&i_cfg, 500u) == 896u,
          "478×60000/31978 = 896.7");

    /* 用例 J：目标低于睡眠电流 → 不可达 */
    check("J 目标 10 µA（低于睡眠 22 µA）不可达", power_budget_max_on_ms(&i_cfg, 10u) == 0u, "返回 0");

    /* 用例 H：空指针必须安全（嵌入式里回调传 NULL 是常见错误） */
    check("H NULL 配置安全", power_budget_validate(0) == POWER_ERR_PERIOD_ZERO &&
          power_budget_average_ua(0) == 0u, "不崩溃");

    printf("\n合计 %d 项，失败 %d\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
