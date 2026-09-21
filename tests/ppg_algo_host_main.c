/* PPG 算法主机端运行器：把 CSV 里的 PPG 列喂给 C 算法，输出心率/质量结果。
 *
 * 用法：
 *   cc -std=c99 -O2 -I firmware/app tests/ppg_algo_host_main.c firmware/app/ppg_algo.c -lm -o /tmp/ppg_algo_host
 *   /tmp/ppg_algo_host data/synth_hr72.csv ppg_ir 100
 *
 * 输出（供 tests/test_algo_conformance.py 解析）：
 *   RESULT hr=72.10 pi=3.084 beats=35 quality=1 samples=3750
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppg_algo.h"

#define MAX_COLS 16
#define LINE_MAX_LEN 4096

static int find_column(const char *header, const char *name)
{
    char buf[LINE_MAX_LEN];
    snprintf(buf, sizeof(buf), "%s", header);
    int idx = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",\r\n", &save); tok != NULL; tok = strtok_r(NULL, ",\r\n", &save)) {
        if (strcmp(tok, name) == 0) {
            return idx;
        }
        idx++;
        if (idx >= MAX_COLS) {
            break;
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

    uint32_t rows = 0;
    while (fgets(line, sizeof(line), fh) != NULL) {
        int idx = 0;
        char *save = NULL;
        const char *value = NULL;
        for (char *tok = strtok_r(line, ",\r\n", &save); tok != NULL; tok = strtok_r(NULL, ",\r\n", &save)) {
            if (idx == col_idx) {
                value = tok;
                break;
            }
            idx++;
        }
        if (value == NULL) {
            continue;
        }
        ppg_algo_push(&algo, (float)atof(value));
        rows++;
    }
    fclose(fh);

    const ppg_result_t r = ppg_algo_result(&algo);
    printf("RESULT hr=%.2f pi=%.4f beats=%u quality=%d ready=%d samples=%u rows=%u\n",
           (double)r.hr_bpm, (double)r.pi_percent, r.beats, r.quality_ok ? 1 : 0,
           r.ready ? 1 : 0, r.samples, rows);
    return 0;
}
