/*
 * ppg_algo.h —— PPG 心率/质量算法（C 侧，**流式 + 因果**，给 MCU 用）
 *
 * 与 Python 侧（tools/ppg_hr.py）的关系：
 *   - 算法思路与参数一致：带通 0.5–4 Hz、自适应阈值峰值检测、RR 中位数出心率、PI 质量指标；
 *   - **但 Python 侧用 filtfilt（零相位，离线）**，MCU 上不可能零相位（需要未来数据），
 *     因此这里改用**因果双二阶级联**（HP 0.5 Hz + LP 4 Hz），会引入固定的群延迟。
 *     两边结果不要求逐拍相同，测试用 bpm 容差比较（见 tests/test_algo_conformance.py）。
 *   - 每秒处理 100 个样本，实测在 Cortex-M3 上占用可忽略（无浮点单元时走软浮点，
 *     P2 若要省电可改 Q15 定点，接口不变）。
 *
 * 使用方法（每来一个样本调用一次）：
 *     ppg_algo_t algo;
 *     ppg_algo_init(&algo, 100.0f);
 *     ppg_algo_push(&algo, ir);
 *     ppg_result_t r = ppg_algo_result(&algo);
 */

#ifndef BIOSIGNAL_PPG_ALGO_H
#define BIOSIGNAL_PPG_ALGO_H

#include <stdbool.h>
#include <stdint.h>

#define PPG_ALGO_MAX_RR 8          /* RR 中位数使用的最近心搏数 */
#define PPG_ALGO_MIN_RR_MS 270.0f  /* 220 bpm */
#define PPG_ALGO_MAX_RR_MS 2000.0f /* 30 bpm */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} ppg_biquad_t;

typedef struct {
    float fs;
    ppg_biquad_t hp;               /* 0.5 Hz 高通 */
    ppg_biquad_t lp;               /* 4 Hz 低通 */

    /* DC 与包络估计：用于自适应阈值 */
    float dc;
    float env;                     /* 幅度包络（带衰减的滑动最大值） */
    float prev_ac;                 /* 上一个带通输出，用于判断局部极大 */

    /* 峰值检测状态 */
    uint32_t sample_index;
    uint32_t last_peak_index;
    float last_peak_amplitude;
    float threshold;               /* 当前自适应阈值 */

    /* RR 与质量 */
    float rr_ms[PPG_ALGO_MAX_RR];
    uint8_t rr_count;
    uint8_t rr_pos;
    float rr_median_ms;
    uint32_t beats;
    uint32_t samples;
    float pi_percent;              /* AC/DC × 100 */
    float ac_pp_est;               /* AC 峰峰值滑动估计（慢速） */
    bool quality_ok;
    bool ready;                    /* 采样率是否受支持；false 时所有输出无效 */
    float hr_bpm;
} ppg_algo_t;

typedef struct {
    float hr_bpm;
    float pi_percent;
    float rr_median_ms;
    uint32_t beats;
    uint32_t samples;
    bool quality_ok;
    bool ready;
} ppg_result_t;

void ppg_algo_init(ppg_algo_t *a, float fs);
void ppg_algo_push(ppg_algo_t *a, float raw);
ppg_result_t ppg_algo_result(const ppg_algo_t *a);

#endif /* BIOSIGNAL_PPG_ALGO_H */
