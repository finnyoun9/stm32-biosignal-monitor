/*
 * biquad.h —— 二阶节（Direct Form II Transposed）滤波器原语
 *
 * 为什么抽成独立模块：PPG（0.5–4 Hz）与 ECG（5–15 Hz）两条链路的滤波结构完全相同，
 * 只是系数不同。把它做成单一实现，改一处优化（例如后续换定点）两条链路一起受益，
 * 也避免两份「看起来一样但有一处写错」的滤波器代码。
 *
 * 系数由 tools/gen_biquad_coeffs.py 生成，不在这里运行时计算。
 */

#ifndef BIOSIGNAL_BIQUAD_H
#define BIOSIGNAL_BIQUAD_H

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

/* 用长度为 3 的 b/a 数组初始化（a[0] 必须为 1） */
void biquad_init(biquad_t *q, const float b[3], const float a[3]);

/* 处理一个样本。注意是**因果**实现：只依赖当前与历史样本，接入实时流不会有未来信息泄漏。 */
float biquad_step(biquad_t *q, float x);

/* 复位状态（不清系数） */
void biquad_reset(biquad_t *q);

#endif /* BIOSIGNAL_BIQUAD_H */
