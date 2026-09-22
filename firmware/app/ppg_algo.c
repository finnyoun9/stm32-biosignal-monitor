/*
 * ppg_algo.c —— PPG 心率/质量算法的 C 实现（流式、因果、无动态内存）
 *
 * 滤波系数由 tools/gen_biquad_coeffs.py 生成（2 阶 Butterworth 高通 0.5 Hz + 低通 4 Hz），
 * 只内置 50/100/200 Hz 三档——嵌入式工程里"系数表 + 明确支持的采样率"比运行时算三角函数更可控。
 */

#include "ppg_algo.h"

#include <math.h>
#include <string.h>

#define DC_ALPHA 0.995f          /* DC 估计慢速低通（100 Hz 下时间常数 ≈ 2 s） */
#define ENV_DECAY 0.9995f        /* 包络衰减：约 2 s 内保持量级，跟得上幅度变化 */
#define ENV_RISE 0.02f           /* 包络跟随上升系数 */
#define THRESHOLD_K 0.55f        /* 阈值 = k × 包络（对应 Python 侧 k=0.6 的中位数+MAD 策略） */
#define REFRACTORY_MS 300.0f     /* 不应期：生理上限 200 bpm 留足余量 */
#define PI_AC_ALPHA 0.999f       /* AC 峰峰值估计的衰减 */
#define PI_MIN_PERCENT 0.05f     /* 与 Python 侧 quality 门限一致 */
#define MIN_BEATS_FOR_QUALITY 3u

typedef struct {
    const float *b;
    const float *a;
} coeff_set_t;

/* 由 scipy.signal.butter(2, fc, btype=...) 生成，生成脚本：tools/gen_biquad_coeffs.py */
static const float HP_B_50[3] = {0.956543226f, -1.913086451f, 0.956543226f};
static const float HP_A_50[3] = {1.000000000f, -1.911197067f, 0.914975835f};
static const float LP_B_50[3] = {0.046131802f, 0.092263604f, 0.046131802f};
static const float LP_A_50[3] = {1.000000000f, -1.307285029f, 0.491812237f};

static const float HP_B_100[3] = {0.978030479f, -1.956060958f, 0.978030479f};
static const float HP_A_100[3] = {1.000000000f, -1.955578240f, 0.956543677f};
static const float LP_B_100[3] = {0.013359200f, 0.026718400f, 0.013359200f};
static const float LP_A_100[3] = {1.000000000f, -1.647459981f, 0.700896781f};

static const float HP_B_125[3] = {0.982385439f, -1.964770877f, 0.982385439f};
static const float HP_A_125[3] = {1.000000000f, -1.964460580f, 0.965081174f};
static const float LP_B_125[3] = {0.008826087f, 0.017652173f, 0.008826087f};
static const float LP_A_125[3] = {1.000000000f, -1.717211835f, 0.752516182f};

static const float HP_B_200[3] = {0.988954248f, -1.977908496f, 0.988954248f};
static const float HP_A_200[3] = {1.000000000f, -1.977786484f, 0.978030508f};
static const float LP_B_200[3] = {0.003621682f, 0.007243363f, 0.003621682f};
static const float LP_A_200[3] = {1.000000000f, -1.822694925f, 0.837181651f};

/* 滤波器原语 biquad_init / biquad_step 由 firmware/app/biquad.c 提供（PPG 与 ECG 共用） */

static bool select_coeffs(float fs, coeff_set_t *hp, coeff_set_t *lp)
{
    /* 支持的采样率与各自系数：命中要求 fs 落在 ±10% 内。
     * 早先版本把 75–130 Hz 全部套用 100 Hz 系数，等于「静默算错」——
     * 这在真机上会表现为「滤波后波形怪但心率还能出」，最误导人。现在改为明确失败。 */
    struct {
        float fs;
        const float *hb, *ha, *lb, *la;
    } table[] = {
        {50.0f,  HP_B_50,  HP_A_50,  LP_B_50,  LP_A_50},
        {100.0f, HP_B_100, HP_A_100, LP_B_100, LP_A_100},
        {125.0f, HP_B_125, HP_A_125, LP_B_125, LP_A_125},
        {200.0f, HP_B_200, HP_A_200, LP_B_200, LP_A_200},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (fabsf(fs - table[i].fs) <= 0.10f * table[i].fs) {
            hp->b = table[i].hb; hp->a = table[i].ha;
            lp->b = table[i].lb; lp->a = table[i].la;
            return true;
        }
    }
    (void)fs;
    return false;
}

void ppg_algo_init(ppg_algo_t *a, float fs)
{
    memset(a, 0, sizeof(*a));
    a->fs = fs;
    coeff_set_t hp = {0}, lp = {0};
    if (!select_coeffs(fs, &hp, &lp)) {
        a->ready = false;      /* 采样率不受支持：不静默降级 */
        a->quality_ok = false;
        return;
    }
    a->ready = true;
    biquad_init(&a->hp, hp.b, hp.a);
    biquad_init(&a->lp, lp.b, lp.a);
    a->dc = 0.0f;
    a->env = 0.0f;
    a->prev_ac = 0.0f;
    a->last_peak_index = 0;
    a->rr_median_ms = 0.0f;
    a->hr_bpm = 0.0f;
    a->quality_ok = false;
}

/* 计算 RR 数组的中位数（元素 ≤8，插入排序足够） */
static float median_rr(const ppg_algo_t *a)
{
    if (a->rr_count == 0) {
        return 0.0f;
    }
    float tmp[PPG_ALGO_MAX_RR];
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
    const uint8_t mid = a->rr_count / 2;
    return (a->rr_count % 2u) ? tmp[mid] : 0.5f * (tmp[mid - 1] + tmp[mid]);
}

static void push_rr(ppg_algo_t *a, float rr)
{
    if (rr < PPG_ALGO_MIN_RR_MS || rr > PPG_ALGO_MAX_RR_MS) {
        return;                     /* 明显不可能的 RR 直接丢弃，不污染中位数 */
    }
    a->rr_ms[a->rr_pos] = rr;
    a->rr_pos = (uint8_t)((a->rr_pos + 1u) % PPG_ALGO_MAX_RR);
    if (a->rr_count < PPG_ALGO_MAX_RR) {
        a->rr_count++;
    }
    a->rr_median_ms = median_rr(a);
    if (a->rr_median_ms > 0.0f) {
        a->hr_bpm = 60000.0f / a->rr_median_ms;
    }
}

void ppg_algo_push(ppg_algo_t *a, float raw)
{
    a->samples++;

    /* 1) DC 估计与去直流 */
    a->dc = DC_ALPHA * a->dc + (1.0f - DC_ALPHA) * raw;
    float x = raw - a->dc;

    /* 2) 因果带通：高通 → 低通 */
    x = biquad_step(&a->hp, x);
    x = biquad_step(&a->lp, x);

    /* 3) 幅度包络与 AC 峰峰值估计（PI 用） */
    const float mag = fabsf(x);
    if (mag > a->env) {
        a->env += ENV_RISE * (mag - a->env);
    } else {
        a->env *= ENV_DECAY;
    }
    if (mag > a->ac_pp_est) {
        a->ac_pp_est += ENV_RISE * (mag - a->ac_pp_est);
    } else {
        a->ac_pp_est *= PI_AC_ALPHA;
    }
    a->threshold = THRESHOLD_K * a->env;
    if (a->dc != 0.0f) {
        /* AC 峰峰值 ≈ 2×包络（单边幅度 × 2），与 Python 的 99%−1% 分位数量纲一致 */
        a->pi_percent = 100.0f * (2.0f * a->ac_pp_est) / fabsf(a->dc);
    }

    /* 4) 峰值检测：局部极大 + 超过自适应阈值 + 过了不应期 */
    const uint32_t refractory_samples = (uint32_t)(REFRACTORY_MS * a->fs / 1000.0f);
    const bool is_local_max = (a->prev_ac > 0.0f) && (x <= a->prev_ac) && (a->prev_ac > a->threshold);
    if (is_local_max && (a->sample_index - a->last_peak_index) >= refractory_samples) {
        if (a->last_peak_index > 0u) {
            const float rr_ms = 1000.0f * (float)(a->sample_index - a->last_peak_index) / a->fs;
            push_rr(a, rr_ms);
        }
        a->last_peak_index = a->sample_index;
        a->last_peak_amplitude = a->prev_ac;
        a->beats++;
    }

    a->prev_ac = x;
    a->sample_index++;

    /* 5) 质量判定：信号强度 + 心搏数 + RR 稳定性（替代 Python 侧的模板相关性，成本低得多） */
    bool stable = true;
    if (a->rr_count >= 3u) {
        float mean = 0.0f;
        for (uint8_t i = 0; i < a->rr_count; ++i) {
            mean += a->rr_ms[i];
        }
        mean /= (float)a->rr_count;
        for (uint8_t i = 0; i < a->rr_count; ++i) {
            /* RR 偏离中位数 > 30% 视为不稳定（运动伪影的典型表现） */
            if (fabsf(a->rr_ms[i] - a->rr_median_ms) > 0.3f * a->rr_median_ms) {
                stable = false;
                break;
            }
        }
        (void)mean;
    }
    a->quality_ok = (a->pi_percent > PI_MIN_PERCENT) && (a->beats >= MIN_BEATS_FOR_QUALITY) && stable;
}

ppg_result_t ppg_algo_result(const ppg_algo_t *a)
{
    ppg_result_t r;
    r.hr_bpm = a->hr_bpm;
    r.pi_percent = a->pi_percent;
    r.rr_median_ms = a->rr_median_ms;
    r.beats = a->beats;
    r.samples = a->samples;
    r.quality_ok = a->quality_ok;
    r.ready = a->ready;
    return r;
}
