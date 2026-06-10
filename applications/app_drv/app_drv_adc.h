#ifndef APP_DRV_ADC_H__
#define APP_DRV_ADC_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DRV_ADC_TOTAL_CH    16U

/* Channel index → physical pin mapping (matches reference current-sample BSP):
 *   0..5  : PA6, PC4, PB1, PA7, PC5, PB0     (ADC1)
 *   6..7  : PC0, PC1                         (ADC1)
 *   8..13 : PA2, PA3, PA0, PA1, PA4, PA5     (ADC1)
 *   14..15: PC3, PC2                         (ADC3, INP1 then INP0)
 */
int      app_drv_adc_init(void);
int      app_drv_adc_wait(rt_int32_t timeout_ms);
void     app_drv_adc_get_snapshot(uint16_t out[APP_DRV_ADC_TOTAL_CH]);
uint16_t app_drv_adc_get_vrefint_raw(void);
uint32_t app_drv_adc_raw_to_mv(uint16_t raw);
uint32_t app_drv_adc_raw_to_current_ma(uint16_t raw);

#ifdef __cplusplus
}
#endif

#endif
