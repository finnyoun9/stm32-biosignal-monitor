/*
 * board.h —— 板级初始化（时钟、LED、毫秒时基）
 *
 * 时钟与 GPIO 配置移植自 stm32-smart-home-ota 的 SystemClock_Config / system_init：
 * 同一块 STM32F103C8T6 蓝板，8 MHz HSE 晶振 → PLL×8 → 64 MHz，APB1 = 32 MHz（≤36 MHz 上限）。
 * 保持一致的好处：现有实机经验（时序、功耗基线、上传脚本）可以直接沿用。
 */

#ifndef BIOSIGNAL_BOARD_H
#define BIOSIGNAL_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32f1xx_hal.h"

#define BOARD_LED_PORT GPIOC
#define BOARD_LED_PIN GPIO_PIN_13      /* 蓝板板载 LED（低电平点亮） */

/* 时钟 + GPIO + 时基。失败返回 false（时钟配不起来就不该继续跑） */
bool board_init(void);

/* 1 kHz 递增的毫秒计数（TIM2 中断驱动），用于采样时间戳 */
uint32_t board_millis(void);

/* 只有 LED，便于现场判断固件是否在跑 */
void board_led_toggle(void);

#endif /* BIOSIGNAL_BOARD_H */
