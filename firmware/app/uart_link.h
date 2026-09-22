/*
 * uart_link.h —— USART1（PA9/PA10，115200 8N1）发送通道 + 协议封帧
 *
 * 仅实现设备→上位机方向（P1 验收要的就是把采样与状态发出去）；上位机→设备的 CMD
 * 帧接收属于后续增强，见 firmware/README.md 的待办列表。
 *
 * 发射用轮询（HAL_UART_Transmit，带超时）而不是 DMA：单帧最大 100 ms 一次、每帧几十字节，
 * 115200 下传输时间 < 2 ms，阻塞代价可接受，而且实现简单、失败可见（返回 false）。
 * 若后续带宽吃紧（PPG 100 Hz + ECG 250 Hz 同时上报），再换 DMA + 发送环形缓冲。
 */

#ifndef BIOSIGNAL_UART_LINK_H
#define BIOSIGNAL_UART_LINK_H

#include <stdbool.h>
#include <stdint.h>

bool uart_link_init(void);

/* 发送一帧（type/seq/payload），内部按 protocol.h 封帧并计算 CRC */
bool uart_link_send_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t length);

/* 发送已封好的原始帧字节（调用方用 protocol.h 的编码器生成，避免手写载荷布局） */
bool uart_link_send_raw(const uint8_t *data, uint16_t length);

/* 发送一段文本日志（内部封成 PROTO_TYPE_LOG 帧），用于现场排查 */
bool uart_link_send_log(uint8_t seq, const char *text);

/* 统计：发送失败次数与最近一次 HAL 错误码 */
uint32_t uart_link_tx_failures(void);
uint32_t uart_link_last_error(void);

#endif /* BIOSIGNAL_UART_LINK_H */
