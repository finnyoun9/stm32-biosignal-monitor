/*
 * protocol.h —— 采集设备与上位机之间的串口帧协议（C 侧实现，与 tools/protocol.py 一一对应）
 *
 * 为什么放在 header-only：这一层要能被**主机端测试**（gcc 直接编译）与**固件**同时使用，
 * 不依赖 HAL、不依赖 FreeRTOS，才能做「同一份格式定义、两种语言实现、字节级一致」的验证。
 * 格式说明见 tools/protocol.py 顶部注释（单一事实来源）。
 *
 * 帧：SOF(0xAA 0x55) | type | seq | len(2, LE) | payload | CRC16(2, LE)
 * CRC：CRC-16/CCITT-FALSE（init=0xFFFF, poly=0x1021），覆盖 type..payload
 */

#ifndef BIOSIGNAL_PROTOCOL_H
#define BIOSIGNAL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_SOF0 0xAAu
#define PROTO_SOF1 0x55u
#define PROTO_MAX_PAYLOAD 512u
#define PROTO_HEADER_LEN 6u   /* SOF(2) + type + seq + len(2) */
#define PROTO_CRC_LEN 2u

#define PROTO_TYPE_PPG_BATCH 0x01u
#define PROTO_TYPE_STATUS 0x02u
#define PROTO_TYPE_LOG 0x03u
#define PROTO_TYPE_ECG_BATCH 0x04u
#define PROTO_TYPE_CMD 0x81u

/* ── CRC ─────────────────────────────────────────────────────────────── */

static inline uint16_t proto_crc16_ccitt(const uint8_t *data, size_t len, uint16_t init)
{
    uint16_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ── 编码 ─────────────────────────────────────────────────────────────── */

/* 把一帧写入 out（容量 out_cap）。返回写入字节数，容量不足返回 0。 */
static inline size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
                                  uint16_t payload_len, uint8_t *out, size_t out_cap)
{
    if (payload_len > PROTO_MAX_PAYLOAD) {
        return 0;
    }
    const size_t total = PROTO_HEADER_LEN + payload_len + PROTO_CRC_LEN;
    if (out_cap < total) {
        return 0;
    }
    out[0] = PROTO_SOF0;
    out[1] = PROTO_SOF1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(payload_len & 0xFFu);
    out[5] = (uint8_t)((payload_len >> 8) & 0xFFu);
    for (uint16_t i = 0; i < payload_len; ++i) {
        out[PROTO_HEADER_LEN + i] = payload[i];
    }
    const uint16_t crc = proto_crc16_ccitt(&out[2], (size_t)4u + payload_len, 0xFFFFu);
    out[PROTO_HEADER_LEN + payload_len] = (uint8_t)(crc & 0xFFu);
    out[PROTO_HEADER_LEN + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    return total;
}

/* PPG_BATCH：ts_ms(4, LE) + N×(ir:3, red:3)。samples 为交错数组 [ir0,red0,ir1,red1,...]。 */
static inline size_t proto_encode_ppg_batch(uint8_t seq, uint32_t ts_ms,
                                            const uint32_t *samples, uint16_t sample_pairs,
                                            uint8_t *out, size_t out_cap)
{
    const uint16_t payload_len = (uint16_t)(4u + 6u * sample_pairs);
    if (payload_len > PROTO_MAX_PAYLOAD) {
        return 0;
    }
    uint8_t payload[PROTO_MAX_PAYLOAD];
    payload[0] = (uint8_t)(ts_ms & 0xFFu);
    payload[1] = (uint8_t)((ts_ms >> 8) & 0xFFu);
    payload[2] = (uint8_t)((ts_ms >> 16) & 0xFFu);
    payload[3] = (uint8_t)((ts_ms >> 24) & 0xFFu);
    for (uint16_t i = 0; i < sample_pairs; ++i) {
        const uint32_t ir = samples[2u * i] & 0x3FFFFu;
        const uint32_t red = samples[2u * i + 1u] & 0x3FFFFu;
        uint8_t *p = &payload[4u + 6u * i];
        p[0] = (uint8_t)(ir & 0xFFu);
        p[1] = (uint8_t)((ir >> 8) & 0xFFu);
        p[2] = (uint8_t)((ir >> 16) & 0xFFu);
        p[3] = (uint8_t)(red & 0xFFu);
        p[4] = (uint8_t)((red >> 8) & 0xFFu);
        p[5] = (uint8_t)((red >> 16) & 0xFFu);
    }
    return proto_encode(PROTO_TYPE_PPG_BATCH, seq, payload, payload_len, out, out_cap);
}

/* ECG_BATCH：ts_ms(4, LE) + N×(ecg:2, LE)。AD8232 走 MCU 的 12 位 ADC，2 字节足够。 */
static inline size_t proto_encode_ecg_batch(uint8_t seq, uint32_t ts_ms,
                                            const uint16_t *samples, uint16_t count,
                                            uint8_t *out, size_t out_cap)
{
    const uint16_t payload_len = (uint16_t)(4u + 2u * count);
    if (payload_len > PROTO_MAX_PAYLOAD) {
        return 0;
    }
    uint8_t payload[PROTO_MAX_PAYLOAD];
    payload[0] = (uint8_t)(ts_ms & 0xFFu);
    payload[1] = (uint8_t)((ts_ms >> 8) & 0xFFu);
    payload[2] = (uint8_t)((ts_ms >> 16) & 0xFFu);
    payload[3] = (uint8_t)((ts_ms >> 24) & 0xFFu);
    for (uint16_t i = 0; i < count; ++i) {
        payload[4u + 2u * i] = (uint8_t)(samples[i] & 0xFFu);
        payload[5u + 2u * i] = (uint8_t)((samples[i] >> 8) & 0xFFu);
    }
    return proto_encode(PROTO_TYPE_ECG_BATCH, seq, payload, payload_len, out, out_cap);
}

/* STATUS：led_current(1) + sample_rate_hz(2, LE) + quality_ok(1) + dropped(2, LE) */
static inline size_t proto_encode_status(uint8_t seq, uint8_t led_current, uint16_t fs_hz,
                                         bool quality_ok, uint16_t dropped,
                                         uint8_t *out, size_t out_cap)
{
    uint8_t payload[6];
    payload[0] = led_current;
    payload[1] = (uint8_t)(fs_hz & 0xFFu);
    payload[2] = (uint8_t)((fs_hz >> 8) & 0xFFu);
    payload[3] = quality_ok ? 1u : 0u;
    payload[4] = (uint8_t)(dropped & 0xFFu);
    payload[5] = (uint8_t)((dropped >> 8) & 0xFFu);
    return proto_encode(PROTO_TYPE_STATUS, seq, payload, sizeof(payload), out, out_cap);
}

/* LOG：UTF-8 文本（固件侧错误/状态） */
static inline size_t proto_encode_log(uint8_t seq, const char *text, uint16_t text_len,
                                      uint8_t *out, size_t out_cap)
{
    return proto_encode(PROTO_TYPE_LOG, seq, (const uint8_t *)text, text_len, out, out_cap);
}

/* ── 流式解析（与 Python 侧 Parser 行为一致） ─────────────────────────── */

typedef struct {
    uint8_t buf[PROTO_MAX_PAYLOAD + PROTO_HEADER_LEN + PROTO_CRC_LEN];
    uint16_t len;          /* 已缓存字节数 */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t bytes_discarded;
} proto_parser_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint16_t payload_len;
    const uint8_t *payload;   /* 指向解析器内部缓冲，下次 feed 前有效 */
} proto_frame_t;

static inline void proto_parser_init(proto_parser_t *p)
{
    p->len = 0;
    p->frames_ok = 0;
    p->crc_errors = 0;
    p->bytes_discarded = 0;
}

/* 丢弃缓冲头部 n 字节 */
static inline void proto_parser_drop(proto_parser_t *p, uint16_t n)
{
    if (n >= p->len) {
        p->len = 0;
        return;
    }
    for (uint16_t i = 0; i + n < p->len; ++i) {
        p->buf[i] = p->buf[i + n];
    }
    p->len = (uint16_t)(p->len - n);
}

/* 从 in[0..n) 中取出最多 max_frames 个完整帧。
 * 返回取出的帧数；每取出一个帧，通过 out_frames[i] 返回（payload 指向 p->buf 内部）。
 * 与 Python 侧一致：遇到坏帧不中断，向后滑动一个字节重新同步。 */
static inline uint16_t proto_parser_feed(proto_parser_t *p, const uint8_t *in, uint16_t n,
                                         proto_frame_t *out_frames, uint16_t max_frames)
{
    uint16_t produced = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (p->len < sizeof(p->buf)) {
            p->buf[p->len++] = in[i];
        } else {
            proto_parser_drop(p, 1);            /* 缓冲满：丢最旧字节，避免死锁 */
            p->buf[p->len++] = in[i];
            p->bytes_discarded++;
        }
        while (p->len >= PROTO_HEADER_LEN) {
            /* 重同步：找到 SOF */
            uint16_t sof = 0xFFFFu;
            for (uint16_t k = 0; k + 1 < p->len; ++k) {
                if (p->buf[k] == PROTO_SOF0 && p->buf[k + 1] == PROTO_SOF1) {
                    sof = k;
                    break;
                }
            }
            if (sof == 0xFFFFu) {
                /* 没有 SOF：保留最后一个字节（可能是 SOF0 的前半） */
                if (p->len > 1) {
                    p->bytes_discarded += (uint32_t)(p->len - 1);
                    proto_parser_drop(p, (uint16_t)(p->len - 1));
                }
                break;
            }
            if (sof > 0) {
                p->bytes_discarded += sof;
                proto_parser_drop(p, sof);
                continue;
            }
            const uint16_t payload_len = (uint16_t)(p->buf[4] | ((uint16_t)p->buf[5] << 8));
            if (payload_len > PROTO_MAX_PAYLOAD) {
                p->bytes_discarded++;
                proto_parser_drop(p, 1);
                continue;
            }
            const uint16_t total = PROTO_HEADER_LEN + payload_len + PROTO_CRC_LEN;
            if (p->len < total) {
                break;                              /* 半帧：等后续字节 */
            }
            const uint16_t crc_rx = (uint16_t)(p->buf[6 + payload_len] |
                                               ((uint16_t)p->buf[7 + payload_len] << 8));
            const uint16_t crc_calc = proto_crc16_ccitt(&p->buf[2], (size_t)4u + payload_len, 0xFFFFu);
            if (crc_rx != crc_calc) {
                p->crc_errors++;
                p->bytes_discarded++;
                proto_parser_drop(p, 1);            /* 滑动一字节重新同步 */
                continue;
            }
            if (produced < max_frames) {
                out_frames[produced].type = p->buf[2];
                out_frames[produced].seq = p->buf[3];
                out_frames[produced].payload_len = payload_len;
                out_frames[produced].payload = &p->buf[PROTO_HEADER_LEN];
                produced++;
            }
            p->frames_ok++;
            proto_parser_drop(p, total);
        }
    }
    return produced;
}

#endif /* BIOSIGNAL_PROTOCOL_H */
