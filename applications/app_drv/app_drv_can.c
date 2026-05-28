#include "app_drv_can.h"

#include <board.h>
#include <rtdevice.h>
#include <string.h>

#define CAN_RX_RING_SZ      32U
#define CAN_TX_POLL_TICK    1U      /* ms between TX-FIFO-full retries */
#define CAN_IRQ_PRIORITY    5U

struct app_drv_can {
    FDCAN_HandleTypeDef hfdcan;
    app_drv_can_frame_t rx_ring[CAN_RX_RING_SZ];
    volatile rt_uint16_t rx_head;
    volatile rt_uint16_t rx_tail;
    struct rt_semaphore  rx_sem;
    struct rt_mutex      tx_mutex;
    rt_bool_t            inited;
};

static struct app_drv_can g_can;

static uint32_t fdcan_dlc_encode(uint8_t bytes)
{
    /* Classic CAN: 0..8 bytes, HAL constants FDCAN_DLC_BYTES_n are bit-shifted. */
    switch (bytes) {
    case 0: return FDCAN_DLC_BYTES_0;
    case 1: return FDCAN_DLC_BYTES_1;
    case 2: return FDCAN_DLC_BYTES_2;
    case 3: return FDCAN_DLC_BYTES_3;
    case 4: return FDCAN_DLC_BYTES_4;
    case 5: return FDCAN_DLC_BYTES_5;
    case 6: return FDCAN_DLC_BYTES_6;
    case 7: return FDCAN_DLC_BYTES_7;
    default: return FDCAN_DLC_BYTES_8;
    }
}

static uint8_t fdcan_dlc_decode(uint32_t hal_dlc)
{
    /* HAL puts DataLength as the FDCAN_DLC_BYTES_n bit field; map back to bytes. */
    if (hal_dlc == FDCAN_DLC_BYTES_0) return 0;
    if (hal_dlc == FDCAN_DLC_BYTES_1) return 1;
    if (hal_dlc == FDCAN_DLC_BYTES_2) return 2;
    if (hal_dlc == FDCAN_DLC_BYTES_3) return 3;
    if (hal_dlc == FDCAN_DLC_BYTES_4) return 4;
    if (hal_dlc == FDCAN_DLC_BYTES_5) return 5;
    if (hal_dlc == FDCAN_DLC_BYTES_6) return 6;
    if (hal_dlc == FDCAN_DLC_BYTES_7) return 7;
    return 8;
}

static rt_err_t fdcan_configure(void)
{
    FDCAN_HandleTypeDef *h = &g_can.hfdcan;

    h->Instance = FDCAN2;
    h->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    h->Init.Mode = FDCAN_MODE_NORMAL;
    h->Init.AutoRetransmission = ENABLE;
    h->Init.TransmitPause = DISABLE;
    h->Init.ProtocolException = DISABLE;
    /* 500 kbps @ FDCAN_CLK=25MHz HSE: Tq=80ns, bit=2us=25Tq, sample 72%. */
    h->Init.NominalPrescaler = 2;
    h->Init.NominalSyncJumpWidth = 1;
    h->Init.NominalTimeSeg1 = 17;
    h->Init.NominalTimeSeg2 = 7;
    /* Classic mode ignores the Data* fields, but HAL still asserts the ranges. */
    h->Init.DataPrescaler = 1;
    h->Init.DataSyncJumpWidth = 1;
    h->Init.DataTimeSeg1 = 1;
    h->Init.DataTimeSeg2 = 1;
    h->Init.MessageRAMOffset = 0;
    h->Init.StdFiltersNbr = 1;
    h->Init.ExtFiltersNbr = 0;
    h->Init.RxFifo0ElmtsNbr = 16;
    h->Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
    h->Init.RxFifo1ElmtsNbr = 0;
    h->Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
    h->Init.RxBuffersNbr = 0;
    h->Init.RxBufferSize = FDCAN_DATA_BYTES_8;
    h->Init.TxEventsNbr = 0;
    h->Init.TxBuffersNbr = 0;
    h->Init.TxFifoQueueElmtsNbr = 8;
    h->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    h->Init.TxElmtSize = FDCAN_DATA_BYTES_8;
    if (HAL_FDCAN_Init(h) != HAL_OK) return -RT_ERROR;

    /* Filter 0: accept every standard ID into RX FIFO 0. */
    FDCAN_FilterTypeDef filt = {0};
    filt.IdType = FDCAN_STANDARD_ID;
    filt.FilterIndex = 0;
    filt.FilterType = FDCAN_FILTER_MASK;
    filt.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filt.FilterID1 = 0x000;
    filt.FilterID2 = 0x000;     /* mask = 0 → match all */
    if (HAL_FDCAN_ConfigFilter(h, &filt) != HAL_OK) return -RT_ERROR;

    /* Reject any frame the filter table did not match. */
    if (HAL_FDCAN_ConfigGlobalFilter(h, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
        return -RT_ERROR;
    }

    if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) return -RT_ERROR;

    HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, CAN_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);

    if (HAL_FDCAN_Start(h) != HAL_OK) return -RT_ERROR;
    return RT_EOK;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t flags)
{
    if ((flags & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U) return;

    FDCAN_RxHeaderTypeDef hdr;
    uint8_t data[8];

    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0U) {
        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &hdr, data) != HAL_OK) break;

        rt_uint16_t next = (g_can.rx_head + 1U) % CAN_RX_RING_SZ;
        if (next == g_can.rx_tail) break;   /* ring full — drop newer frame */

        app_drv_can_frame_t *f = &g_can.rx_ring[g_can.rx_head];
        f->id  = hdr.Identifier;
        f->ide = (hdr.IdType == FDCAN_EXTENDED_ID) ? 1U : 0U;
        f->rtr = (hdr.RxFrameType == FDCAN_REMOTE_FRAME) ? 1U : 0U;
        f->dlc = fdcan_dlc_decode(hdr.DataLength);
        memcpy(f->data, data, sizeof(f->data));
        g_can.rx_head = next;
    }
    rt_sem_release(&g_can.rx_sem);
}

void FDCAN2_IT0_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_FDCAN_IRQHandler(&g_can.hfdcan);
    rt_interrupt_leave();
}

int app_drv_can_init(void)
{
    if (g_can.inited) return RT_EOK;

    rt_sem_init(&g_can.rx_sem, "can_rx", 0, RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_can.tx_mutex, "can_tx", RT_IPC_FLAG_PRIO);
    g_can.rx_head = 0;
    g_can.rx_tail = 0;

    if (fdcan_configure() != RT_EOK) return -RT_ERROR;

    g_can.inited = RT_TRUE;
    return RT_EOK;
}
INIT_DEVICE_EXPORT(app_drv_can_init);

app_drv_can_t app_drv_can_instance(void)
{
    return &g_can;
}

int app_drv_can_send(app_drv_can_t h, const app_drv_can_frame_t *f, rt_int32_t timeout_ms)
{
    if (h == RT_NULL || f == RT_NULL) return -RT_EINVAL;
    if (!h->inited) return -RT_ERROR;
    if (f->dlc > 8) return -RT_EINVAL;

    FDCAN_TxHeaderTypeDef hdr = {0};
    hdr.Identifier = f->id;
    hdr.IdType = f->ide ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
    hdr.TxFrameType = f->rtr ? FDCAN_REMOTE_FRAME : FDCAN_DATA_FRAME;
    hdr.DataLength = fdcan_dlc_encode(f->dlc);
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch = FDCAN_BRS_OFF;
    hdr.FDFormat = FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr.MessageMarker = 0;

    rt_tick_t deadline = rt_tick_get();
    rt_tick_t budget = (timeout_ms < 0) ? RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout_ms);
    if (budget != RT_WAITING_FOREVER) deadline += budget;

    if (rt_mutex_take(&h->tx_mutex, budget) != RT_EOK) return -RT_ETIMEOUT;

    rt_err_t err = RT_EOK;
    for (;;) {
        if (HAL_FDCAN_GetTxFifoFreeLevel(&h->hfdcan) > 0U) {
            if (HAL_FDCAN_AddMessageToTxFifoQ(&h->hfdcan, &hdr, (uint8_t *)f->data) == HAL_OK) {
                break;
            }
            err = -RT_ERROR;
            break;
        }
        if (budget == RT_WAITING_FOREVER) {
            rt_thread_mdelay(CAN_TX_POLL_TICK);
            continue;
        }
        if ((rt_int32_t)(deadline - rt_tick_get()) <= 0) { err = -RT_ETIMEOUT; break; }
        rt_thread_mdelay(CAN_TX_POLL_TICK);
    }
    rt_mutex_release(&h->tx_mutex);
    return err;
}

int app_drv_can_recv(app_drv_can_t h, app_drv_can_frame_t *f, rt_int32_t timeout_ms)
{
    if (h == RT_NULL || f == RT_NULL) return -RT_EINVAL;
    if (!h->inited) return -RT_ERROR;

    rt_tick_t budget = (timeout_ms < 0) ? RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout_ms);
    for (;;) {
        rt_base_t lvl = rt_hw_interrupt_disable();
        if (h->rx_tail != h->rx_head) {
            *f = h->rx_ring[h->rx_tail];
            h->rx_tail = (h->rx_tail + 1U) % CAN_RX_RING_SZ;
            rt_hw_interrupt_enable(lvl);
            return RT_EOK;
        }
        rt_hw_interrupt_enable(lvl);

        rt_err_t err = rt_sem_take(&h->rx_sem, budget);
        if (err != RT_EOK) return -RT_ETIMEOUT;
        /* Loop to drain ring even if sem was released by a previous burst. */
    }
}

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

static int cmd_can_send(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("usage: can_send <hexid> [b0 b1 ... b7]\n");
        return -1;
    }
    app_drv_can_frame_t f = {0};
    f.id = strtoul(argv[1], RT_NULL, 16);
    f.dlc = (argc > 9) ? 8 : (uint8_t)(argc - 2);
    for (uint8_t i = 0; i < f.dlc; i++) {
        f.data[i] = (uint8_t)strtoul(argv[2 + i], RT_NULL, 16);
    }
    int err = app_drv_can_send(app_drv_can_instance(), &f, 1000);
    rt_kprintf("can_send id=0x%03X dlc=%u -> %d\n", f.id, f.dlc, err);
    return err;
}
MSH_CMD_EXPORT_ALIAS(cmd_can_send, can_send, send one CAN frame);

static int cmd_can_sniff(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 10;
    app_drv_can_frame_t f;
    while (n-- > 0) {
        if (app_drv_can_recv(app_drv_can_instance(), &f, 5000) != RT_EOK) {
            rt_kprintf("can_sniff: timeout\n");
            return -1;
        }
        rt_kprintf("ID=0x%03X %s DLC=%u  ",
                   f.id, f.ide ? "EXT" : "STD", f.dlc);
        for (uint8_t i = 0; i < f.dlc; i++) rt_kprintf("%02X ", f.data[i]);
        rt_kprintf("\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_can_sniff, can_sniff, sniff CAN frames (default 10));
#endif
