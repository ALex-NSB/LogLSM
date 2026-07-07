#ifndef __STM_TEMP_H
#define __STM_TEMP_H

#include "stm32l4xx_hal.h"

void calc_vref(void);
void get_stm_temp(float *temp);
void ADC1_Temp_Channel_Init(void);

#endif //__STM_TEMP_H
