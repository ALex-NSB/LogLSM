#ifndef __COM_H
#define __COM_H

#include "stm32l4xx_hal.h"

/* Адресация LTP на линии стенда (CLAUDE.md, раздел «Стенд»):
 *   0x8C — сам Nucleo (драйвер шагового двигателя)
 *   0x8D — регистратор LOGLSMA (ретранслируется на regUart как есть) */
#define LTP_OWN_ADDR             0x8C
#define LTP_REG_ADDR             0x8D

void ComInit();
void ComPoll();

#endif //__COM_H
