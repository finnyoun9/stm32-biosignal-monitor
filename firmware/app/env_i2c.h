/*
 * env_i2c.h —— I2C1 总线抽象（移植自 stm32-smart-home-ota 的 env_i2c，另加寄存器语义访问）
 *
 * 为什么保留这层抽象：环境终端项目已经用它在 PB6/PB7 上挂了 OLED/BH1750/AHT20/BMP280，
 * MAX30102（7 位地址 0x57）挂同一条总线即可，地址不冲突。抽象层让「总线怎么通」与「器件怎么用」分开，
 * 器件驱动（max30102.c）只依赖函数指针，因此可以在主机上用模拟芯片做单测。
 *
 * 与旧版的差别：MAX30102 需要「先写寄存器地址、再读数据」的寄存器语义，
 * 所以要额外的 env_i2c_mem_write/read —— 用 HAL 的 Mem 接口（内部是 repeated-start，
 * 不要用「写指针→STOP→再 START 读」的两段式，那会在高采样率下丢数据）。
 */

#ifndef BIOSIGNAL_ENV_I2C_H
#define BIOSIGNAL_ENV_I2C_H

#include <stdbool.h>
#include <stdint.h>

bool env_i2c_init(void);
bool env_i2c_device_ready(uint8_t address_7bit);
bool env_i2c_write(uint8_t address_7bit, const uint8_t *data, uint16_t length);
bool env_i2c_read(uint8_t address_7bit, uint8_t *data, uint16_t length);

/* 寄存器语义：写/读 reg 起始的 len 字节（MAX30102 驱动用这两个） */
bool env_i2c_mem_write(uint8_t address_7bit, uint8_t reg, const uint8_t *data, uint16_t length);
bool env_i2c_mem_read(uint8_t address_7bit, uint8_t reg, uint8_t *data, uint16_t length);

/* 最近一次失败的 HAL 状态码，便于串口上报定位（0 = 无错误） */
uint32_t env_i2c_last_error(void);

#endif /* BIOSIGNAL_ENV_I2C_H */
