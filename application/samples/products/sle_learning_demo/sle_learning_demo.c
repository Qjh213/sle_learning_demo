/*
 * Beginner SLE TX/RX demo for BearPi-Pico H3863.
 *
 * 数据通路（应用层按行组帧，'\n' 为帧结束符）：
 *   CLIENT 串口 RX -> 环形缓冲 -> 任务组帧 -> ssapc_write_req -> SERVER 写回调 -> SERVER 串口 TX
 *   SERVER 串口 RX -> 环形缓冲 -> 任务组帧 -> SSAP 通知      -> CLIENT 通知回调 -> CLIENT 串口 TX
 *
 * 设计要点：
 *   1) UART 接收回调工作在中断上下文，只把字节搬进环形缓冲，不做打印、不调用协议栈 API；
 *   2) 组帧与发送在任务上下文完成，避免在中断里做重活；
 *   3) 帧边界由结束符 '\n' 显式定义，不再由 UART 缓冲区大小隐式决定。
 */
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "pinctrl.h"
#include "uart.h"
#include "string.h"
#include "sle_errcode.h"

#if defined(CONFIG_SLE_LEARNING_ROLE_SERVER)
#include "securec.h"
#include "sle_device_discovery.h"
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#elif defined(CONFIG_SLE_LEARNING_ROLE_CLIENT)
#include "sle_connection_manager.h"
#include "sle_ssap_client.h"
#include "sle_uart_client.h"
#endif

#define LEARN_LOG                  "[SLE-LEARN]"
#define LEARN_ADV_HANDLE           1
#define LEARN_QUEUE_LEN            5
#define LEARN_QUEUE_ITEM_SIZE      32
#define LEARN_QUEUE_POLL_TICKS     1           /* 消息队列轮询超时(tick)：不可永久阻塞，否则串口数据得不到处理 */
#define LEARN_RESTART_MESSAGE      "sle_dis_connect"
#define LEARN_FRAME_END            '\n'        /* 帧结束符：显式定义一条消息的边界 */
#define LEARN_RX_RING_SIZE         512         /* 环形缓冲容量，必须是 2 的幂 */
#define LEARN_TASK_POLL_MS         10

#define LEARN_FRAME_IDLE_FLUSH_MS  50          /* 空闲超时兜底：这么久没有新字节，就把已收到的当作一帧 */

static uint8_t g_uart_rx_buffer[CONFIG_SLE_LEARNING_MAX_PAYLOAD];
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_uart_rx_buffer,
    .rx_buffer_size = CONFIG_SLE_LEARNING_MAX_PAYLOAD,
};

static void learning_uart_init(void)
{
    uart_attr_t attr = {
        .baud_rate = CONFIG_SLE_LEARNING_UART_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE,
    };
    uart_pin_config_t pins = {
        .tx_pin = CONFIG_SLE_LEARNING_UART_TX_PIN,
        .rx_pin = CONFIG_SLE_LEARNING_UART_RX_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };

    uapi_pin_set_mode(CONFIG_SLE_LEARNING_UART_TX_PIN, PIN_MODE_1);
    uapi_pin_set_mode(CONFIG_SLE_LEARNING_UART_RX_PIN, PIN_MODE_1);
    uapi_uart_deinit(CONFIG_SLE_LEARNING_UART_BUS);
    uapi_uart_init(CONFIG_SLE_LEARNING_UART_BUS, &pins, &attr, NULL, &g_uart_buffer_config);
    osal_printk("%s UART ready: bus=%d baud=%d tx=%d rx=%d max_payload=%d\r\n",
        LEARN_LOG, CONFIG_SLE_LEARNING_UART_BUS, CONFIG_SLE_LEARNING_UART_BAUDRATE,
        CONFIG_SLE_LEARNING_UART_TX_PIN, CONFIG_SLE_LEARNING_UART_RX_PIN,
        CONFIG_SLE_LEARNING_MAX_PAYLOAD);
}

/* ---------- 环形缓冲：中断上下文写、任务上下文读 ---------- */

static uint8_t g_rx_ring[LEARN_RX_RING_SIZE];
static volatile uint16_t g_rx_ring_head;
static volatile uint16_t g_rx_ring_tail;
static volatile uint32_t g_rx_ring_lost;    /* 因环形缓冲满而丢弃的字节数 */
static volatile uint32_t g_rx_error_cnt;    /* UART 报错回调次数 */

static uint8_t g_frame_buf[CONFIG_SLE_LEARNING_MAX_PAYLOAD];
static uint16_t g_frame_len;                /* 已存入 g_frame_buf 的字节数 */
static uint32_t g_frame_total;              /* 本条帧累计字节数（含未存入的） */
static unsigned long long g_frame_last_tick; /* 最近一次收到数据的时刻（ticks） */

static void learning_send_frame(const uint8_t *data, uint16_t len);

static inline uint8_t learning_ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((g_rx_ring_head + 1U) & (LEARN_RX_RING_SIZE - 1U));

    if (next == g_rx_ring_tail) {
        return 0;
    }
    g_rx_ring[g_rx_ring_head] = byte;
    g_rx_ring_head = next;
    return 1;
}

static inline uint8_t learning_ring_pop(uint8_t *byte)
{
    if (g_rx_ring_head == g_rx_ring_tail) {
        return 0;
    }
    *byte = g_rx_ring[g_rx_ring_tail];
    g_rx_ring_tail = (uint16_t)((g_rx_ring_tail + 1U) & (LEARN_RX_RING_SIZE - 1U));
    return 1;
}

/*
 * UART 接收回调运行在【中断上下文】。
 * 这里只把字节搬进环形缓冲：不做打印、不调用协议栈 API，避免拉长中断响应。
 */
static void learning_uart_rx_isr(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint16_t index;

    if (error || buffer == NULL || length == 0) {
        g_rx_error_cnt++;
        return;
    }
    for (index = 0; index < length; index++) {
        if (learning_ring_push(bytes[index]) == 0) {
            g_rx_ring_lost += (uint32_t)(length - index);
            break;
        }
    }
}

static void learning_frame_reset(void)
{
    g_frame_len = 0;
    g_frame_total = 0;
}

/* 提交当前累积的数据：超长则丢弃，否则作为一帧发出去 */
static void learning_frame_commit(void)
{
    if (g_frame_total > sizeof(g_frame_buf)) {
        osal_printk("%s [DROP] frame len=%u exceeds max=%d\r\n",
            LEARN_LOG, (uint16_t)g_frame_total, CONFIG_SLE_LEARNING_MAX_PAYLOAD);
    } else if (g_frame_len != 0) {
        learning_send_frame(g_frame_buf, g_frame_len);
    }
    learning_frame_reset();
}

/* 组装一条帧：'\n' 是分隔符，出现即提交；'\r' 忽略（兼容 CRLF） */
static void learning_frame_feed(uint8_t byte)
{
    if (byte == LEARN_FRAME_END) {
        learning_frame_commit();
        return;
    }
    if (byte == '\r') {
        return;
    }
    g_frame_total++;
    if (g_frame_len < sizeof(g_frame_buf)) {
        g_frame_buf[g_frame_len] = byte;
        g_frame_len++;
    }
}

/*
 * 任务上下文：排空环形缓冲并组帧。
 * 分帧策略：'\n' 优先（边界精确）；
 *           若一段时间没有新字节，则把已收到的数据作为一帧提交（空闲超时兜底），
 *           避免发送方忘记加换行时数据一直发不出去、或与下一组数据粘成一帧。
 */
static void learning_drain_rx_ring(void)
{
    uint8_t byte;
    uint8_t got_data = 0;

    if (g_rx_error_cnt != 0) {
        osal_printk("%s [UART-RX-DROP] error events=%u\r\n", LEARN_LOG, g_rx_error_cnt);
        g_rx_error_cnt = 0;
    }
    if (g_rx_ring_lost != 0) {
        osal_printk("%s [DROP] uart ring overflow, lost=%u bytes\r\n", LEARN_LOG, g_rx_ring_lost);
        g_rx_ring_lost = 0;
    }
    while (learning_ring_pop(&byte) != 0) {
        got_data = 1;
        learning_frame_feed(byte);
    }
    if (got_data != 0) {
        g_frame_last_tick = osal_get_jiffies();
        return;
    }
    if (g_frame_total != 0 &&
        (osal_get_jiffies() - g_frame_last_tick) >=
            (unsigned long long)osal_msecs_to_jiffies(LEARN_FRAME_IDLE_FLUSH_MS)) {
        learning_frame_commit();
    }
}

#if defined(CONFIG_SLE_LEARNING_ROLE_SERVER)
static unsigned long g_server_queue_id;

static void learning_server_read_cb(uint8_t server_id, uint16_t conn_id,
    ssaps_req_read_cb_t *request, errcode_t status)
{
    osal_printk("%s [SERVER][RX-READ] server=%u conn=%u handle=%u status=0x%x\r\n",
        LEARN_LOG, server_id, conn_id, request->handle, status);
}

static void learning_server_write_cb(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *request, errcode_t status)
{
    if (request == NULL || request->value == NULL || request->length == 0) {
        osal_printk("%s [SERVER][DROP] empty write, status=0x%x\r\n", LEARN_LOG, status);
        return;
    }
    if (request->length > CONFIG_SLE_LEARNING_MAX_PAYLOAD) {
        osal_printk("%s [SERVER][DROP] len=%u exceeds max=%d\r\n",
            LEARN_LOG, request->length, CONFIG_SLE_LEARNING_MAX_PAYLOAD);
        return;
    }
    osal_printk("%s [SERVER][RX-WRITE] conn=%u handle=%u len=%u status=0x%x\r\n",
        LEARN_LOG, conn_id, request->handle, request->length, status);
    uapi_uart_write(CONFIG_SLE_LEARNING_UART_BUS, request->value, request->length, 0);
    uapi_uart_write(CONFIG_SLE_LEARNING_UART_BUS, (const uint8_t *)"\n", 1, 0);
}

static void learning_send_frame(const uint8_t *data, uint16_t len)
{
    errcode_t ret;

    if (!sle_uart_client_is_connected()) {
        osal_printk("%s [SERVER][TX-SKIP] no paired client\r\n", LEARN_LOG);
        return;
    }
    osal_printk("%s [SERVER][TX-NOTIFY] len=%u\r\n", LEARN_LOG, len);
    ret = sle_uart_server_send_report_by_handle(data, len);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s [SERVER][TX-FAIL] status=0x%x\r\n", LEARN_LOG, ret);
    }
}

static void learning_queue_write(uint8_t *buffer, uint16_t length)
{
    osal_msg_queue_write_copy(g_server_queue_id, buffer, length, 0);
}

static void *learning_role_task(const char *arg)
{
    unused(arg);
    uint8_t event[LEARN_QUEUE_ITEM_SIZE] = {0};
    uint32_t event_len;

    osal_printk("%s role=SERVER: enable -> service -> announce -> wait connection\r\n", LEARN_LOG);
    osal_msg_queue_create("sle_learn_queue", LEARN_QUEUE_LEN, &g_server_queue_id, 0, LEARN_QUEUE_ITEM_SIZE);
    sle_uart_server_register_msg(learning_queue_write);
    sle_uart_server_init(learning_server_read_cb, learning_server_write_cb);
    learning_uart_init();
    uapi_uart_unregister_rx_callback(CONFIG_SLE_LEARNING_UART_BUS);
    uapi_uart_register_rx_callback(CONFIG_SLE_LEARNING_UART_BUS, UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE,
        1, learning_uart_rx_isr);

    while (1) {
        /* ① 断连事件：带超时轮询；不能永久阻塞，否则串口数据得不到处理 */
        event_len = sizeof(event);
        (void)memset_s(event, sizeof(event), 0, sizeof(event));
        if (osal_msg_queue_read_copy(g_server_queue_id, event, &event_len,
            LEARN_QUEUE_POLL_TICKS) == OSAL_SUCCESS &&
            strncmp((const char *)event, LEARN_RESTART_MESSAGE, sizeof(LEARN_RESTART_MESSAGE)) == 0) {
            osal_printk("%s [SERVER] disconnected, restart announce\r\n", LEARN_LOG);
            sle_start_announce(LEARN_ADV_HANDLE);
        }
        /* ② 串口数据：出环形缓冲 -> 按行组帧 -> 通知 Client */
        learning_drain_rx_ring();
    }
    return NULL;
}

#elif defined(CONFIG_SLE_LEARNING_ROLE_CLIENT)
void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    if (data == NULL || data->data == NULL || data->data_len == 0) {
        osal_printk("%s [CLIENT][DROP] empty notification\r\n", LEARN_LOG);
        return;
    }
    osal_printk("%s [CLIENT][RX-NOTIFY] conn=%u len=%u status=0x%x\r\n",
        LEARN_LOG, conn_id, data->data_len, status);
    uapi_uart_write(CONFIG_SLE_LEARNING_UART_BUS, data->data, data->data_len, 0);
    uapi_uart_write(CONFIG_SLE_LEARNING_UART_BUS, (const uint8_t *)"\n", 1, 0);
}

void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    sle_uart_notification_cb(client_id, conn_id, data, status);
}

static void learning_send_frame(const uint8_t *data, uint16_t len)
{
    ssapc_write_param_t *write = get_g_sle_uart_send_param();
    errcode_t ret;

    if (write == NULL) {
        return;
    }
    /* 发送的是已组装完整的整帧缓冲，不再引用 UART 驱动的临时缓冲 */
    write->data = (uint8_t *)data;
    write->data_len = len;
    osal_printk("%s [CLIENT][TX-WRITE] conn=%u handle=%u len=%u\r\n",
        LEARN_LOG, get_g_sle_uart_conn_id(), write->handle, len);
    ret = ssapc_write_req(0, get_g_sle_uart_conn_id(), write);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s [CLIENT][TX-FAIL] status=0x%x\r\n", LEARN_LOG, ret);
    }
}

static void *learning_role_task(const char *arg)
{
    unused(arg);
    osal_printk("%s role=CLIENT: enable -> seek -> connect -> pair -> discover\r\n", LEARN_LOG);
    learning_uart_init();
    uapi_uart_unregister_rx_callback(CONFIG_SLE_LEARNING_UART_BUS);
    uapi_uart_register_rx_callback(CONFIG_SLE_LEARNING_UART_BUS, UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE,
        1, learning_uart_rx_isr);
    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);

    /* 常驻循环：把串口收到的数据按行组帧后写往 Server */
    while (1) {
        learning_drain_rx_ring();
        osal_msleep(LEARN_TASK_POLL_MS);
    }
    return NULL;
}
#endif

static void sle_learning_entry(void)
{
    osal_task *task;
    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)learning_role_task, NULL,
        "SLELearningTask", CONFIG_SLE_LEARNING_TASK_STACK_SIZE);
    if (task != NULL) {
        osal_kthread_set_priority(task, CONFIG_SLE_LEARNING_TASK_PRIORITY);
    } else {
        osal_printk("%s task creation failed\r\n", LEARN_LOG);
    }
    osal_kthread_unlock();
}

app_run(sle_learning_entry);

