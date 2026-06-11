#include "app_drv_gpio.h"

#include <rtdevice.h>
#include <rtthread.h>

/*
 * Channel polarity. The PWR_ENx lines are assumed active-high: drive the pin
 * HIGH to turn that channel's power on. If the fixture wires them active-low,
 * flip this one pair — enable()/disable() and the power-off-at-boot default
 * both follow from it.
 */
#define PWR_EN_ACTIVE_LEVEL     PIN_HIGH
#define PWR_EN_INACTIVE_LEVEL   PIN_LOW

/* Channel table, indexed by (PWR_ENx number - 1). Order matches the enum. */
static const app_drv_gpio_ch_t s_channels[APP_DRV_GPIO_CH_NUM] = {
    PWR_EN1,  PWR_EN2,  PWR_EN3,  PWR_EN4,
    PWR_EN5,  PWR_EN6,  PWR_EN7,  PWR_EN8,
    PWR_EN9,  PWR_EN10, PWR_EN11, PWR_EN12,
    PWR_EN13, PWR_EN14, PWR_EN15, PWR_EN16,
};

/*
 * Pins that are not part of the 14 GPIO-owned PWR_EN outputs on THIS board (per
 * the CubeMX .ioc). The fixture keeps these two channel numbers reserved, so
 * this module leaves them untouched and ignores write ops for them:
 *
 *   PWR_EN7  / PD2 = SDMMC1_CMD.
 *   PWR_EN15 / PB2 = QUADSPI_CLK / XIP clock line.
 *
 * PWM now uses TIM1_CH4 on PE14, so PWR_EN10/PE9 is a normal GPIO output.
 */
static rt_bool_t ch_is_reserved(app_drv_gpio_ch_t ch)
{
    switch (ch) {
    case PWR_EN7:               /* PD2 = SDMMC1_CMD */
        return RT_TRUE;
    case PWR_EN15:              /* PB2 = QUADSPI_CLK / XIP clock line */
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static void io_high(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return;
    }
    rt_pin_write((rt_base_t)ch, PIN_HIGH);
}

static void io_low(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return;
    }
    rt_pin_write((rt_base_t)ch, PIN_LOW);
}

static void io_toggle(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return;
    }
    rt_pin_write((rt_base_t)ch, !rt_pin_read((rt_base_t)ch));
}

static void io_enable(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return;
    }
    rt_pin_write((rt_base_t)ch, PWR_EN_ACTIVE_LEVEL);
}

static void io_disable(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return;
    }
    rt_pin_write((rt_base_t)ch, PWR_EN_INACTIVE_LEVEL);
}

static int io_read(app_drv_gpio_ch_t ch)
{
    if (ch_is_reserved(ch)) {
        return -1;
    }
    return (int)rt_pin_read((rt_base_t)ch);
}

const app_drv_gpio_handle_t app_drv_gpio = {
    .high    = io_high,
    .low     = io_low,
    .toggle  = io_toggle,
    .enable  = io_enable,
    .disable = io_disable,
    .read    = io_read,
};

static int app_drv_gpio_init(void)
{
    rt_size_t i;

    for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
        rt_base_t pin = (rt_base_t)s_channels[i];

        if (ch_is_reserved(s_channels[i])) {
            continue;
        }

        /* Latch the inactive level into ODR before switching to output so the
         * line never glitches active during the input→output transition. */
        rt_pin_write(pin, PWR_EN_INACTIVE_LEVEL);
        rt_pin_mode(pin, PIN_MODE_OUTPUT);
    }
    return RT_EOK;
}
/* INIT_DEVICE_EXPORT runs strictly after rt_hw_pin_init() (INIT_BOARD_EXPORT),
 * so the pin framework is guaranteed up by the time we touch it. */
INIT_DEVICE_EXPORT(app_drv_gpio_init);

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

static const char *ch_pin_name(app_drv_gpio_ch_t ch, char *buf, rt_size_t len)
{
    unsigned int id = (unsigned int)ch;

    rt_snprintf(buf, len, "P%c%u", (char)('A' + (id / 16U)), id % 16U);
    return buf;
}

static void pwr_en_print_all(void)
{
    char name[6];
    int i;

    rt_kprintf("PWR_EN  PIN   LEVEL\n");
    for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
        app_drv_gpio_ch_t ch = s_channels[i];

        if (ch_is_reserved(ch)) {
            rt_kprintf("  %-4d  %-4s  rsvd\n", i + 1,
                       ch_pin_name(ch, name, sizeof(name)));
        } else {
            rt_kprintf("  %-4d  %-4s  %d\n", i + 1,
                       ch_pin_name(ch, name, sizeof(name)), app_drv_gpio.read(ch));
        }
    }
}

/*
 * pwr_en                  - dump all 16 channel levels
 * pwr_en <1..16>          - read one channel
 * pwr_en <1..16> <0|1>    - drive one channel LOW/HIGH (raw level)
 * pwr_en all <0|1>        - drive every GPIO-owned channel LOW/HIGH
 * pwr_en all <en|dis>     - enable/disable every GPIO-owned channel
 */
static int cmd_pwr_en(int argc, char **argv)
{
    if (argc == 1) {
        pwr_en_print_all();
        return 0;
    }

    if (rt_strcmp(argv[1], "all") == 0) {
        int i;
        if (argc != 3) {
            rt_kprintf("usage: pwr_en all <0|1|en|dis>\n");
            return -1;
        }

        if (rt_strcmp(argv[2], "en") == 0) {
            for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
                app_drv_gpio.enable(s_channels[i]);
            }
            rt_kprintf("pwr_en: all -> en\n");
            return 0;
        }
        if (rt_strcmp(argv[2], "dis") == 0) {
            for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
                app_drv_gpio.disable(s_channels[i]);
            }
            rt_kprintf("pwr_en: all -> dis\n");
            return 0;
        }

        if (rt_strcmp(argv[2], "1") == 0) {
            for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
                app_drv_gpio.high(s_channels[i]);
            }
            rt_kprintf("pwr_en: all -> 1\n");
            return 0;
        }
        if (rt_strcmp(argv[2], "0") == 0) {
            for (i = 0; i < APP_DRV_GPIO_CH_NUM; i++) {
                app_drv_gpio.low(s_channels[i]);
            }
            rt_kprintf("pwr_en: all -> 0\n");
            return 0;
        }

        rt_kprintf("usage: pwr_en all <0|1|en|dis>\n");
        return -1;
    }

    int n = atoi(argv[1]);
    if (n < 1 || n > APP_DRV_GPIO_CH_NUM) {
        rt_kprintf("usage: pwr_en [<1..16> [0|1]] | [all <0|1|en|dis>]\n");
        return -1;
    }

    app_drv_gpio_ch_t ch = s_channels[n - 1];

    if (ch_is_reserved(ch)) {
        char name[6];
        rt_kprintf("pwr_en: PWR_EN%d (%s) is reserved (not app_drv_gpio-owned) — ignored\n",
                   n, ch_pin_name(ch, name, sizeof(name)));
        return 0;
    }

    if (argc == 2) {
        char name[6];
        rt_kprintf("PWR_EN%d (%s) = %d\n",
                   n, ch_pin_name(ch, name, sizeof(name)), app_drv_gpio.read(ch));
        return 0;
    }

    int v = atoi(argv[2]) ? 1 : 0;
    v ? app_drv_gpio.high(ch) : app_drv_gpio.low(ch);
    rt_kprintf("pwr_en: PWR_EN%d -> %d\n", n, v);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_pwr_en, pwr_en, control PWR_EN1..16 power-enable pins);
#endif /* RT_USING_FINSH */
