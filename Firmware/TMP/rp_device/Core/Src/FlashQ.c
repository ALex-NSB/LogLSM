#include "main.h"
#include "quadspi.h"
#include "FlashQ.h"

static QSPI_CommandTypeDef      sCommand;

static uint32_t QSPI_Read(QSPI_CommandTypeDef  *sCommand, uint8_t *buffer);
static uint32_t QSPI_Write(QSPI_CommandTypeDef  *sCommand, uint8_t *buffer);
static void WaitBusy(uint32_t timeout);

void FlashOn()
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  
  HAL_GPIO_WritePin(FLASH_0_GPIO_Port, FLASH_0_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(FLASH_22_GPIO_Port, FLASH_22_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  GPIO_InitStruct.Pin = FLASH_0_Pin;
  HAL_GPIO_Init(FLASH_0_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FLASH_22_Pin;
  HAL_GPIO_Init(FLASH_22_GPIO_Port, &GPIO_InitStruct);
  
  MX_QUADSPI_Init();
  
  sCommand.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
  sCommand.AddressSize       = QSPI_ADDRESS_24_BITS;
  sCommand.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  sCommand.DdrMode           = QSPI_DDR_MODE_DISABLE;
  sCommand.DdrHoldHalfCycle  = QSPI_DDR_HHC_ANALOG_DELAY;
  sCommand.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
  
  union ConfigRegBits config;
  FlashReadConfig(&config);
  
}

void FlashOff()
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  GPIO_InitStruct.Pin = FLASH_0_Pin;
  HAL_GPIO_Init(FLASH_0_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FLASH_22_Pin;
  HAL_GPIO_Init(FLASH_22_GPIO_Port, &GPIO_InitStruct);
  
  HAL_QSPI_DeInit(&hqspi);
}

uint32_t QSPI_Read(QSPI_CommandTypeDef  *sCommand, uint8_t *buffer)
{
  uint32_t ok;
  ok = HAL_QSPI_Command(&hqspi, sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE);
  if(ok != HAL_OK)
    return ok;
  ok = HAL_QSPI_Receive(&hqspi, buffer, HAL_QPSI_TIMEOUT_DEFAULT_VALUE);
  return ok;
}

uint32_t QSPI_Write(QSPI_CommandTypeDef  *sCommand, uint8_t *buffer)
{
  uint32_t ok;
  ok = HAL_QSPI_Command(&hqspi, sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE);
  if(ok != HAL_OK)
    return ok;
  ok = HAL_QSPI_Transmit(&hqspi, buffer, HAL_QPSI_TIMEOUT_DEFAULT_VALUE);
  return ok;
}

void FlashReadID(uint8_t *ID)
{
  sCommand.Instruction = 0x9F;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 0;
  sCommand.NbData = 3;

  QSPI_Read(&sCommand, ID);
}

void FlashReadStat(union StatusRegBits *status)
{
  sCommand.Instruction = 0x35;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 0;
  sCommand.NbData = 1;

  for(uint32_t i=0; i<2; i++)
  {
    if(HAL_OK != QSPI_Read(&sCommand, &status->reg[i]))
    {
      Error_Handler();
    }
    sCommand.Instruction = 0x05;
  }
}

void FlashWriteStat(union StatusRegBits *status)
{
  uint8_t ins[2] = {0x31, 0x01};

  for(uint32_t i=0; i<2; i++)
  {
    FlashWriteEnable();
    
    sCommand.Instruction = ins[i];
    sCommand.AddressMode = QSPI_ADDRESS_NONE;
    sCommand.DataMode    = QSPI_DATA_1_LINE;
    sCommand.DummyCycles = 0;
    sCommand.NbData = 1;
    
    if(HAL_OK != QSPI_Write(&sCommand, &status->reg[i]))
    {
      Error_Handler();
    }
    
    WaitBusy(100);
  }
}

void FlashReadConfig(union ConfigRegBits *config)
{
  sCommand.Instruction = 0x15;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 0;
  sCommand.NbData = 1;
  
  if(HAL_OK != QSPI_Read(&sCommand, &config->reg))
  {
    Error_Handler();
  }
}

void FlashWriteConfig(union ConfigRegBits *config)
{
  sCommand.Instruction = 0x11;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 0;
  sCommand.NbData = 1;
  
  if(HAL_OK != QSPI_Write(&sCommand, &config->reg))
  {
    Error_Handler();
  }
}

void FlashWriteEnable()
{
  sCommand.Instruction = 0x06;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_NONE;
  sCommand.DummyCycles = 0;
  
  if (HAL_QSPI_Command(&hqspi, &sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    Error_Handler();
  }
}

void WaitBusy(uint32_t timeout)
{
  union StatusRegBits status;
  uint32_t t;
  
  if(timeout == 0)
    return;
  t = HAL_GetTick();
  do
  {
    HAL_Delay(10);
    FlashReadStat(&status);
  } while( status.flag.WIP && (HAL_GetTick()-t < timeout) );
}

void FlashFastRead(uint8_t *buffer, uint32_t address, uint32_t size)
{
  sCommand.Instruction = 0x0B;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 8;
  sCommand.NbData = size;
  sCommand.Address = address;
  
  if(HAL_OK != QSPI_Read(&sCommand, buffer))
  {
    Error_Handler();
  }
}

void FlashQuadRead(uint8_t *buffer, uint32_t address, uint32_t size)
{
  sCommand.Instruction = 0x6B;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_4_LINES;
  sCommand.DummyCycles = 8;
  sCommand.NbData = size;
  sCommand.Address = address;
  
  if(HAL_OK != QSPI_Read(&sCommand, buffer))
  {
    Error_Handler();
  }
}

void FlashPageErase(uint32_t address)
{
  FlashWriteEnable();

  sCommand.Instruction = 0x81;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_NONE;
  sCommand.DummyCycles = 0;
  sCommand.Address = address;

  if (HAL_QSPI_Command(&hqspi, &sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    Error_Handler();
  }
  WaitBusy(100);
}

void FlashSectorErase(uint32_t address)
{
  FlashWriteEnable();

  sCommand.Instruction = 0x20;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_NONE;
  sCommand.DummyCycles = 0;
  sCommand.Address = address;

  if (HAL_QSPI_Command(&hqspi, &sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    Error_Handler();
  }
  WaitBusy(100);
}

void FlashChipErase(uint32_t timeout)
{
  FlashWriteEnable();

  sCommand.Instruction = 0x60;
  sCommand.AddressMode = QSPI_ADDRESS_NONE;
  sCommand.DataMode    = QSPI_DATA_NONE;
  sCommand.DummyCycles = 0;

  if (HAL_QSPI_Command(&hqspi, &sCommand, HAL_QPSI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    Error_Handler();
  }
  WaitBusy(timeout);
}

void FlashPageProgram(uint8_t *buffer, uint32_t address, uint32_t size)
{
  FlashWriteEnable();
  
  sCommand.Instruction = 0x02;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_1_LINE;
  sCommand.DummyCycles = 0;
  sCommand.Address = address;
  sCommand.NbData = size;
  
  if(HAL_OK != QSPI_Write(&sCommand, buffer))
  {
    Error_Handler();
  }
  WaitBusy(100);
}

void FlashQuadPageProgram(uint8_t *buffer, uint32_t address, uint32_t size)
{
  FlashWriteEnable();
  
  sCommand.Instruction = 0x32;
  sCommand.AddressMode = QSPI_ADDRESS_1_LINE;
  sCommand.DataMode    = QSPI_DATA_4_LINES;
  sCommand.DummyCycles = 0;
  sCommand.Address = address;
  sCommand.NbData = size;
      
  if(HAL_OK != QSPI_Write(&sCommand, buffer))
  {
    Error_Handler();
  }
  WaitBusy(100);
}
