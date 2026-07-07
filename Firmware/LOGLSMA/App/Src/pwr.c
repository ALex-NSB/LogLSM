#include "pwr.h"
#include "quadspi.h"
#include "spi.h"
#include "i2c.h"
#include "usart.h"
#include "p25q128.h"
#include "fm25xx.h"
#include "tmp117.h"
#include "globals.h"

void framActiv(void)
{
  HAL_GPIO_WritePin(FPWR_GPIO_Port, FPWR_Pin, GPIO_PIN_SET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
  MX_SPI1_Init();
  fm25xx_Init(&hspi1, SPI1_CS_GPIO_Port, SPI1_CS_Pin, 128 * 1024);
}

void framDeactiv(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  HAL_SPI_DeInit(&hspi1);
  /* CS в LOW перед отключением питания */
  HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
  /* M_PWR (PB5) → Analog (Hi-Z): не Output-LOW, иначе ток через пин MCU */
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Pin  = FPWR_Pin;
  HAL_GPIO_Init(FPWR_GPIO_Port, &GPIO_InitStruct);
}

void tmp117Activ(void)
{
  HAL_GPIO_WritePin(TPWR_GPIO_Port, TPWR_Pin, GPIO_PIN_SET);
  HAL_Delay(2);
  MX_I2C2_Init();
  tmp117_Init(&hi2c2);
}

void tmp117Deactiv(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  HAL_I2C_DeInit(&hi2c2);
  /* T_PWR (PB4) → Analog (Hi-Z) */
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Pin  = TPWR_Pin;
  HAL_GPIO_Init(TPWR_GPIO_Port, &GPIO_InitStruct);
}

void bleActiv(void)
{
  HAL_GPIO_WritePin(nBPWR_GPIO_Port, nBPWR_Pin, GPIO_PIN_RESET);
  for(uint32_t i=0; i<(uint32_t)1000000; i++)
    __NOP();
  ble_flag = 1;
  //HAL_Delay(100);
  MX_USART1_UART_Init();
}

void bleDeactiv(void)
{
  HAL_UART_DeInit(&huart1);
  HAL_GPIO_WritePin(nBPWR_GPIO_Port, nBPWR_Pin, GPIO_PIN_SET);
}

void flashActiv(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  
  HAL_GPIO_WritePin(FLASH_22_GPIO_Port, FLASH_22_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(FLASH_0_GPIO_Port, FLASH_0_Pin, GPIO_PIN_SET);
  HAL_Delay(5);   /* старт питания Flash до первых QSPI-команд (иначе Wait_Busy зависнет) */

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  
  GPIO_InitStruct.Pin = FLASH_22_Pin;
  HAL_GPIO_Init(FLASH_22_GPIO_Port, &GPIO_InitStruct);  
  GPIO_InitStruct.Pin = FLASH_0_Pin;
  HAL_GPIO_Init(FLASH_0_GPIO_Port, &GPIO_InitStruct);
  MX_QUADSPI_Init();
  flash.hqspi = &hqspi;  /* flash.hqspi никогда не присваивался → NULL → все QPI-команды уходили в никуда */
  P25Qx_Init(&flash);
  P25Qx_SetQPI(&flash);
}
