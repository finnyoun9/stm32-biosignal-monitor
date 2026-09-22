/*
 * notch.c —— 工频陷波实现（系数表 + 明确失败）
 *
 * 系数由 tools/gen_notch_coeffs.py 生成（scipy.signal.iirnotch, Q=20）。
 * 只支持 50/60 Hz × 125/250 Hz：其余组合返回 ready=false，不用近似系数硬算。
 * 原因见 ppg_algo.c 里的教训——静默用错系数在真机上表现为"波形有点怪但结果还能出"，极难发现。
 */

#include "notch.h"

#include <math.h>
#include <string.h>

typedef struct {
    float fs;
    float f0;
    const float *b;
    const float *a;
} notch_coeff_t;

static const float N50_125_B[3] = {0.940809296f, 1.522261418f, 0.940809296f};
static const float N50_125_A[3] = {1.000000000f, 1.522261418f, 0.881618592f};
static const float N60_125_B[3] = {0.929764280f, 1.844865622f, 0.929764280f};
static const float N60_125_A[3] = {1.000000000f, 1.844865622f, 0.859528560f};
static const float N50_250_B[3] = {0.969531253f, -0.599203267f, 0.969531253f};
static const float N50_250_A[3] = {1.000000000f, -0.599203267f, 0.939062506f};
static const float N60_250_B[3] = {0.963653884f, -0.121016656f, 0.963653884f};
static const float N60_250_A[3] = {1.000000000f, -0.121016656f, 0.927307768f};

static const notch_coeff_t TABLE[] = {
    {125.0f, 50.0f, N50_125_B, N50_125_A},
    {125.0f, 60.0f, N60_125_B, N60_125_A},
    {250.0f, 50.0f, N50_250_B, N50_250_A},
    {250.0f, 60.0f, N60_250_B, N60_250_A},
};

void notch_init(notch_t *n, float fs, float f0)
{
    memset(n, 0, sizeof(*n));
    n->fs = fs;
    n->f0 = f0;
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); ++i) {
        const bool fs_ok = fabsf(fs - TABLE[i].fs) <= 0.10f * TABLE[i].fs;
        const bool f0_ok = fabsf(f0 - TABLE[i].f0) <= 0.10f * TABLE[i].f0;
        if (fs_ok && f0_ok) {
            biquad_init(&n->q, TABLE[i].b, TABLE[i].a);
            n->ready = true;
            return;
        }
    }
    n->ready = false;
}

float notch_step(notch_t *n, float x)
{
    if (!n->ready) {
        return x;      /* 未就绪时直通：调用方应从 ready 判断是否可用，而不是拿到静默错误结果 */
    }
    return biquad_step(&n->q, x);
}
