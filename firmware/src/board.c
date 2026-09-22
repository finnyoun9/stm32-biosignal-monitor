/*
 * board.c —— 板级初始化实现
 *
 * 注意 SysTick 的用法：HAL 需要 1 ms 时基（HAL_IncTick），FreeRTOS 也要用 SysTick 做调度节拍。
 * 这里沿用环境终端项目验证过的写法——SysTick_Handler 里先 HAL_IncTick()，再在调度器启动后
 * 调 xPortSysTickHandler()（见 src/main.c）。这样 HAL_Delay 与 vTaskDelay 可以共存。
 *
 * TIM2 只做「毫秒时间戳」，不参与调度（configTICK_RATE_HZ 用 SysTick）。之所以另外要一个
 * 硬件计时器：采样时间戳要求单调且不受任务调度影响，用它给每批样本打 ts_ms。
 */

#include "board.h"

static volatile uint32_t g_millis;
static TIM_HandleTypeDef g_htim2;

bool board_init(void)
{
    /* 1) HAL 与时钟：8 MHz HSE → PLL×8 → 64 MHz */
    HAL_Init();

    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL8;                 /* 8 MHz × 8 = 64 MHz */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return false;
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;            /* PCLK1 = 32 MHz，F103 上限 36 MHz */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;            /* PCLK2 = 64 MHz */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        return false;
    }

    /* 2) 板载 LED（PC13，低电平点亮） */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = BOARD_LED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOARD_LED_PORT, &gpio);
    HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN, GPIO_PIN_SET);   /* 灭 */

    /* 3) TIM2 → 1 kHz 更新中断，做毫秒时间戳 */
    __HAL_RCC_TIM2_CLK_ENABLE();
    g_millis = 0;

    g_htim2.Instance = TIM2;
    /* 计数时钟 = PCLK1 × 2 = 64 MHz（APB1 分频不为 1 时定时器时钟倍频），
     * 所以 PSC = 64-1、ARR = 1000-1 → 1 kHz 更新中断。 */
    g_htim2.Init.Prescaler = 64U - 1U;
    g_htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    g_htim2.Init.Period = 1000U - 1U;
    g_htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    g_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&g_htim2) != HAL_OK) {
        return false;
    }
    HAL_NVIC_SetPriority(TIM2_IRQn, 6U, 0U);       /* 低于 configMAX_SYSCALL_INTERRUPT_PRIORITY(5) */
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    return HAL_TIM_Base_Start_IT(&g_htim2) == HAL_OK;
}

uint32_t board_millis(void)
{
    return g_millis;
}

void board_led_toggle(void)
{
    HAL_GPIO_TogglePin(BOARD_LED_PORT, BOARD_LED_PIN);
}

/* TIM2 更新中断：只做计数，不做任何耗时操作（中断里绝不发串口、绝不加延时） */
void TIM2_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&g_htim2, TIM_FLAG_UPDATE) != RESET &&
        __HAL_TIM_GET_IT_SOURCE(&g_htim2, TIM_IT_UPDATE) != RESET) {
        __HAL_TIM_CLEAR_IT(&g_htim2, TIM_IT_UPDATE);
        g_millis++;
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    }
}
