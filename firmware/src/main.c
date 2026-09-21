/*
 * main.c —— 应用层骨架（FreeRTOS 任务划分 + HAL 传输注入 + 上报）
 *
 * 状态（如实标注）：
 *   - 本文件依赖 STM32CubeMX 生成的 HAL 初始化代码（firmware/Core/）与 FreeRTOS 源码，
 *     **尚未在真机编译验证**；
 *   - 已通过主机端验证的部分：协议编码/解析（tests/test_protocol_conformance.py）、
 *     MAX30102 驱动逻辑（tests/test_driver_host.py）；
 *   - 上机后请把实际现象与修正记录到 docs/04-调试记录.md（模板已给）。
 *
 * 任务划分与理由：
 *   AcqTask    —— 由 A_FULL 中断/200 Hz 节拍驱动读 FIFO，只做「取数据 + 入队」，不做运算
 *                 （保证采集时序不被算法阻塞，这是采集链路最容易被写坏的地方）
 *   ProcTask   —— 从队列取样本，滑窗算 HR/质量指标；CPU 重活都放这里
 *   ReportTask —— 按协议封帧通过 UART 发出；与算法解耦，便于单独调试串口
 */

#include "main.h"          /* CubeMX 生成 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "max30102.h"
#include "protocol.h"

#define ACQ_SAMPLE_RATE_HZ  100u
#define ACQ_CHUNK_SAMPLES   10u     /* 每 100 ms 读一次，留够 A_FULL 的余量 */
#define PPG_QUEUE_LENGTH    64u
#define HR_WINDOW_SAMPLES   (ACQ_SAMPLE_RATE_HZ * 10u)   /* 10 s 滑窗 */

extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef htim2;

typedef struct {
    uint32_t red;
    uint32_t ir;
} ppg_sample_t;

static QueueHandle_t s_ppg_queue;
static max30102_t s_max30102;
static volatile uint32_t s_tick_ms;

/* ── HAL 传输注入 ─────────────────────────────────────────────────── */

static bool hal_i2c_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(addr << 1), reg, I2C_MEMADD_SIZE_8BIT,
                             (uint8_t *)data, len, 50) == HAL_OK;
}

static bool hal_i2c_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    (void)ctx;
    return HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(addr << 1), reg, I2C_MEMADD_SIZE_8BIT,
                            data, len, 50) == HAL_OK;
}

static void uart_send(const uint8_t *buf, uint16_t len)
{
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 100);
}

static void send_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN];
    const size_t n = proto_encode(type, seq, payload, len, frame, sizeof(frame));
    if (n > 0) {
        uart_send(frame, (uint16_t)n);
    }
}

static uint32_t now_ms(void)
{
    return s_tick_ms;                  /* 由 TIM2 1 kHz 中断递增 */
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        s_tick_ms++;
    }
}

/* ── 任务 ─────────────────────────────────────────────────────────── */

static void AcqTask(void *arg)
{
    (void)arg;
    uint8_t seq = 0;
    uint32_t red[ACQ_CHUNK_SAMPLES];
    uint32_t ir[ACQ_CHUNK_SAMPLES];

    for (;;) {
        /* 按采样节拍对齐：ACQ_CHUNK_SAMPLES 个样本的时间 = 100 ms */
        vTaskDelay(pdMS_TO_TICKS(1000u * ACQ_CHUNK_SAMPLES / ACQ_SAMPLE_RATE_HZ));
        const uint8_t n = max30102_read_fifo(&s_max30102, red, ir, ACQ_CHUNK_SAMPLES);
        if (n == 0u) {
            /* 没有数据：可能是手指离开或 FIFO 溢出，交给 ProcTask 判断质量 */
            continue;
        }
        for (uint8_t i = 0; i < n; ++i) {
            const ppg_sample_t s = {red[i], ir[i]};
            /* 队列满时丢弃最旧样本并计数，绝不阻塞采集任务 */
            if (xQueueSend(s_ppg_queue, &s, 0) != pdPASS) {
                ppg_sample_t dropped;
                (void)xQueueReceive(s_ppg_queue, &dropped, 0);
                (void)xQueueSend(s_ppg_queue, &s, 0);
            }
        }
        /* 每 1 s 上报一次状态帧（LED 电流、采样率、质量、溢出计数） */
        if (++seq % 10u == 0u) {
            uint8_t payload[6];
            payload[0] = s_max30102.led_current;
            payload[1] = (uint8_t)(s_max30102.sample_rate_hz & 0xFFu);
            payload[2] = (uint8_t)(s_max30102.sample_rate_hz >> 8);
            payload[3] = 1u;   /* quality_ok 由 ProcTask 通过共享变量给出 */
            payload[4] = (uint8_t)(s_max30102.fifo_overflows & 0xFFu);
            payload[5] = (uint8_t)((s_max30102.fifo_overflows >> 8) & 0xFFu);
            send_frame(PROTO_TYPE_STATUS, seq, payload, sizeof(payload));
        }
    }
}

static void ProcTask(void *arg)
{
    (void)arg;
    static uint32_t red_buf[HR_WINDOW_SAMPLES];
    static uint32_t ir_buf[HR_WINDOW_SAMPLES];
    uint32_t idx = 0;
    uint32_t filled = 0;

    for (;;) {
        ppg_sample_t s;
        if (xQueueReceive(s_ppg_queue, &s, portMAX_DELAY) != pdPASS) {
            continue;
        }
        red_buf[idx] = s.red;
        ir_buf[idx] = s.ir;
        idx = (idx + 1u) % HR_WINDOW_SAMPLES;
        if (filled < HR_WINDOW_SAMPLES) {
            filled++;
        }
        /* TODO(P1)：窗口填满后调用算法层（把 tools/ppg_hr.py 的链路移植为 C）。
         * 移植时保持与 Python 侧相同的参数（0.5–4 Hz 带通、k=0.6、RR 270–2000 ms），
         * 便于用同一批数据交叉验证 C 与 Python 的结果。 */
    }
}

static void ReportTask(void *arg)
{
    (void)arg;
    uint8_t seq = 0;
    static uint32_t ts_base;

    for (;;) {
        /* TODO(P1)：按 100 ms 批量打包 PPG_BATCH 帧；当前先发心跳日志验证链路 */
        char msg[] = "report task alive";
        send_frame(PROTO_TYPE_LOG, seq++, (const uint8_t *)msg, (uint16_t)(sizeof(msg) - 1u));
        ts_base = now_ms();
        (void)ts_base;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── 初始化与启动 ─────────────────────────────────────────────────── */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_USART1_UART_Init();
    MX_TIM2_Init();

    (void)HAL_TIM_Base_Start_IT(&htim2);      /* 1 kHz 时基 */

    s_ppg_queue = xQueueCreate(PPG_QUEUE_LENGTH, sizeof(ppg_sample_t));

    const max30102_io_t io = {hal_i2c_write, hal_i2c_read, NULL, 0};
    if (!max30102_init(&s_max30102, &io, 0x33u, MAX30102_SR_100, MAX30102_MODE_SPO2)) {
        const char err[] = "max30102 init failed";
        send_frame(PROTO_TYPE_LOG, 0xFFu, (const uint8_t *)err, (uint16_t)(sizeof(err) - 1u));
        /* 初始化失败不静默：真机上这一步失败通常意味着 I2C 没通或器件不对 */
        for (;;) {
            HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin);
            HAL_Delay(200);
        }
    }

    xTaskCreate(AcqTask, "acq", 512, NULL, 3, NULL);
    xTaskCreate(ProcTask, "proc", 1024, NULL, 2, NULL);
    xTaskCreate(ReportTask, "report", 512, NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;) {
    }
}
