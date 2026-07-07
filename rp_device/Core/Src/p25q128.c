#include "p25q128.h"

#define ManufactDeviceID_CMD	        0x9f
#define REMS_CMD	                0x90
#define READ_STATU_REGISTER_1           0x05
#define READ_STATU_REGISTER_2           0x35
#define WRITE_STATU_REGISTER_1          0x01
#define WRITE_STATU_REGISTER_2          0x31
//#define READ_SPI_DATA_CMD	        0x03
#define READ_DATA_CMD	                0x0B
#define WRITE_ENABLE_CMD	        0x06
#define WRITE_DISABLE_CMD	        0x04
#define PAGE_ERASE_CMD	                0x81
#define CHIP_ERASE_CMD0	                0xc7
#define CHIP_ERASE_CMD1	                0x60
#define SECTOR_ERASE_CMD		0x20
#define PAGE_PROGRAM_CMD                0x02
#define ENABLE_QPI	                0x38
#define DISABLE_QPI	                0xff

HAL_StatusTypeDef QSPI_Send_Command(P25Qx_HandleTypeDef *handle,
                                    uint32_t instruction, 
                                    uint32_t address, 
                                    uint32_t dummyCycles, 
                                    uint32_t instructionMode, 
                                    uint32_t addressMode, 
                                    uint32_t addressSize, 
                                    uint32_t dataMode)
{
    QSPI_CommandTypeDef cmd;
    cmd.Instruction = instruction;                 	        // инструкция
    cmd.Address = address;                                      //адрес
    cmd.DummyCycles = dummyCycles;                              // Устанавливаем количество пустых командных циклов
    cmd.InstructionMode = instructionMode;			// Командный режим
    cmd.AddressMode = addressMode;   				// Адресный режим
    cmd.AddressSize = addressSize;   				// Длина адреса
    cmd.DataMode = dataMode;             			// Режим данных
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;       	        // Отправлять инструкции каждый раз
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;          // Нет чередующихся байтов
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;           	        // Закройте режим DDR
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    return HAL_QSPI_Command(handle->hqspi, &cmd, 500);
}

HAL_StatusTypeDef QSPI_Receive(P25Qx_HandleTypeDef *handle, uint8_t* recv_buf, uint32_t size)
{
    handle->hqspi->Instance->DLR = size - 1;                                            // Длина данных конфигурации
    return HAL_QSPI_Receive(handle->hqspi, recv_buf, 500);		                // Получение данных
}

uint8_t P25Qx_ReadSR(P25Qx_HandleTypeDef *handle, uint8_t reg)
{
    uint8_t cmd = 0, result = 0;	
    switch(reg)
    {
      case 1:
        cmd = READ_STATU_REGISTER_1;
        break;
      case 2:
        cmd = READ_STATU_REGISTER_2;
        break;
      default:
        cmd = READ_STATU_REGISTER_1;
    }
    if(handle->mode != qpi_mode)
      QSPI_Send_Command(handle, cmd, 0, 0, QSPI_INSTRUCTION_1_LINE, QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS, QSPI_DATA_1_LINE);
    else
      QSPI_Send_Command(handle, cmd, 0, 0, QSPI_INSTRUCTION_4_LINES, QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS, QSPI_DATA_4_LINES);
    QSPI_Receive(handle, &result, 1);
    return result;
}

void Wait_Busy(P25Qx_HandleTypeDef *handle)
{
    while((P25Qx_ReadSR(handle, 1) & 0x01) == 0x01);
}

void P25Qx_Init(P25Qx_HandleTypeDef *handle)
{
  handle->mode = standart_mode;
  uint8_t r = P25Qx_ReadSR(handle, 2);
  if(r & 0x02)
    handle->mode = quadspi_mode;
  handle->state = P25QX_READY;
}

void P25Qx_DeInit(P25Qx_HandleTypeDef *handle)
{
  handle->state = P25QX_DEINIT;
}

void P25Qx_SetQuadSpi(P25Qx_HandleTypeDef *handle)
{
  uint8_t r = P25Qx_ReadSR(handle, 2);
  if((r & 0x02) == 0)
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
            QSPI_INSTRUCTION_1_LINE,
            QSPI_ADDRESS_NONE,
            QSPI_ADDRESS_24_BITS,
            QSPI_DATA_NONE);
    
    QSPI_Send_Command(handle, WRITE_STATU_REGISTER_2, 0, 0,
            QSPI_INSTRUCTION_1_LINE,
            QSPI_ADDRESS_NONE,
            QSPI_ADDRESS_24_BITS,
            QSPI_DATA_1_LINE);
    
    handle->hqspi->Instance->DLR = 0;
    r=2;
    HAL_QSPI_Transmit(handle->hqspi, &r, 500);
    Wait_Busy(handle);
  }
  handle->mode = quadspi_mode;
}

/*void P25Qx_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  QSPI_Send_Command(handle, SECTOR_ERASE_CMD, addr, 0,
                    QSPI_INSTRUCTION_1_LINE,
                    QSPI_ADDRESS_1_LINE, QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_NONE);
  Wait_Busy(handle);
}*/

void P25Qx_SetQPI(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command( handle,
                     ENABLE_QPI,
                     0,                                    //адрес
                     0,                                    //dummyCycles
                     QSPI_INSTRUCTION_1_LINE,              //instructionMode
                     QSPI_ADDRESS_NONE,                    // addressMode, 
                     QSPI_ADDRESS_24_BITS,                 // addressSize, 
                     QSPI_DATA_NONE);                      // dataMode
  HAL_Delay(2);
  
  /*uint8_t param = (1 << 4);
  QSPI_Send_Command(handle, 0xC0, 0, 0,
        QSPI_INSTRUCTION_4_LINES,
        QSPI_ADDRESS_NONE,
        0,
        QSPI_DATA_4_LINES);
  handle->hqspi->Instance->DLR = 8 - 1;
  HAL_QSPI_Transmit(handle->hqspi, &param, 500);*/
  
  handle->mode = qpi_mode;
}

void P25Qx_QPI_DisableQPI(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command(handle, DISABLE_QPI, 0, 0,
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_NONE);
  handle->mode = quadspi_mode;
}

void P25Qx_QPI_EnableWrite(P25Qx_HandleTypeDef *handle)
{
  if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_NONE, 0,
                      QSPI_DATA_NONE);
  } 
  else
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_NONE, 0,
                      QSPI_DATA_NONE);
  }
  Wait_Busy(handle);
}

/*
void P25Qx_QPI_DisableWrite(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command(handle, WRITE_DISABLE_CMD, 0, 0,
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS,
                    QSPI_DATA_NONE);
  Wait_Busy(handle);
}*/

void P25Qx_QPI_ProgramPage(P25Qx_HandleTypeDef *handle, uint32_t addr, uint8_t *data, uint16_t size)
{
  P25Qx_QPI_EnableWrite(handle);
  if(handle->mode == standart_mode)
  {
    QSPI_Send_Command(handle, 0x02, addr, 0,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_1_LINE,
                  QSPI_ADDRESS_24_BITS,
                  QSPI_DATA_1_LINE);
  }
  else if(handle->mode == quadspi_mode)
  {   
    QSPI_Send_Command(handle, 0x32, addr, 0,
              QSPI_INSTRUCTION_1_LINE,
              QSPI_ADDRESS_1_LINE,
              QSPI_ADDRESS_24_BITS,
              QSPI_DATA_4_LINES);
  }
  else if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, 0x02, addr, 0,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_4_LINES,
          QSPI_ADDRESS_24_BITS,
          QSPI_DATA_4_LINES);
  }
  else
  {
    return;
  }
  handle->hqspi->Instance->DLR = size - 1;
  HAL_QSPI_Transmit(handle->hqspi, data, 500);
  Wait_Busy(handle);
}

void P25Qx_QPI_ErasePage(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  P25Qx_QPI_EnableWrite(handle);
  
  if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, PAGE_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_4_LINES, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  else
  {
    QSPI_Send_Command(handle, PAGE_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_1_LINE, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  Wait_Busy(handle);
}

/*
void P25Qx_QPI_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  P25Qx_QPI_EnableWrite(handle);
  QSPI_Send_Command(handle, SECTOR_ERASE_CMD, addr, 0,
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_4_LINES, QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_NONE);
  Wait_Busy(handle);
}*/

void P25Qx_QPI_EraseChip(P25Qx_HandleTypeDef *handle)
{
  P25Qx_QPI_EnableWrite(handle);
  if((handle->mode == standart_mode) || (handle->mode == quadspi_mode))
  {
    QSPI_Send_Command(handle, 0x60, 0, 0,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_NONE,
                  0,
                  QSPI_DATA_NONE);
  }
  else if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, 0x60, 0, 0,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_NONE,
          0,
          QSPI_DATA_NONE);
  }
  else
  {
    return;
  }
  //Wait_Busy(handle);
}

void P25Qx_QPI_Read(P25Qx_HandleTypeDef *handle, uint32_t addr, uint16_t len, uint8_t *data)
{
  if(handle->mode == standart_mode)
  {
    QSPI_Send_Command(handle, 0x0B, addr,
                  8,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_1_LINE,
                  QSPI_ADDRESS_24_BITS,
                  QSPI_DATA_1_LINE);
  }
  else if(handle->mode == quadspi_mode)
  {
    QSPI_Send_Command(handle, 0x6B, addr,
              8,
              QSPI_INSTRUCTION_1_LINE,
              QSPI_ADDRESS_1_LINE,
              QSPI_ADDRESS_24_BITS,
              QSPI_DATA_4_LINES);
  }
  else if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, 0x0B, addr,
          10,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_4_LINES,
          QSPI_ADDRESS_24_BITS,
          QSPI_DATA_4_LINES);
  }
  else
  {
    return;
  }
  QSPI_Receive(handle, data, len);
}

uint32_t P25Qx_QPI_ReadID(P25Qx_HandleTypeDef *handle)
{
  uint32_t id;
  QSPI_Send_Command(handle, REMS_CMD,
                    0,                                                          //Addres
                    0,                                                          //Dummy
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_4_LINES,
                    QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_4_LINES);
  QSPI_Receive(handle, (uint8_t*)&id, 3);
  return id;
}
