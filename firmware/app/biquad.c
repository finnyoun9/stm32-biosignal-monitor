/*
 * biquad.c —— 二阶节滤波器的单一实现（PPG 与 ECG 共用）
 *
 * 采用转置直接 II 型：状态量只有两个（z1/z2），数值稳定性好，每样本 5 次乘加。
 * 这是嵌入式里最常见的 IIR 结构，MCU 上不需要任何库支持。
 */

#include "biquad.h"

void biquad_init(biquad_t *q, const float b[3], const float a[3])
{
    q->b0 = b[0];
    q->b1 = b[1];
    q->b2 = b[2];
    q->a1 = a[1];
    q->a2 = a[2];
    q->z1 = 0.0f;
    q->z2 = 0.0f;
}

float biquad_step(biquad_t *q, float x)
{
    const float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

void biquad_reset(biquad_t *q)
{
    q->z1 = 0.0f;
    q->z2 = 0.0f;
}
