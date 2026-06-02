#ifndef APP_DRV_PWM_H__
#define APP_DRV_PWM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board singleton: TIM1 CH1 @ PE9. Init via RT-Thread INIT_APP_EXPORT internally. */
typedef struct app_drv_pwm_handle {
    int (*set_duty)(uint32_t duty_permille);       /* 0..1000 → 0.0%..100.0% */
    int (*set_high_time)(uint32_t high_time_ns);   /* high-level width, ns */
} app_drv_pwm_handle_t;

extern const app_drv_pwm_handle_t app_drv_pwm;

#ifdef __cplusplus
}
#endif

#endif
