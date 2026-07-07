#ifndef __STEP_DRIVER_H
#define __STEP_DRIVER_H

#include "stm32l4xx_hal.h"

void StepDriverInit();
void StepDriverSetSpeed(uint16_t speed, uint8_t coef);


#endif //__STEP_DRIVER_H


