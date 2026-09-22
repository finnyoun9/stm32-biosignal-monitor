/*
 * uart_link.c —— USART1 发送实现（移植环境终端项目的串口初始化写法，去掉 DMA 接收）
 */

#include "uart_link.h"

#include <string.h>

#include "protocol.h"
#include "stm32f1xx_hal.h"

#define UART_TX_TIMEOUT_MS 100U
#define UART_BAUD_RATE 115200U

static UART_HandleTypeDef g_huart;
static bool g_initialized;
static uint32_t g_tx_failures;
static uint32_t g_last_error;

bool uart_link_init(void)
{
    g_huart.Instance = USART1;
    g_huart.Init.BaudRate = UART_BAUD_RATE;
    g_huart.Init.WordLength = UART_WORDLENGTH_8B;
    g_huart.Init.StopBits = UART_STOPBITS_1;
    g_huart.Init.Parity = UART_PARITY_NONE;
    g_huart.Init.Mode = UART_MODE_TX_RX;
    g_huart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    g_huart.Init.OverSampling = UART_OVERSAMPLING_16;

    const HAL_StatusTypeDef st = HAL_UART_Init(&g_huart);
    g_last_error = (st == HAL_OK) ? 0U : (uint32_t)st;
    g_initialized = (st == HAL_OK);
    return g_initialized;
}

static bool uart_write(const uint8_t *data, uint16_t length)
{
    if (!g_initialized || data == 0 || length == 0U) {
        return false;
    }
    const HAL_StatusTypeDef st = HAL_UART_Transmit(&g_huart, (uint8_t *)data, length, UART_TX_TIMEOUT_MS);
    if (st != HAL_OK) {
        g_tx_failures++;
        g_last_error = (uint32_t)st;
        return false;
    }
    return true;
}

bool uart_link_send_raw(const uint8_t *data, uint16_t length)
{
    return uart_write(data, length);
}

bool uart_link_send_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN];
    const size_t n = proto_encode(type, seq, payload, length, frame, sizeof(frame));
    if (n == 0U) {
        return false;      /* 载荷超长：明确失败，不截断发送 */
    }
    return uart_write(frame, (uint16_t)n);
}

bool uart_link_send_log(uint8_t seq, const char *text)
{
    if (text == 0) {
        return false;
    }
    const size_t len = strlen(text);
    if (len == 0U || len > PROTO_MAX_PAYLOAD) {
        return false;
    }
    return uart_link_send_frame(PROTO_TYPE_LOG, seq, (const uint8_t *)text, (uint16_t)len);
}

uint32_t uart_link_tx_failures(void)
{
    return g_tx_failures;
}

uint32_t uart_link_last_error(void)
{
    return g_last_error;
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) {
        return;
    }

    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9;                 /* PA9 = USART1_TX */
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;                /* PA10 = USART1_RX */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}
