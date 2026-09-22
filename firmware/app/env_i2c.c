/*
 * env_i2c.c —— I2C1（PB6/PB7）实现，移植自 stm32-smart-home-ota/application/Core/Src/env_i2c.c
 *
 * 保持了原项目的实现风格（静态句柄、50 ms 超时、MspInit 里配 GPIO），差异只有两点：
 *   1. 增加 Mem 读写（MAX30102 的寄存器语义，用 repeated-start）；
 *   2. 记录最近一次 HAL 错误码，供串口上报——真机上 I2C 出错最常见的原因就是总线被拉低/上拉不足，
 *      有错误码才能区分「器件没回应」与「时序错误」。
 */

#include "env_i2c.h"

#include "stm32f1xx_hal.h"

#define ENV_I2C_TIMEOUT_MS 50U

static I2C_HandleTypeDef g_hi2c1;
static bool g_initialized;
static uint32_t g_last_error;

bool env_i2c_init(void)
{
    g_hi2c1.Instance = I2C1;
    g_hi2c1.Init.ClockSpeed = 100000U;      /* 10 kHz? 不：100 kHz 标准模式，MAX30102 支持到 400 kHz */
    g_hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    g_hi2c1.Init.OwnAddress1 = 0U;
    g_hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    g_hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    g_hi2c1.Init.OwnAddress2 = 0U;
    g_hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    g_hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    const HAL_StatusTypeDef st = HAL_I2C_Init(&g_hi2c1);
    g_last_error = (st == HAL_OK) ? 0U : (uint32_t)st;
    g_initialized = (st == HAL_OK);
    return g_initialized;
}

bool env_i2c_device_ready(uint8_t address_7bit)
{
    if (!g_initialized) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(&g_hi2c1, (uint16_t)(address_7bit << 1), 2U,
                                                      ENV_I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

bool env_i2c_write(uint8_t address_7bit, const uint8_t *data, uint16_t length)
{
    if (!g_initialized || data == 0 || length == 0U) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(&g_hi2c1, (uint16_t)(address_7bit << 1),
                                                        (uint8_t *)data, length, ENV_I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

bool env_i2c_read(uint8_t address_7bit, uint8_t *data, uint16_t length)
{
    if (!g_initialized || data == 0 || length == 0U) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_I2C_Master_Receive(&g_hi2c1, (uint16_t)(address_7bit << 1),
                                                       data, length, ENV_I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

bool env_i2c_mem_write(uint8_t address_7bit, uint8_t reg, const uint8_t *data, uint16_t length)
{
    if (!g_initialized || data == 0 || length == 0U) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&g_hi2c1, (uint16_t)(address_7bit << 1), reg,
                                                   I2C_MEMADD_SIZE_8BIT, (uint8_t *)data, length,
                                                   ENV_I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

bool env_i2c_mem_read(uint8_t address_7bit, uint8_t reg, uint8_t *data, uint16_t length)
{
    if (!g_initialized || data == 0 || length == 0U) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&g_hi2c1, (uint16_t)(address_7bit << 1), reg,
                                                  I2C_MEMADD_SIZE_8BIT, data, length,
                                                  ENV_I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

uint32_t env_i2c_last_error(void)
{
    return g_last_error;
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) {
        return;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
}
