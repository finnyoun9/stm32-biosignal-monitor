/*
 * ecg_algo.c —— ECG QRS 检测的流式实现（因果、无动态内存）
 *
 * 系数由 tools/gen_biquad_coeffs.py 生成（--preset ecg：5–15 Hz 带通）。
 * 只支持 125 / 250 Hz（±10%）——不少可穿戴与临床设备用这两档；其余采样率明确失败，
 * 不用近似系数"凑合算"（这是 PPG 侧踩过的坑）。
 */

#include "ecg_algo.h"

#include <math.h>
#include <string.h>

/* ECG 带通 5–15 Hz 系数（tools/gen_biquad_coeffs.py --preset ecg） */
static const float ECG_HP_B_125[3] = {0.837089191f, -1.674178381f, 0.837089191f};
static const float ECG_HP_A_125[3] = {1.000000000f, -1.647459981f, 0.700896781f};
static const float ECG_LP_B_125[3] = {0.091314900f, 0.182629801f, 0.091314900f};
static const float ECG_LP_A_125[3] = {1.000000000f, -0.982405793f, 0.347665395f};

static const float ECG_HP_B_250[3] = {0.914969144f, -1.829938288f, 0.914969144f};
static const float ECG_HP_A_250[3] = {1.000000000f, -1.822694925f, 0.837181651f};
static const float ECG_LP_B_250[3] = {0.027859766f, 0.055719532f, 0.027859766f};
static const float ECG_LP_A_250[3] = {1.000000000f, -1.475480444f, 0.586919508f};

#define INTEG_MS 150.0f
#define REFRACTORY_MS 200.0f
#define THRESHOLD_FACTOR 0.25f
#define SPKI_WEIGHT 0.125f
#define DELAY_MS 200.0f

static bool select_coeffs(float fs, const float **hb, const float **ha,
                          const float **lb, const float **la)
{
    if (fabsf(fs - 125.0f) <= 12.5f) {
        *hb = ECG_HP_B_125; *ha = ECG_HP_A_125; *lb = ECG_LP_B_125; *la = ECG_LP_A_125;
        return true;
    }
    if (fabsf(fs - 250.0f) <= 25.0f) {
        *hb = ECG_HP_B_250; *ha = ECG_HP_A_250; *lb = ECG_LP_B_250; *la = ECG_LP_A_250;
        return true;
    }
    return false;
}

void ecg_algo_init(ecg_algo_t *a, float fs)
{
    memset(a, 0, sizeof(*a));
    a->fs = fs;

    const float *hb = 0, *ha = 0, *lb = 0, *la = 0;
    if (!select_coeffs(fs, &hb, &ha, &lb, &la)) {
        a->ready = false;
        return;
    }
    biquad_init(&a->hp, hb, ha);
    biquad_init(&a->lp, lb, la);

    a->integ_len = (uint16_t)(INTEG_MS * fs / 1000.0f);
    if (a->integ_len < 3u) {
        a->integ_len = 3u;
    }
    if (a->integ_len > ECG_DELAY_MAX) {
        a->integ_len = ECG_DELAY_MAX;
    }
    a->delay_len = (uint16_t)(DELAY_MS * fs / 1000.0f);
    if (a->delay_len > ECG_DELAY_MAX) {
        a->delay_len = ECG_DELAY_MAX;
    }
    a->last_peak_index = 0xFFFFFFFFu;   /* 第一个检测点不做 RR 计算 */
    a->ready = true;
}

static float median_rr(const ecg_algo_t *a)
{
    if (a->rr_count == 0u) {
        return 0.0f;
    }
    float tmp[ECG_MAX_RR];
    memcpy(tmp, a->rr_ms, sizeof(float) * a->rr_count);
    for (uint8_t i = 1; i < a->rr_count; ++i) {
        const float key = tmp[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    const uint8_t mid = (uint8_t)(a->rr_count / 2u);
    return (a->rr_count % 2u) ? tmp[mid] : 0.5f * (tmp[mid - 1u] + tmp[mid]);
}

static void push_rr(ecg_algo_t *a, float rr)
{
    if (rr < ECG_MIN_RR_MS || rr > ECG_MAX_RR_MS) {
        return;
    }
    a->rr_ms[a->rr_pos] = rr;
    a->rr_pos = (uint8_t)((a->rr_pos + 1u) % ECG_MAX_RR);
    if (a->rr_count < ECG_MAX_RR) {
        a->rr_count++;
    }
    a->rr_median_ms = median_rr(a);
    if (a->rr_median_ms > 0.0f) {
        a->hr_bpm = 60000.0f / a->rr_median_ms;
    }
    /* RR 稳定性：任一心搏偏离中位数 >30% 视为不稳定（异位搏动/伪影） */
    a->rr_stable = true;
    for (uint8_t i = 0; i < a->rr_count; ++i) {
        if (fabsf(a->rr_ms[i] - a->rr_median_ms) > 0.3f * a->rr_median_ms) {
            a->rr_stable = false;
            break;
        }
    }
}

void ecg_algo_push(ecg_algo_t *a, float sample)
{
    if (!a->ready) {
        return;
    }

    /* 1) 带通 5–15 Hz */
    float bp = biquad_step(&a->hp, sample);
    bp = biquad_step(&a->lp, bp);

    /* 2) 5 点差分（Pan–Tompkins 原式）：y[n] = (2x[n] + x[n-1] − x[n-3] − 2x[n-4]) × fs/8 */
    a->bp_hist[4] = a->bp_hist[3];
    a->bp_hist[3] = a->bp_hist[2];
    a->bp_hist[2] = a->bp_hist[1];
    a->bp_hist[1] = a->bp_hist[0];
    a->bp_hist[0] = bp;
    const float deriv = (2.0f * a->bp_hist[0] + a->bp_hist[1]
                         - a->bp_hist[3] - 2.0f * a->bp_hist[4]) * a->fs / 8.0f;

    /* 3) 平方 */
    const float squared = deriv * deriv;

    /* 4) 150 ms 滑动积分（环形缓冲维护运行和，O(1)/样本） */
    a->integ_sum -= a->integ_buf[a->integ_pos];
    a->integ_buf[a->integ_pos] = squared;
    a->integ_sum += squared;
    a->integ_pos = (uint16_t)((a->integ_pos + 1u) % a->integ_len);
    const float integ = a->integ_sum / (float)a->integ_len;

    /* 延迟线：保存带通样本，用于把检测点回对齐到 R 波峰 */
    a->delay[a->delay_pos] = bp;
    a->delay_pos = (uint16_t)((a->delay_pos + 1u) % a->delay_len);

    /* 幅度均值（慢速）用于质量评估 */
    a->mean_abs_bp = 0.999f * a->mean_abs_bp + 0.001f * fabsf(bp);

    /* 5) 积分域局部极大 + 双自适应阈值 + 不应期 */
    const bool local_max = (a->integ_prev1 > a->integ_prev2) && (a->integ_prev1 >= integ);
    if (local_max) {
        const float threshold = a->npki + THRESHOLD_FACTOR * (a->spki - a->npki);
        if (a->integ_prev1 > threshold) {
            a->spki = SPKI_WEIGHT * a->integ_prev1 + (1.0f - SPKI_WEIGHT) * a->spki;
            /* 检测点落后 R 波约半个积分窗：在延迟线里回找 |bp| 最大处 */
            const uint32_t refractory_samples = (uint32_t)(REFRACTORY_MS * a->fs / 1000.0f);
            const bool first = (a->last_peak_index == 0xFFFFFFFFu);
            if (first || (a->sample_index - a->last_peak_index) >= refractory_samples) {
                float best = 0.0f;
                uint16_t best_offset = 0u;
                for (uint16_t k = 0; k < a->integ_len && k < a->delay_len; ++k) {
                    const uint16_t idx = (uint16_t)((a->delay_pos + a->delay_len - 1u - k) % a->delay_len);
                    const float mag = fabsf(a->delay[idx]);
                    if (mag > best) {
                        best = mag;
                        best_offset = k;
                    }
                }
                const uint32_t peak_index = a->sample_index - 1u - best_offset;
                if (!first) {
                    const float rr = 1000.0f * (float)(peak_index - a->last_peak_index) / a->fs;
                    push_rr(a, rr);
                }
                a->last_peak_index = peak_index;
                a->last_peak_amp = best;
                a->beats++;
            }
        } else {
            a->npki = SPKI_WEIGHT * a->integ_prev1 + (1.0f - SPKI_WEIGHT) * a->npki;
        }
    }

    a->integ_prev2 = a->integ_prev1;
    a->integ_prev1 = integ;
    a->sample_index++;

    /* 6) 质量：心搏数 + RR 稳定性 + 幅度信噪比（峰幅度 vs 平均绝对幅度） */
    const float snr = (a->mean_abs_bp > 1e-9f) ? (a->last_peak_amp / a->mean_abs_bp) : 0.0f;
    a->quality_ok = (a->beats >= 3u) && a->rr_stable && (snr > 2.0f) && (a->rr_median_ms > 0.0f);
}

ecg_result_t ecg_algo_result(const ecg_algo_t *a)
{
    ecg_result_t r;
    r.hr_bpm = a->hr_bpm;
    r.rr_median_ms = a->rr_median_ms;
    r.beats = a->beats;
    r.last_peak_amp = a->last_peak_amp;
    r.rr_stable = a->rr_stable;
    r.quality_ok = a->quality_ok;
    r.ready = a->ready;
    return r;
}
