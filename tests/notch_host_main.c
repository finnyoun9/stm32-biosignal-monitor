/* 工频陷波的主机端验证：用 Goertzel 量测「50 Hz 衰减多少、10 Hz 保留多少」。
 *
 * 为什么用 Goertzel 而不是 FFT：只关心两个已知频点的幅度，Goertzel 一次 O(N)、无需库。
 * 为什么衰减用「输出/输入同频幅度比」：这是滤波器在稳态下的真实增益，能直接对上 Python 侧（scipy）的结果。
 *
 * 用法：
 *   cc -std=c99 -O2 -I firmware/app tests/notch_host_main.c firmware/app/notch.c firmware/app/biquad.c -lm -o /tmp/notch_host
 *   /tmp/notch_host 250 50
 *
 * 输出：
 *   RESULT fs=250 f0=50 ready=1 atten_50_db=-84.13 atten_10_db=-0.02
 */

#include <math.h>

/* M_PI 不是 C 标准的一部分：macOS 的 math.h 默认暴露它，glibc 在 -std=c99 严格模式下不暴露，
 * 于是本机编译通过、CI（Linux）报 "M_PI undeclared"。自己定义一份，跨平台一致。 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>

#include "notch.h"

/* Goertzel：返回频率 f 处的幅度（对整数个周期近似；这里用 1 s 窗，f 为整数 Hz 时精确） */
static double goertzel_mag(const double *x, int n, double fs, double f)
{
    const double w = 2.0 * M_PI * f / fs;
    const double cw = cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * cw;
    const double imag = s2 * sin(w);
    return sqrt(real * real + imag * imag) / (n / 2.0);
}

/* 生成正弦、过陷波、丢弃瞬态，返回稳态段的同频幅度 */
static double measure(double fs, double f, double f0, double *out)
{
    const int n = (int)(4.0 * fs);          /* 4 s */
    const int skip = (int)(1.0 * fs);       /* 丢弃前 1 s 瞬态 */
    static double buf_in[4096];
    static double buf_out[4096];
    if (n > 4096 || n - skip > 4096) {
        fprintf(stderr, "采样率过高，缓冲区不足\n");
        exit(64);
    }
    notch_t nf;
    notch_init(&nf, (float)fs, (float)f0);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        const double x = sin(2.0 * M_PI * f * t);
        buf_in[i] = x;
        buf_out[i] = nf.ready ? (double)notch_step(&nf, (float)x) : x;
    }
    const int m = n - skip;
    const double mag_in = goertzel_mag(buf_in + skip, m, fs, f);
    const double mag_out = goertzel_mag(buf_out + skip, m, fs, f);
    if (out != 0) {
        out[0] = mag_in;
        out[1] = mag_out;
    }
    return 20.0 * log10(mag_out / mag_in);
}

int main(int argc, char **argv)
{
    const double fs = (argc > 1) ? atof(argv[1]) : 250.0;
    const double f0 = (argc > 2) ? atof(argv[2]) : 50.0;

    notch_t probe;
    notch_init(&probe, (float)fs, (float)f0);

    const double a50 = measure(fs, f0, f0, 0);          /* 干扰频率处 */
    const double a10 = measure(fs, 10.0, f0, 0);        /* QRS 频段内（10 Hz） */

    printf("RESULT fs=%.0f f0=%.0f ready=%d atten_%d_db=%.2f atten_10_db=%.2f\n",
           fs, f0, probe.ready ? 1 : 0, (int)f0, a50, a10);
    return 0;
}
