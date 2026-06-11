#ifndef APP_DRV_GPIO_H__
#define APP_DRV_GPIO_H__

#include <board.h>      /* GET_PIN */
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Power-enable channels for the Smart B-pillar aging-test fixture.
 *
 * Each enumerator's value is the RT-Thread pin id (GET_PIN) of the MCU pin that
 * drives that channel's "电源使能 / PWR_ENx" line. Pass the enumerator straight
 * to any app_drv_gpio handle method:
 *
 *     app_drv_gpio.enable(PWR_EN3);    // channel 3 power on
 *     app_drv_gpio.disable(PWR_EN3);   // channel 3 power off
 *     app_drv_gpio.high(PWR_EN3);      // drive the pin HIGH, polarity-agnostic
 *
 * Pin map (per fixture wiring table; 14 GPIO-owned channels are enabled):
 *   EN1 PE1   EN2 PB9   EN3 PB7   EN4 PB5    EN5 PB3    EN6 PD6   EN7 PD2   EN8 PD4
 *   EN9 PE7   EN10 PE9  EN11 PE13 EN12 PE11  EN13 PE12  EN14 PE10 EN15 PB2  EN16 PE8
 *
 * RESERVED PINS — not owned by app_drv_gpio on this board (see the .ioc).
 * This module leaves them alone; calls are silently ignored and read() gives -1:
 *   PWR_EN7  / PD2 = SDMMC1_CMD.
 *   PWR_EN15 / PB2 = QUADSPI_CLK / XIP clock line.
 * The other 14 channels are fixed GPIO outputs. PWM moved to TIM1_CH4/PE14, so
 * PWR_EN10/PE9 is GPIO-owned.
 */
typedef enum {
    PWR_EN1  = GET_PIN(E, 1),
    PWR_EN2  = GET_PIN(B, 9),
    PWR_EN3  = GET_PIN(B, 7),
    PWR_EN4  = GET_PIN(B, 5),
    PWR_EN5  = GET_PIN(B, 3),
    PWR_EN6  = GET_PIN(D, 6),
    PWR_EN7  = GET_PIN(D, 2),
    PWR_EN8  = GET_PIN(D, 4),
    PWR_EN9  = GET_PIN(E, 7),
    PWR_EN10 = GET_PIN(E, 9),
    PWR_EN11 = GET_PIN(E, 13),
    PWR_EN12 = GET_PIN(E, 11),
    PWR_EN13 = GET_PIN(E, 12),
    PWR_EN14 = GET_PIN(E, 10),
    PWR_EN15 = GET_PIN(B, 2),
    PWR_EN16 = GET_PIN(E, 8),
} app_drv_gpio_ch_t;

#define APP_DRV_GPIO_CH_NUM   16   /* PWR_EN1 .. PWR_EN16 */

/*
 * Global singleton handle. Every method takes a PWR_ENx channel.
 *   high/low/toggle/read — raw pin level, ignores the active-level polarity.
 *   enable/disable       — logical power on/off, honours PWR_EN_ACTIVE_LEVEL.
 */
typedef struct app_drv_gpio_handle {
    void (*high)(app_drv_gpio_ch_t ch);
    void (*low)(app_drv_gpio_ch_t ch);
    void (*toggle)(app_drv_gpio_ch_t ch);
    void (*enable)(app_drv_gpio_ch_t ch);
    void (*disable)(app_drv_gpio_ch_t ch);
    int  (*read)(app_drv_gpio_ch_t ch);
} app_drv_gpio_handle_t;

extern const app_drv_gpio_handle_t app_drv_gpio;

#ifdef __cplusplus
}
#endif

#endif /* APP_DRV_GPIO_H__ */
