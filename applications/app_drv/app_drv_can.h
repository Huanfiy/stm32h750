#ifndef APP_DRV_CAN_H__
#define APP_DRV_CAN_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_drv_can_frame {
    uint32_t id;            /* 11-bit standard ID or 29-bit extended ID */
    uint8_t  ide;           /* 0 = standard, 1 = extended */
    uint8_t  rtr;           /* 0 = data frame, 1 = remote frame */
    uint8_t  dlc;           /* payload length, 0..8 (Classic CAN) */
    uint8_t  data[8];
} app_drv_can_frame_t;

/* Opaque handle. Single backing instance — `app_drv_can_instance()` is the
 * sole way to obtain it. Multiple callers share the same handle. */
typedef struct app_drv_can *app_drv_can_t;

int           app_drv_can_init(void);
app_drv_can_t app_drv_can_instance(void);
int           app_drv_can_send(app_drv_can_t h, const app_drv_can_frame_t *f, rt_int32_t timeout_ms);
int           app_drv_can_recv(app_drv_can_t h, app_drv_can_frame_t *f, rt_int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
