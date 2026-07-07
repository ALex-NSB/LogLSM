#ifndef __command_h
#define __command_h

#include "main.h"

/* Список команд */
/*typedef enum eCmds
{
  cmdReadTemp = 0x01,
  
} cmds_t;*/


void Service(void);
void Service_IRQHandler(UART_HandleTypeDef *UartHandle);

void CmdProcess(uint8_t cmd, uint8_t *data, uint8_t size);
void CmdProcessAddr(uint8_t addr, uint8_t cmd, uint8_t *data, uint8_t size);

#endif //__command_h