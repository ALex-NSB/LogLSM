#ifndef __reset_h_
#define __reset_h_

#include "stm32l4xx_hal.h"

void SetPowerResetFlag(void);
uint8_t GetPowerResetFlag(void);
uint32_t GetResetStatus();
void SaveResetStatus();

#endif //__reset_h_
