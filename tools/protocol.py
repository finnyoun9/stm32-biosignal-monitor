#!/usr/bin/env python3
"""串口帧协议：上位机与固件共用一份定义（单一事实来源）。

为什么不用直接打印文本：真机采集要求**可验证的完整性**——丢一帧数据要能发现，
而不是让上位机画出一条悄悄少了几百个点的曲线。所以协议里带长度与 CRC-16。

帧格式（小端）：

    +--------+--------+-----+-----+--------+-----------+--------+
    | SOF0   | SOF1   | type| seq | len(2) | payload   | CRC(2) |
    | 0xAA   | 0x55   | 1B  | 1B  | 2B LE  | len 字节   | 2B LE  |
    +--------+--------+-----+-----+--------+-----------+--------+

    CRC = CRC-16/CCITT-FALSE(init=0xFFFF, poly=0x1021)，覆盖 type..payload

帧类型：
    0x01 PPG_BATCH  payload = ts_ms(4B LE) + N×(ir:3B LE, red:3B LE)   每样本对 6 字节（18 位原始值）
    0x02 STATUS     payload = led_current(1B) + sample_rate_hz(2B LE) + quality_ok(1B) + dropped(2B LE)
    0x03 LOG        payload = UTF-8 文本（固件侧错误/状态，便于现场排查）
    0x04 ECG_BATCH  payload = ts_ms(4B LE) + N×(ecg:2B LE)      每样本 2 字节（12 位 ADC 右对齐）
    0x81 CMD        payload = 命令字节（上位机 → 固件：开始/停止/改采样率）

ECG 用 2 字节而 PPG 用 3 字节：MAX30102 输出 18 位原始值，而 AD8232 走 MCU 的 12 位 ADC，
2 字节足够且省一半带宽（250 Hz × 2 B = 500 B/s，115200 波特率下毫无压力）。

与固件的一致性：`firmware/include/protocol.h` 是同一份格式的 C 实现，
任何改动必须两边同时改，并跑 `tests/test_algorithms.py` 里的协议用例。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

SOF = b"\xAA\x55"
TYPE_PPG_BATCH = 0x01
TYPE_STATUS = 0x02
TYPE_LOG = 0x03
TYPE_ECG_BATCH = 0x04
TYPE_CMD = 0x81

MAX_PAYLOAD = 512


def crc16_ccitt(data: bytes, init: int = 0xFFFF, poly: int = 0x1021) -> int:
    """CRC-16/CCITT-FALSE。"""
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes = b""

    def encode(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("payload too large")
        body = struct.pack("<BBH", self.type, self.seq, len(self.payload)) + self.payload
        return SOF + body + struct.pack("<H", crc16_ccitt(body))

    def decode_ecg(self) -> tuple[int, list[int]]:
        """解析 ECG_BATCH：返回 (首个样本时间戳 ms, [ecg, ...])。"""
        if self.type != TYPE_ECG_BATCH:
            raise ValueError("not an ECG batch frame")
        if len(self.payload) < 4 or (len(self.payload) - 4) % 2 != 0:
            raise ValueError("bad ECG payload length")
        ts_ms = struct.unpack_from("<I", self.payload, 0)[0]
        samples: list[int] = []
        for off in range(4, len(self.payload), 2):
            samples.append(struct.unpack_from("<H", self.payload, off)[0])
        return ts_ms, samples

    def decode_ppg(self) -> tuple[int, list[tuple[int, int]]]:
        """解析 PPG_BATCH：返回 (首个样本时间戳 ms, [(ir, red), ...])。"""
        if self.type != TYPE_PPG_BATCH:
            raise ValueError("not a PPG batch frame")
        if len(self.payload) < 4 or (len(self.payload) - 4) % 6 != 0:
            raise ValueError("bad PPG payload length")
        ts_ms = struct.unpack_from("<I", self.payload, 0)[0]
        pairs: list[tuple[int, int]] = []
        for off in range(4, len(self.payload), 6):
            ir = int.from_bytes(self.payload[off:off + 3], "little")
            red = int.from_bytes(self.payload[off + 3:off + 6], "little")
            pairs.append((ir, red))
        return ts_ms, pairs


def ppg_batch(seq: int, ts_ms: int, samples: list[tuple[int, int]]) -> Frame:
    """构造 PPG 批量帧（每样本对 6 字节，18 位原始值）。"""
    payload = struct.pack("<I", ts_ms)
    for ir, red in samples:
        payload += int(ir & 0x3FFFF).to_bytes(3, "little")
        payload += int(red & 0x3FFFF).to_bytes(3, "little")
    return Frame(TYPE_PPG_BATCH, seq & 0xFF, payload)


def ecg_batch(seq: int, ts_ms: int, samples: list[int]) -> Frame:
    """构造 ECG 批量帧（每样本 2 字节，12 位 ADC 原始值）。"""
    payload = struct.pack("<I", ts_ms)
    for value in samples:
        payload += int(value & 0xFFFF).to_bytes(2, "little")
    return Frame(TYPE_ECG_BATCH, seq & 0xFF, payload)


def status(seq: int, led_current: int, fs_hz: int, quality_ok: bool, dropped: int) -> Frame:
    return Frame(TYPE_STATUS, seq & 0xFF,
                 struct.pack("<BHBH", led_current & 0xFF, fs_hz & 0xFFFF,
                             1 if quality_ok else 0, dropped & 0xFFFF))


def log(seq: int, text: str) -> Frame:
    return Frame(TYPE_LOG, seq & 0xFF, text.encode("utf-8"))


@dataclass
class Parser:
    """流式解析器：喂多少字节都行，返回本次解析出的完整帧。

    设计点：遇到坏帧/噪声不抛异常、不中断——真机串口上电瞬间一定有垃圾字节，
    解析器必须在任意位置重新同步（靠 SOF 扫描），并把丢弃字节数记下来供排查。
    """
    buf: bytearray = field(default_factory=bytearray)
    frames: list[Frame] = field(default_factory=list)
    bytes_discarded: int = 0
    crc_errors: int = 0

    def feed(self, chunk: bytes) -> list[Frame]:
        self.buf.extend(chunk)
        out: list[Frame] = []
        while True:
            idx = self.buf.find(SOF)
            if idx < 0:
                if len(self.buf) > 1:
                    self.bytes_discarded += len(self.buf) - 1
                    del self.buf[:-1]
                break
            if idx > 0:
                self.bytes_discarded += idx
                del self.buf[:idx]
            if len(self.buf) < 6:            # SOF + type + seq + len
                break
            _, _, length = struct.unpack_from("<BBH", self.buf, 2)
            if length > MAX_PAYLOAD:
                self.bytes_discarded += 1
                del self.buf[:1]
                continue
            total = 2 + 4 + length + 2
            if len(self.buf) < total:
                break
            body = bytes(self.buf[2:6 + length])
            crc_rx = struct.unpack_from("<H", self.buf, 6 + length)[0]
            if crc16_ccitt(body) != crc_rx:
                self.crc_errors += 1
                self.bytes_discarded += 1
                del self.buf[:1]
                continue
            out.append(Frame(body[0], body[1], body[4:]))
            del self.buf[:total]
        self.frames.extend(out)
        return out
