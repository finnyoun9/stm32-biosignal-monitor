/*
 * ecg_algo.h —— ECG QRS 检测（C 侧，流式 + 因果，给 MCU 用）
 *
 * 与 Python 侧（tools/ecg_qrs.py）的关系：
 *   算法链路一致（Pan–Tompkins 简化版：带通 → 差分 → 平方 → 滑动积分 → 双自适应阈值 + 不应期），
 *   但 Python 侧用 filtfilt（零相位）并带 RR 回检；C 侧为了实时性只做因果滤波、暂不做回检。
 *   两侧在同一批数据上的偏差由 tests/test_ecg_conformance.py 量化。
 *
 * 参数与理由（详见 docs/03-信号处理笔记.md）：
 *   * 带通 5–15 Hz：QRS 主能量区间，压掉 P/T 波（低频）与肌电（高频）
 *   * 差分 + 平方 + 150 ms 滑动积分：把陡峭的 QRS 变成平滑包络
 *   * SPKI/NPKI 双自适应阈值：**两个都要更新**——只更新 SPKI 会让阈值长期偏低，
 *     表现为"每过一个不应期就误报一个峰"（本项目在 BIDMC bidmc03 上真实踩过，MAE 从 19.3 修到 2.5）
 *   * 不应期 200 ms：生理上不可能有 300 bpm
 */

#ifndef BIOSIGNAL_ECG_ALGO_H
#define BIOSIGNAL_ECG_ALGO_H

#include <stdbool.h>
#include <stdint.h>

#include "biquad.h"
#include "notch.h"

#define ECG_MAX_RR 8
#define ECG_MIN_RR_MS 270.0f
#define ECG_MAX_RR_MS 2000.0f
#define ECG_DELAY_MAX 64u          /* 0.2 s @250 Hz 的延迟线，用于回找 R 波位置 */

typedef struct {
    float fs;
    biquad_t hp;                   /* 5 Hz 高通 */
    biquad_t lp;                   /* 15 Hz 低通 */
    notch_t notch;                 /* 可选工频陷波（仅在看原始/宽带波形或干扰过大时启用） */
    bool notch_enabled;

    /* 5 点差分用的历史 */
    float bp_hist[5];

    /* 150 ms 滑动积分（环形缓冲 + 运行和） */
    float integ_buf[ECG_DELAY_MAX];
    uint16_t integ_len;            /* 窗口样本数 */
    uint16_t integ_pos;
    float integ_sum;
    float integ_prev1, integ_prev2;

    /* 延迟线：保存最近的带通样本，用于把检测点对齐回 R 波峰 */
    float delay[ECG_DELAY_MAX];
    uint16_t delay_len;
    uint16_t delay_pos;

    /* 自适应阈值状态 */
    float spki;
    float npki;

    /* 心搏与 RR */
    uint32_t sample_index;
    uint32_t last_peak_index;
    float rr_ms[ECG_MAX_RR];
    uint8_t rr_count;
    uint8_t rr_pos;
    float rr_median_ms;
    float hr_bpm;
    uint32_t beats;

    /* 质量 */
    float mean_abs_bp;
    float last_peak_amp;
    bool rr_stable;
    bool quality_ok;
    bool ready;
} ecg_algo_t;

typedef struct {
    float hr_bpm;
    float rr_median_ms;
    uint32_t beats;
    float last_peak_amp;
    bool rr_stable;
    bool quality_ok;
    bool ready;
} ecg_result_t;

void ecg_algo_init(ecg_algo_t *a, float fs);

/* 带选项初始化：enable_notch=true 时在带通之前串一级工频陷波（mains_hz 取 50 或 60）。
 * 默认 ecg_algo_init() 不开陷波——QRS 检测用 5–15 Hz 带通，本身在 50 Hz 已有 20 dB 以上衰减，
 * 上陷波只在"原始波形通路 / 干扰幅度远大于 QRS"时才有意义（见 docs/03）。 */
void ecg_algo_init_opt(ecg_algo_t *a, float fs, bool enable_notch, float mains_hz);
void ecg_algo_push(ecg_algo_t *a, float sample);
ecg_result_t ecg_algo_result(const ecg_algo_t *a);

#endif /* BIOSIGNAL_ECG_ALGO_H */
