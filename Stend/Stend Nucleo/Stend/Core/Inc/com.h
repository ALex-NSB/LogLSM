#ifndef __COM_H
#define __COM_H

#include "stm32l4xx_hal.h"

#define WAKE_OWN_ADDR           0x81

void ComInit();
void ComPoll();

#endif //__COM_H
