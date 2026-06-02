#include "app_drv_pwm.h"

#include <rtdevice.h>
#include <rtthread.h>

#ifdef BSP_USING_PWM

#define PWM_DEV_NAME            "pwm1"
#define PWM_DEV_CHANNEL         1

/* CubeMX: PSC=239, ARR=19999 @ 240 MHz TIM1 clk → 50 Hz (20 ms period). */
#define PWM_DEFAULT_PERIOD_NS      20000000U
#define PWM_DEFAULT_HIGH_TIME_NS   1500000U   /* 1.5 ms → 7.5% @ 20 ms period */

static struct {
    struct rt_device_pwm *dev;
    uint32_t              period_ns;
    uint32_t              pulse_ns;
    rt_bool_t             enabled;
    rt_bool_t             inited;
} s_pwm;

static int pwm_apply(void)
{
    rt_err_t err;

    err = rt_pwm_set(s_pwm.dev, PWM_DEV_CHANNEL, s_pwm.period_ns, s_pwm.pulse_ns);
    if (err != RT_EOK) {
        return (int)err;
    }

    if (s_pwm.pulse_ns == 0U) {
        if (s_pwm.enabled) {
            err = rt_pwm_disable(s_pwm.dev, PWM_DEV_CHANNEL);
            if (err == RT_EOK) {
                s_pwm.enabled = RT_FALSE;
            }
        }
        return (int)err;
    }

    if (!s_pwm.enabled) {
        err = rt_pwm_enable(s_pwm.dev, PWM_DEV_CHANNEL);
        if (err == RT_EOK) {
            s_pwm.enabled = RT_TRUE;
        }
    }
    return (int)err;
}

static int pwm_set_duty(uint32_t duty_permille)
{
    if (!s_pwm.inited) {
        return -RT_ERROR;
    }
    if (duty_permille > 1000U) {
        return -RT_EINVAL;
    }

    s_pwm.pulse_ns = (uint32_t)((uint64_t)s_pwm.period_ns * duty_permille / 1000U);
    return pwm_apply();
}

static int pwm_set_high_time(uint32_t high_time_ns)
{
    if (!s_pwm.inited) {
        return -RT_ERROR;
    }
    if (high_time_ns > s_pwm.period_ns) {
        return -RT_EINVAL;
    }

    s_pwm.pulse_ns = high_time_ns;
    return pwm_apply();
}

static int app_drv_pwm_init(void)
{
    if (s_pwm.inited) {
        return RT_EOK;
    }

    s_pwm.dev = (struct rt_device_pwm *)rt_device_find(PWM_DEV_NAME);
    if (s_pwm.dev == RT_NULL) {
        return -RT_ERROR;
    }

    s_pwm.period_ns = PWM_DEFAULT_PERIOD_NS;
    s_pwm.pulse_ns = PWM_DEFAULT_HIGH_TIME_NS;
    s_pwm.enabled = RT_FALSE;
    s_pwm.inited = RT_TRUE;
    return pwm_apply();
}
INIT_APP_EXPORT(app_drv_pwm_init);

const app_drv_pwm_handle_t app_drv_pwm = {
    .set_duty      = pwm_set_duty,
    .set_high_time = pwm_set_high_time,
};

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

static void pwm_print_status(void)
{
    uint32_t duty_permille = 0U;

    if (s_pwm.period_ns > 0U) {
        duty_permille = (uint32_t)((uint64_t)s_pwm.pulse_ns * 1000U / s_pwm.period_ns);
    }
    rt_kprintf("pwm: PE9/TIM1_CH1  period=%u ns  pulse=%u ns  duty=%u.%u%%  %s\n",
               s_pwm.period_ns,
               s_pwm.pulse_ns,
               duty_permille / 10U,
               duty_permille % 10U,
               s_pwm.enabled ? "enabled" : "disabled");
}

static int cmd_pwm_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_pwm.inited) {
        rt_kprintf("pwm: not initialized\n");
        return -1;
    }
    pwm_print_status();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_pwm_info, pwm_info, show board PWM status);

static int cmd_pwm_duty(int argc, char **argv)
{
    if (argc != 2) {
        rt_kprintf("usage: pwm_duty <permille>   (0..1000, e.g. 750 = 75.0%%)\n");
        return -1;
    }
    uint32_t duty = (uint32_t)strtoul(argv[1], RT_NULL, 10);
    int err = app_drv_pwm.set_duty(duty);
    if (err != RT_EOK) {
        rt_kprintf("pwm_duty: failed (%d)\n", err);
        return -1;
    }
    pwm_print_status();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_pwm_duty, pwm_duty, set PWM duty permille 0..1000);

static int cmd_pwm_high(int argc, char **argv)
{
    if (argc != 2) {
        rt_kprintf("usage: pwm_high <us>   (high-level time in microseconds, max %u)\n",
                   s_pwm.inited ? (s_pwm.period_ns / 1000U) : 0U);
        return -1;
    }
    uint32_t high_us = (uint32_t)strtoul(argv[1], RT_NULL, 10);
    int err = app_drv_pwm.set_high_time(high_us * 1000U);
    if (err != RT_EOK) {
        rt_kprintf("pwm_high: failed (%d)\n", err);
        return -1;
    }
    pwm_print_status();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_pwm_high, pwm_high, set PWM high-level time in us);
#endif /* RT_USING_FINSH */

#else /* BSP_USING_PWM */

static int pwm_not_available(uint32_t unused)
{
    (void)unused;
    return -RT_ERROR;
}

const app_drv_pwm_handle_t app_drv_pwm = {
    .set_duty      = pwm_not_available,
    .set_high_time = pwm_not_available,
};

#endif /* BSP_USING_PWM */
