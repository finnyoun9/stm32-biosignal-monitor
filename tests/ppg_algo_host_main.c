/* PPG 算法主机端运行器：把 CSV 里的 PPG 列喂给 C 算法，输出心率/质量结果。
 *
 * 用法：
 *   cc -std=c99 -O2 -I firmware/app tests/ppg_algo_host_main.c firmware/app/ppg_algo.c -lm -o /tmp/ppg_algo_host
 *   /tmp/ppg_algo_host data/synth_hr72.csv ppg_ir 100
 *
 * 输出（供 tests/test_algo_conformance.py 解析）：
 *   RESULT hr=72.10 pi=3.084 beats=35 quality=1 ready=1 samples=3750 rows=3750
 *
 * 说明：这里刻意**不用 strtok_r**——在 `-std=c99` 严格模式下 glibc 会把它藏起来（需要
 * _POSIX_C_SOURCE 才能声明），本机 macOS 能编译、CI 的 Linux 直接报错。手写一个只读的
 * 字段提取函数既跨平台，也没有可重入/破坏原缓冲的问题。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppg_algo.h"

#define LINE_MAX_LEN 4096
#define FIELD_MAX_LEN 128
#define MAX_COLS 32

/* 把第 idx 个（0 基）逗号分隔字段复制到 out；不存在或为空返回 0。只读，不修改 line。 */
static int field_copy(const char *line, int idx, char *out, size_t out_len)
{
    int cur = 0;
    const char *p = line;
    for (;;) {
        const char *start = p;
        while (*p != '\0' && *p != ',' && *p != '\r' && *p != '\n') {
            p++;
        }
        if (cur == idx) {
            const size_t n = (size_t)(p - start);
            if (n == 0 || n + 1 > out_len) {
                return 0;
            }
            memcpy(out, start, n);
            out[n] = '\0';
            return 1;
        }
        if (*p != ',') {
            return 0;
        }
        p++;
        cur++;
    }
}

static int find_column(const char *header, const char *name)
{
    char field[FIELD_MAX_LEN];
    for (int i = 0; i < MAX_COLS; ++i) {
        if (!field_copy(header, i, field, sizeof(field))) {
            return -1;
        }
        if (strcmp(field, name) == 0) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <csv> <column> [fs]\n", argv[0]);
        return 64;
    }
    const char *path = argv[1];
    const char *col = argv[2];
    const float fs = (argc > 3) ? (float)atof(argv[3]) : 100.0f;

    FILE *fh = fopen(path, "r");
    if (fh == NULL) {
        fprintf(stderr, "无法打开 %s\n", path);
        return 66;
    }

    char line[LINE_MAX_LEN];
    if (fgets(line, sizeof(line), fh) == NULL) {
        fclose(fh);
        fprintf(stderr, "空文件\n");
        return 65;
    }
    const int col_idx = find_column(line, col);
    if (col_idx < 0) {
        fclose(fh);
        fprintf(stderr, "列 %s 不存在\n", col);
        return 64;
    }

    ppg_algo_t algo;
    ppg_algo_init(&algo, fs);
    if (!algo.ready) {
        fclose(fh);
        fprintf(stderr, "不支持的采样率 %.1f Hz（支持 50/100/125/200 的 ±10%%）\n", (double)fs);
        return 64;
    }

    char field[FIELD_MAX_LEN];
    uint32_t rows = 0;
    while (fgets(line, sizeof(line), fh) != NULL) {
        if (!field_copy(line, col_idx, field, sizeof(field))) {
            continue;
        }
        ppg_algo_push(&algo, (float)atof(field));
        rows++;
    }
    fclose(fh);

    const ppg_result_t r = ppg_algo_result(&algo);
    printf("RESULT hr=%.2f pi=%.4f beats=%u quality=%d ready=%d samples=%u rows=%u\n",
           (double)r.hr_bpm, (double)r.pi_percent, r.beats, r.quality_ok ? 1 : 0,
           r.ready ? 1 : 0, r.samples, rows);
    return 0;
}
