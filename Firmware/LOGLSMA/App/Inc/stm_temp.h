#ifndef __STM_TEMP_H
#define __STM_TEMP_H

#include "stm32l4xx_hal.h"

void calc_vref(void);
void get_stm_temp(float *temp);
void ADC1_Temp_Channel_Init(void);

/* Безопасное чтение по запросу: VDDA (В) + температура кристалла (°C).
 * Возврат 1 — успех, 0 — ошибка (значения обнулены). Не вешает прибор. */
int stm_adc_read(float *vdda, float *temp);

#endif //__STM_TEMP_H
