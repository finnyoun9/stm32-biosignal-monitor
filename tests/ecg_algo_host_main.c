/* ECG 算法主机端运行器：把 CSV 里的 ECG 列喂给 C 算法，输出心率/质量结果。
 *
 * 用法：
 *   cc -std=c99 -O2 -I firmware/app -I tests tests/ecg_algo_host_main.c \
 *      firmware/app/ecg_algo.c firmware/app/biquad.c -lm -o /tmp/ecg_algo_host
 *   /tmp/ecg_algo_host data/synth_ecg75.csv ecg 125
 *
 * 输出（供 tests/test_ecg_conformance.py 解析）：
 *   RESULT hr=75.00 beats=38 quality=1 rr_stable=1 ready=1 samples=9375
 */

#include <stdio.h>
#include <stdlib.h>

#include "csv_util.h"
#include "ecg_algo.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <csv> <column> [fs]\n", argv[0]);
        return 64;
    }
    const char *path = argv[1];
    const char *col = argv[2];
    const float fs = (argc > 3) ? (float)atof(argv[3]) : 125.0f;

    FILE *fh = fopen(path, "r");
    if (fh == NULL) {
        fprintf(stderr, "无法打开 %s\n", path);
        return 66;
    }

    char line[CSV_LINE_MAX];
    if (fgets(line, sizeof(line), fh) == NULL) {
        fclose(fh);
        fprintf(stderr, "空文件\n");
        return 65;
    }
    const int col_idx = csv_find_column(line, col);
    if (col_idx < 0) {
        fclose(fh);
        fprintf(stderr, "列 %s 不存在\n", col);
        return 64;
    }

    ecg_algo_t algo;
    ecg_algo_init(&algo, fs);
    if (!algo.ready) {
        fclose(fh);
        fprintf(stderr, "不支持的采样率 %.1f Hz（支持 125/250 的 ±10%%）\n", (double)fs);
        return 64;
    }

    char field[CSV_FIELD_MAX];
    uint32_t rows = 0;
    while (fgets(line, sizeof(line), fh) != NULL) {
        if (!csv_field_copy(line, col_idx, field, sizeof(field))) {
            continue;
        }
        ecg_algo_push(&algo, (float)atof(field));
        rows++;
    }
    fclose(fh);

    const ecg_result_t r = ecg_algo_result(&algo);
    printf("RESULT hr=%.2f beats=%u quality=%d rr_stable=%d ready=%d samples=%u rows=%u\n",
           (double)r.hr_bpm, r.beats, r.quality_ok ? 1 : 0, r.rr_stable ? 1 : 0,
           r.ready ? 1 : 0, rows, rows);
    return 0;
}
