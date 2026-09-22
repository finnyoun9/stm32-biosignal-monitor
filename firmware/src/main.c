/*
 * main.c —— 应用层：FreeRTOS 任务划分 + MAX30102 采集 + PPG 算法 + 上报
 *
 * 任务划分沿用环境终端项目（stm32-smart-home-ota）验证过的思路——**采集任务只做取数与入队**，
 * 算法与上报放在各自任务里，保证采集时序不被运算阻塞：
 *
 *   AcqTask    每 100 ms 读一次 FIFO（10 样本 @100 Hz），入队；只做 I2C 读 + 入队
 *   ProcTask   从队列取样本喂给 ppg_algo（流式因果算法），每秒上报心率/质量
 *   ReportTask 心跳与 LED 指示（现场判断固件是否在跑）
 *
 * 状态（如实标注）：**已通过 ARM GCC 编译验证**（见 firmware/README.md）；
 * 真机采集（P1）待模块到手后接线验证。算法层、协议层、驱动层都已在主机端测试过（tests/、docs/05）。
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "board.h"
#include "env_i2c.h"
#include "max30102.h"
#include "ppg_algo.h"
#include "protocol.h"
#include "uart_link.h"

#define ACQ_SAMPLE_RATE_HZ 100U
#define ACQ_CHUNK_SAMPLES 10U              /* 10 样本 @100 Hz = 100 ms 一次读取 */
#define PPG_QUEUE_LENGTH 64U

#define STACK_ACQ_TASK 256U                /* 单位：word（4 B）→ 1 KB */
#define STACK_PROC_TASK 384U
#define STACK_REPORT_TASK 256U

#define PRIO_ACQ 3U
#define PRIO_PROC 2U
#define PRIO_REPORT 1U

typedef struct {
    uint32_t red;
    uint32_t ir;
} ppg_sample_t;

static QueueHandle_t s_ppg_queue;
static max30102_t s_max30102;
static volatile bool s_quality_ok;
static volatile float s_hr_bpm;

/* ── MAX30102 的 I2C 注入（复用 env_i2c 的寄存器语义接口） ─────────────── */

static bool max30102_i2c_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return env_i2c_mem_write(addr, reg, data, len);
}

static bool max30102_i2c_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    (void)ctx;
    return env_i2c_mem_read(addr, reg, data, len);
}

/* 原始波形批量上报：上位机的体检报告与双通道验收都依赖它。
 *
 * ⚠️ 这里**调用 protocol.h 里已被跨语言测试覆盖的 proto_encode_ppg_batch()**，而不是手写载荷布局——
 * 我最初手写了一版，结果把 IR/RED 的顺序写反了（编码器是 IR 在前，我写成了 RED 在前）。
 * 手写第二份布局 = 引入一个协议不一致点，而这条路径已经有测试，直接复用即可。
 *
 * 时间戳取「本批第一个样本」的时间（读取时刻回推 n−1 个采样间隔），上位机按 ts + i/fs 线性展开。 */
static void send_ppg_batch(uint8_t seq, uint32_t ts_first_ms, const uint32_t *red,
                           const uint32_t *ir, uint8_t n)
{
    static uint32_t interleaved[2u * ACQ_CHUNK_SAMPLES];   /* [ir0,red0,ir1,red1,…] */
    uint8_t frame[PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN];

    for (uint8_t i = 0; i < n; ++i) {
        interleaved[2u * i] = ir[i];
        interleaved[2u * i + 1u] = red[i];
    }
    const size_t len = proto_encode_ppg_batch(seq, ts_first_ms, interleaved, n, frame, sizeof(frame));
    if (len > 0u) {
        (void)uart_link_send_raw(frame, (uint16_t)len);
    }
}

static void send_status(uint8_t seq)
{
    uint8_t payload[6];
    payload[0] = s_max30102.led_current;
    payload[1] = (uint8_t)(s_max30102.sample_rate_hz & 0xFFu);
    payload[2] = (uint8_t)(s_max30102.sample_rate_hz >> 8);
    payload[3] = s_quality_ok ? 1u : 0u;
    payload[4] = (uint8_t)(s_max30102.fifo_overflows & 0xFFu);
    payload[5] = (uint8_t)((s_max30102.fifo_overflows >> 8) & 0xFFu);
    (void)uart_link_send_frame(PROTO_TYPE_STATUS, seq, payload, sizeof(payload));
}

/* ── AcqTask：只取数、只入队 ─────────────────────────────────────────── */

static void acq_task(void *arg)
{
    (void)arg;
    uint8_t seq = 0;
    uint8_t batch_seq = 0;
    uint32_t red[ACQ_CHUNK_SAMPLES];
    uint32_t ir[ACQ_CHUNK_SAMPLES];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000U * ACQ_CHUNK_SAMPLES / ACQ_SAMPLE_RATE_HZ));

        const uint8_t n = max30102_read_fifo(&s_max30102, red, ir, ACQ_CHUNK_SAMPLES);
        if (n > 0u) {
            /* 本批第一个样本的时间 = 读取时刻 − (n−1) 个采样间隔 */
            const uint32_t step_ms = 1000u / ACQ_SAMPLE_RATE_HZ;
            const uint32_t now = board_millis();
            const uint32_t ts_first = now - (uint32_t)(n - 1u) * step_ms;
            send_ppg_batch(batch_seq++, ts_first, red, ir, n);
        }
        for (uint8_t i = 0; i < n; ++i) {
            const ppg_sample_t sample = {red[i], ir[i]};
            /* 队列满时丢最旧样本并计数：采集任务**绝不阻塞**（这是采集链路最容易被写坏的地方） */
            if (xQueueSend(s_ppg_queue, &sample, 0) != pdPASS) {
                ppg_sample_t dropped;
                (void)xQueueReceive(s_ppg_queue, &dropped, 0);
                (void)xQueueSend(s_ppg_queue, &sample, 0);
            }
        }

        if (++seq % 10u == 0u) {           /* 每 1 s 一帧状态 */
            send_status(seq);
        }
    }
}

/* ── ProcTask：流式算法 + 上报 ───────────────────────────────────────── */

static void proc_task(void *arg)
{
    (void)arg;
    ppg_algo_t algo;
    ppg_algo_init(&algo, (float)ACQ_SAMPLE_RATE_HZ);
    if (!algo.ready) {
        (void)uart_link_send_log(0xE0u, "ppg_algo init failed: unsupported sample rate");
    }

    uint8_t seq = 0;
    uint32_t since_report = 0;
    char msg[64];

    for (;;) {
        ppg_sample_t sample;
        if (xQueueReceive(s_ppg_queue, &sample, portMAX_DELAY) != pdPASS) {
            continue;
        }
        ppg_algo_push(&algo, (float)sample.ir);

        if (++since_report >= ACQ_SAMPLE_RATE_HZ) {      /* 每 1 s 输出一次结果 */
            since_report = 0;
            const ppg_result_t r = ppg_algo_result(&algo);
            s_hr_bpm = r.hr_bpm;
            s_quality_ok = r.quality_ok;
            const int n = snprintf(msg, sizeof(msg), "hr=%.1f pi=%.3f beats=%lu quality=%d",
                                   (double)r.hr_bpm, (double)r.pi_percent,
                                   (unsigned long)r.beats, r.quality_ok ? 1 : 0);
            if (n > 0) {
                (void)uart_link_send_log(seq++, msg);
            }
        }
    }
}

/* ── ReportTask：心跳 ────────────────────────────────────────────────── */

static void report_task(void *arg)
{
    (void)arg;
    char msg[72];

    for (;;) {
        const int n = snprintf(msg, sizeof(msg), "alive hr=%.1f q=%d i2c_err=%lu tx_fail=%lu",
                               (double)s_hr_bpm, s_quality_ok ? 1 : 0,
                               (unsigned long)env_i2c_last_error(),
                               (unsigned long)uart_link_tx_failures());
        if (n > 0) {
            (void)uart_link_send_log(0x80u, msg);
        }
        board_led_toggle();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── FreeRTOS 钩子与中断别名（沿用环境终端项目的写法） ─────────────── */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    (void)uart_link_send_log(0xE1u, "stack overflow");
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    (void)uart_link_send_log(0xE2u, "malloc failed");
    for (;;) {
    }
}

/* configSUPPORT_STATIC_ALLOCATION=1 时必须提供空闲/定时器任务的静态内存 */
static StaticTask_t s_idle_tcb;
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];
static StaticTask_t s_timer_tcb;
static StackType_t s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &s_timer_tcb;
    *ppxTimerTaskStackBuffer = s_timer_stack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vPortSVCHandler(void);
void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

void SVC_Handler(void)
{
    vPortSVCHandler();
}

void PendSV_Handler(void)
{
    xPortPendSVHandler();
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

/* ── 入口 ────────────────────────────────────────────────────────────── */

int main(void)
{
    if (!board_init()) {
        for (;;) {                         /* 时钟都起不来：不静默继续 */
            board_led_toggle();
            HAL_Delay(200);
        }
    }
    (void)uart_link_init();
    (void)env_i2c_init();

    (void)uart_link_send_log(0x01u, "boot: biosignal monitor");

    const max30102_io_t io = {max30102_i2c_write, max30102_i2c_read, 0, 0};
    if (!max30102_init(&s_max30102, &io, 0x33u, MAX30102_SR_100, MAX30102_MODE_SPO2)) {
        /* 初始化失败不静默：真机上这一步失败通常意味着 I2C 没通或器件型号不对 */
        char msg[72];
        const int n = snprintf(msg, sizeof(msg), "max30102 init failed: i2c_err=%lu",
                               (unsigned long)env_i2c_last_error());
        if (n > 0) {
            (void)uart_link_send_log(0xE3u, msg);
        }
        for (;;) {
            board_led_toggle();
            HAL_Delay(200);
        }
    }

    s_ppg_queue = xQueueCreate(PPG_QUEUE_LENGTH, sizeof(ppg_sample_t));
    if (s_ppg_queue == 0) {
        (void)uart_link_send_log(0xE4u, "queue create failed");
        for (;;) {
        }
    }

    (void)xTaskCreate(acq_task, "acq", STACK_ACQ_TASK, 0, PRIO_ACQ, 0);
    (void)xTaskCreate(proc_task, "proc", STACK_PROC_TASK, 0, PRIO_PROC, 0);
    (void)xTaskCreate(report_task, "report", STACK_REPORT_TASK, 0, PRIO_REPORT, 0);

    vTaskStartScheduler();
    for (;;) {
    }
}
