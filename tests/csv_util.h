/* CSV 字段提取工具（供各主机端测试共用）
 *
 * 刻意不用 strtok_r：`-std=c99` 严格模式下 glibc 不声明它（需要 _POSIX_C_SOURCE），
 * 会导致「本机 macOS 能编译、CI 的 Linux 报错」。这个只读实现跨平台且无副作用。
 */

#ifndef BIOSIGNAL_CSV_UTIL_H
#define BIOSIGNAL_CSV_UTIL_H

#include <stdio.h>
#include <string.h>

#define CSV_LINE_MAX 4096
#define CSV_FIELD_MAX 128
#define CSV_MAX_COLS 32

/* 把第 idx 个（0 基）逗号分隔字段复制到 out；不存在或为空返回 0。只读，不修改 line。 */
static inline int csv_field_copy(const char *line, int idx, char *out, size_t out_len)
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

static inline int csv_find_column(const char *header, const char *name)
{
    char field[CSV_FIELD_MAX];
    for (int i = 0; i < CSV_MAX_COLS; ++i) {
        if (!csv_field_copy(header, i, field, sizeof(field))) {
            return -1;
        }
        if (strcmp(field, name) == 0) {
            return i;
        }
    }
    return -1;
}

#endif /* BIOSIGNAL_CSV_UTIL_H */
