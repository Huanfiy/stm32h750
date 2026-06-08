#include "app_drv/app_drv_adc.h"
#include "app_drv/app_drv_can.h"

#include <rtthread.h>
#include <string.h>

#define PROTO_VERSION          0x01U

#define CAN_ID_CONFIG          0x100U
#define CAN_ID_BIND_HEADER     0x110U
#define CAN_ID_BIND_SN         0x111U
#define CAN_ID_CONTROL         0x120U
#define CAN_ID_ACK             0x180U
#define CAN_ID_REPORT          0x200U
#define CAN_ID_EVENT           0x210U

#define CMD_START              0x01U
#define CMD_STOP               0x02U
#define CMD_RESET              0x03U

#define REF_CONFIG             0x00U
#define REF_BIND_HEADER        0x10U
#define REF_CONTROL            0x20U

#define EVT_MCU_FAULT          0x01U
#define EVT_LOW_CURRENT        0x02U
#define EVT_HIGH_CURRENT       0x03U

#define APP_PROTO_QUEUE_LEN    128U
#define APP_PROTO_THREAD_STACK 2048U
#define APP_PROTO_THREAD_PRIO  18U
#define APP_PROTO_MIN_PERIOD   100U

typedef struct {
    rt_uint32_t id;
    rt_uint8_t  dlc;
    rt_uint8_t  data[8];
} proto_frame_t;

typedef struct {
    rt_uint8_t  version;
    rt_uint16_t report_period_ms;
    rt_uint16_t duration_s;
    rt_uint16_t current_min_ma;
    rt_uint16_t current_max_ma;
} proto_config_t;

static struct rt_messagequeue s_proto_mq;
static rt_uint8_t s_proto_mq_pool[APP_PROTO_QUEUE_LEN * sizeof(proto_frame_t)];
static rt_thread_t s_proto_thread;
static rt_uint8_t s_proto_ready;

static proto_config_t s_cfg = {
    .version = PROTO_VERSION,
    .report_period_ms = 1000,
    .duration_s = 60,
    .current_min_ma = 100,
    .current_max_ma = 2000,
};
static rt_uint16_t s_batch_no;
static rt_uint16_t s_channel_mask;
static rt_uint8_t s_running;
static rt_uint8_t s_seq[16];
static rt_uint8_t s_event_emitted[16];
static rt_tick_t s_started_at;
static rt_tick_t s_next_report;
static rt_uint32_t s_rx_seen;
static rt_uint32_t s_rx_queued;
static rt_uint32_t s_rx_queue_fail;
static rt_uint32_t s_handled;
static rt_uint32_t s_ack_sent;
static rt_uint32_t s_ack_fail;
static rt_uint32_t s_report_sent;
static rt_uint32_t s_report_fail;
static rt_uint32_t s_event_sent;
static rt_uint32_t s_event_fail;

static rt_uint16_t u16le(const rt_uint8_t *p)
{
    return (rt_uint16_t)(p[0] | ((rt_uint16_t)p[1] << 8));
}

static void put_u16le(rt_uint8_t *p, rt_uint16_t v)
{
    p[0] = (rt_uint8_t)(v & 0xFFU);
    p[1] = (rt_uint8_t)((v >> 8) & 0xFFU);
}

static int can_send_std(rt_uint32_t id, const rt_uint8_t data[8])
{
    app_drv_can_frame_t f = {0};
    f.id = id;
    f.dlc = 8;
    memcpy(f.data, data, 8);
    return app_drv_can_send(app_drv_can_instance(), &f, 100);
}

static void send_ack(rt_uint8_t ref_low, rt_uint8_t status, rt_uint8_t channel, rt_uint8_t error)
{
    rt_uint8_t data[8] = {ref_low, status, channel, error, 0, 0, 0, 0};
    if (can_send_std(CAN_ID_ACK, data) == RT_EOK) {
        s_ack_sent++;
    } else {
        s_ack_fail++;
    }
}

static void reset_runtime(void)
{
    s_running = 0;
    s_channel_mask = 0;
    memset(s_seq, 0, sizeof(s_seq));
    memset(s_event_emitted, 0, sizeof(s_event_emitted));
}

static void handle_config(const proto_frame_t *f)
{
    if (f->dlc < 8 || f->data[0] != PROTO_VERSION || f->data[1] == 0) {
        send_ack(REF_CONFIG, 1, 0, 1);
        return;
    }
    s_cfg.version = f->data[0];
    s_cfg.report_period_ms = (rt_uint16_t)f->data[1] * 100U;
    if (s_cfg.report_period_ms < APP_PROTO_MIN_PERIOD) {
        s_cfg.report_period_ms = APP_PROTO_MIN_PERIOD;
    }
    s_cfg.duration_s = u16le(&f->data[2]);
    s_cfg.current_min_ma = u16le(&f->data[4]);
    s_cfg.current_max_ma = u16le(&f->data[6]);
    send_ack(REF_CONFIG, 0, 0, 0);
}

static void handle_bind_header(const proto_frame_t *f)
{
    rt_uint8_t ch;

    if (f->dlc < 8) {
        send_ack(REF_BIND_HEADER, 1, 0, 1);
        return;
    }
    ch = f->data[0];
    if (ch < 1 || ch > 16 || f->data[1] == 0 || f->data[1] > 64) {
        send_ack(REF_BIND_HEADER, 1, ch, 2);
        return;
    }
    s_batch_no = u16le(&f->data[3]);
    s_channel_mask |= (rt_uint16_t)(1U << (ch - 1U));
    send_ack(REF_BIND_HEADER, 0, ch, 0);
}

static void handle_control(const proto_frame_t *f)
{
    rt_uint8_t cmd;
    rt_uint16_t mask;

    if (f->dlc < 8) {
        send_ack(REF_CONTROL, 1, 0, 1);
        return;
    }
    cmd = f->data[0];
    s_batch_no = u16le(&f->data[1]);
    mask = u16le(&f->data[3]);

    if (cmd == CMD_START) {
        s_channel_mask = mask;
        s_running = (mask != 0U) ? 1U : 0U;
        memset(s_seq, 0, sizeof(s_seq));
        memset(s_event_emitted, 0, sizeof(s_event_emitted));
        s_started_at = rt_tick_get();
        s_next_report = s_started_at;
        send_ack(REF_CONTROL, s_running ? 0 : 1, 0, s_running ? 0 : 3);
        return;
    }
    if (cmd == CMD_STOP || cmd == CMD_RESET) {
        reset_runtime();
        send_ack(REF_CONTROL, 0, 0, 0);
        return;
    }
    send_ack(REF_CONTROL, 1, 0, 4);
}

static void handle_frame(const proto_frame_t *f)
{
    s_handled++;
    switch (f->id) {
    case CAN_ID_CONFIG:
        handle_config(f);
        break;
    case CAN_ID_BIND_HEADER:
        handle_bind_header(f);
        break;
    case CAN_ID_BIND_SN:
        break;
    case CAN_ID_CONTROL:
        handle_control(f);
        break;
    default:
        break;
    }
}

static void send_event(rt_uint8_t ch, rt_uint8_t event_type, rt_uint16_t current, rt_uint32_t elapsed_s)
{
    rt_uint8_t data[8] = {0};

    data[0] = ch;
    data[1] = event_type;
    put_u16le(&data[2], current);
    put_u16le(&data[4], (rt_uint16_t)(elapsed_s & 0xFFFFU));
    if (can_send_std(CAN_ID_EVENT, data) == RT_EOK) {
        s_event_sent++;
    } else {
        s_event_fail++;
    }
}

static void send_reports(void)
{
    rt_uint16_t adc[APP_DRV_ADC_TOTAL_CH];
    rt_tick_t now = rt_tick_get();
    rt_uint32_t elapsed_s = (rt_uint32_t)((now - s_started_at) / RT_TICK_PER_SECOND);
    rt_uint8_t base_state = (elapsed_s >= s_cfg.duration_s) ? 2U : 1U;

    app_drv_adc_get_snapshot(adc);
    for (rt_uint8_t ch = 1; ch <= 16; ch++) {
        rt_uint16_t raw;
        rt_uint16_t current;
        rt_uint8_t state = base_state;
        rt_uint8_t error = 0;
        rt_uint8_t data[8] = {0};

        if ((s_channel_mask & (1U << (ch - 1U))) == 0U) {
            continue;
        }
        raw = adc[ch - 1U];
        current = (rt_uint16_t)app_drv_adc_raw_to_mv(raw);
        if (current < s_cfg.current_min_ma) {
            state = 3U;
            error = EVT_LOW_CURRENT;
        } else if (current > s_cfg.current_max_ma) {
            state = 3U;
            error = EVT_HIGH_CURRENT;
        }
        if (error != 0U && !s_event_emitted[ch - 1U]) {
            send_event(ch, error, current, elapsed_s);
            s_event_emitted[ch - 1U] = 1U;
        }
        data[0] = ch;
        data[1] = state;
        put_u16le(&data[2], current);
        put_u16le(&data[4], raw);
        data[6] = error;
        data[7] = s_seq[ch - 1U]++;
        if (can_send_std(CAN_ID_REPORT, data) == RT_EOK) {
            s_report_sent++;
        } else {
            s_report_fail++;
        }
    }

    if (base_state == 2U) {
        s_running = 0;
    }
}

static void proto_thread_entry(void *parameter)
{
    (void)parameter;

    for (;;) {
        proto_frame_t f;
        rt_tick_t timeout = RT_TICK_PER_SECOND;

        if (s_running) {
            rt_tick_t now = rt_tick_get();
            if ((rt_int32_t)(s_next_report - now) <= 0) {
                send_reports();
                s_next_report = now + rt_tick_from_millisecond(s_cfg.report_period_ms);
                continue;
            }
            timeout = s_next_report - now;
        }

        if (rt_mq_recv(&s_proto_mq, &f, sizeof(f), timeout) > 0) {
            handle_frame(&f);
        }
    }
}

void app_drv_can_on_rx_frame(const app_drv_can_frame_t *f)
{
    proto_frame_t pf;

    if (f == RT_NULL || f->ide || f->rtr || f->dlc > 8) {
        return;
    }
    if (!s_proto_ready) {
        return;
    }
    if (f->id != CAN_ID_CONFIG && f->id != CAN_ID_BIND_HEADER &&
        f->id != CAN_ID_BIND_SN && f->id != CAN_ID_CONTROL) {
        return;
    }
    s_rx_seen++;
    pf.id = f->id;
    pf.dlc = f->dlc;
    memcpy(pf.data, f->data, sizeof(pf.data));
    if (rt_mq_send(&s_proto_mq, &pf, sizeof(pf)) == RT_EOK) {
        s_rx_queued++;
    } else {
        s_rx_queue_fail++;
    }
}

static int app_proto_init(void)
{
    rt_mq_init(&s_proto_mq, "ag_proto", s_proto_mq_pool, sizeof(proto_frame_t),
               sizeof(s_proto_mq_pool), RT_IPC_FLAG_PRIO);
    s_proto_ready = 1;
    s_proto_thread = rt_thread_create("ag_proto", proto_thread_entry, RT_NULL,
                                      APP_PROTO_THREAD_STACK, APP_PROTO_THREAD_PRIO, 10);
    if (s_proto_thread == RT_NULL) {
        return -RT_ERROR;
    }
    rt_thread_startup(s_proto_thread);
    return RT_EOK;
}
INIT_APP_EXPORT(app_proto_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

static int cmd_ag_proto(int argc, char **argv)
{
    if (argc > 1 && rt_strcmp(argv[1], "ack") == 0) {
        send_ack(REF_CONFIG, 0, 0, 0);
        rt_kprintf("ag_proto: sent manual ACK\n");
        return 0;
    }
    rt_kprintf("ag_proto cfg: period=%u ms duration=%u s min=%u max=%u batch=%u mask=0x%04X running=%u\n",
               s_cfg.report_period_ms, s_cfg.duration_s, s_cfg.current_min_ma,
               s_cfg.current_max_ma, s_batch_no, s_channel_mask, s_running);
    rt_kprintf("ag_proto counters: rx_seen=%u queued=%u qfail=%u handled=%u ack_sent=%u ack_fail=%u report_sent=%u report_fail=%u\n",
               s_rx_seen, s_rx_queued, s_rx_queue_fail, s_handled, s_ack_sent,
               s_ack_fail, s_report_sent, s_report_fail);
    rt_kprintf("ag_proto events: event_sent=%u event_fail=%u\n", s_event_sent, s_event_fail);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ag_proto, ag_proto, show aging CAN protocol state);
#endif
