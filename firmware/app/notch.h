/*
 * notch.h —— 工频陷波（50/60 Hz），用于 ECG 原始/宽带通路
 *
 * 什么时候真的需要它（不要为了"显得专业"而上滤波器）：
 *   1. 要把**原始波形**发给上位机看形态（P 波、ST 段）时；
 *   2. 工频干扰幅度远大于 QRS（-23 dB 不够用）时；
 *   3. QRS 带通放宽到 0.5–40 Hz 做形态分析时。
 * 只跑 QRS 检测（5–15 Hz 带通）时，带通本身在 50 Hz 已有约 -23 dB，陷波增益有限。
 * 实测数据见 docs/03-信号处理笔记.md 的"工频陷波"小节与 tests/test_notch.py。
 *
 * 实现复用 firmware/app/biquad.c 的二阶节，系数由 tools/gen_notch_coeffs.py 生成（Q=20）。
 */

#ifndef BIOSIGNAL_NOTCH_H
#define BIOSIGNAL_NOTCH_H

#include <stdbool.h>

#include "biquad.h"

typedef struct {
    biquad_t q;
    float fs;
    float f0;
    bool ready;
} notch_t;

/* 支持的组合：(50 或 60 Hz) × (125 或 250 Hz)，容差 ±10%；其余组合返回 ready=false */
void notch_init(notch_t *n, float fs, float f0);

/* 处理一个样本 */
float notch_step(notch_t *n, float x);

#endif /* BIOSIGNAL_NOTCH_H */
