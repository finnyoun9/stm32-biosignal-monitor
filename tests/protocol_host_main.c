/* 主机端协议一致性自测（不含 HAL，可用 gcc/clang 直接编译）
 *
 * 用法：
 *   cc -std=c99 -Wall -Wextra -Werror -I firmware/include tests/protocol_host_main.c -o /tmp/proto_host
 *   /tmp/proto_host
 *
 * 输出固定的十六进制帧，供 tests/test_protocol_conformance.py 与 Python 实现逐字节比对。
 */

#include <stdio.h>
#include <string.h>

#include "protocol.h"

static void print_hex(const char *tag, const uint8_t *buf, size_t len)
{
    printf("%s ", tag);
    for (size_t i = 0; i < len; ++i) {
        printf("%02x", buf[i]);
    }
    printf("\n");
}

int main(void)
{
    uint8_t frame[PROTO_MAX_PAYLOAD + PROTO_HEADER_LEN + PROTO_CRC_LEN];

    /* 1) PPG_BATCH：与 Python 用例一致的样本 */
    const uint32_t samples[] = {131071u, 65535u, 0u, 1u, 123456u, 54321u};
    size_t n = proto_encode_ppg_batch(7u, 123456u, samples, 3u, frame, sizeof(frame));
    print_hex("PPG_BATCH", frame, n);

    /* 1b) ECG_BATCH：与 Python 用例一致的样本（含 12 位与边界值） */
    const uint16_t ecg_samples[] = {0u, 2048u, 4095u, 65535u};
    n = proto_encode_ecg_batch(5u, 1000u, ecg_samples, 4u, frame, sizeof(frame));
    print_hex("ECG_BATCH", frame, n);

    /* 2) STATUS */
    n = proto_encode_status(9u, 200u, 50u, false, 17u, frame, sizeof(frame));
    print_hex("STATUS", frame, n);

    /* 3) LOG */
    n = proto_encode_log(2u, "boot ok", 7u, frame, sizeof(frame));
    print_hex("LOG", frame, n);

    /* 4) 解析回归：半帧 + 前导噪声 + CRC 破坏 */
    proto_parser_t parser;
    proto_parser_init(&parser);
    proto_frame_t frames[8];

    uint8_t full[PROTO_MAX_PAYLOAD + PROTO_HEADER_LEN + PROTO_CRC_LEN];
    const size_t full_len = proto_encode_ppg_batch(1u, 100u, (const uint32_t[]){10u, 20u, 30u, 40u},
                                                   2u, full, sizeof(full));
    uint16_t got = 0;
    got += proto_parser_feed(&parser, (const uint8_t *)"\x00\x11\x22", 3u, frames + got, 8u - got);
    got += proto_parser_feed(&parser, full, 3u, frames + got, 8u - got);            /* 半帧 */
    got += proto_parser_feed(&parser, full + 3u, (uint16_t)(full_len - 3u), frames + got, 8u - got);
    printf("RESYNC frames=%u first_seq=%u discarded=%u\n", got,
           got ? frames[0].seq : 0u, parser.bytes_discarded);

    uint8_t broken[PROTO_MAX_PAYLOAD + PROTO_HEADER_LEN + PROTO_CRC_LEN];
    memcpy(broken, full, full_len);
    broken[8] ^= 0xFFu;
    proto_parser_t p2;
    proto_parser_init(&p2);
    uint8_t good[PROTO_MAX_PAYLOAD + PROTO_HEADER_LEN + PROTO_CRC_LEN];
    const size_t good_len = proto_encode_log(2u, "next frame", 10u, good, sizeof(good));
    uint16_t got2 = 0;
    got2 += proto_parser_feed(&p2, broken, (uint16_t)full_len, frames, 8u);
    got2 += proto_parser_feed(&p2, good, (uint16_t)good_len, frames + got2, (uint16_t)(8u - got2));
    printf("CRC_REJECT frames=%u crc_errors=%u\n", got2, p2.crc_errors);

    return 0;
}
