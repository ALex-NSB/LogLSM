#ifndef __P25Q128_H
#define __P25Q128_H

#include "stm32l4xx_hal.h"

#define P25Q_FLASH_SIZE      (uint32_t)(16*1024*1024)
#define P25Q_PAGE_SIZE       256
#define P25Q_SECTOR_SIZE     4096

union StatusRegBits
{
  struct 
  {
    uint8_t SRP1  :       1;  
    uint8_t QE    :       1;
    uint8_t SUS2  :       1;
    uint8_t LB1   :       1;
    uint8_t LB2   :       1;  
    uint8_t LB3   :       1;
    uint8_t CMP   :       1;
    uint8_t SUS1  :       1;

    uint8_t WIP   :       1;    
    uint8_t WEL   :       1;
    uint8_t BP0   :       1;
    uint8_t BP1   :       1;
    uint8_t BP2   :       1;
    uint8_t BP3   :       1; 
    uint8_t BP4   :       1;
    uint8_t SRP0  :       1;
  }flag;
  uint8_t reg[2];
};

typedef struct{
  GPIO_TypeDef *port;
  uint32_t pin;
} IO_Def;

enum P25Qx_Mode {
  standart_mode,
  dual_mode,
  quadspi_mode,
  qpi_mode
};

enum P25Qx_State {
  P25QX_DEINIT = 0,
  P25QX_READY,
};

typedef struct {
  QSPI_HandleTypeDef *hqspi;
  IO_Def write_protect_io;
  IO_Def hold_io;
  enum P25Qx_Mode mode;
  enum P25Qx_State state;
} P25Qx_HandleTypeDef;

void P25Qx_Reset(P25Qx_HandleTypeDef *handle);
void P25Qx_Init(P25Qx_HandleTypeDef *handle);
void P25Qx_DeInit(P25Qx_HandleTypeDef *handle);

void P25Qx_SetQuadSpi(P25Qx_HandleTypeDef *handle);
void P25Qx_SetQPI(P25Qx_HandleTypeDef *handle);
//void P25Qx_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr);

uint8_t P25Qx_ReadSR(P25Qx_HandleTypeDef *handle, uint8_t reg);

void P25Qx_QPI_DisableQPI(P25Qx_HandleTypeDef *handle);
void P25Qx_QPI_EnableWrite(P25Qx_HandleTypeDef *handle);
void P25Qx_QPI_DisableWrite(P25Qx_HandleTypeDef *handle);
void P25Qx_QPI_ProgramPage(P25Qx_HandleTypeDef *handle, uint32_t addr, uint8_t *data, uint16_t size);
void P25Qx_QPI_ErasePage(P25Qx_HandleTypeDef *handle, uint32_t addr);
void P25Qx_QPI_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr);
void P25Qx_QPI_EraseChip(P25Qx_HandleTypeDef *handle);
void P25Qx_QPI_Read(P25Qx_HandleTypeDef *handle, uint32_t addr, uint16_t len, uint8_t *data);
uint32_t P25Qx_QPI_ReadID(P25Qx_HandleTypeDef *handle);

/* Memory-mapped чтение (26.07.2026): флеш виден как область памяти по
 * P25Q_MMAP_BASE, контроллер QUADSPI сам подкачивает данные по AHB —
 * без CPU-поллинга FIFO. Даёт максимальную скорость последовательного
 * чтения. После MemMapped любую indirect-команду можно слать только
 * после ExitMemMapped (HAL_QSPI_Abort). Не требует DMA-канала/.ioc. */
#define P25Q_MMAP_BASE       0x90000000u
void P25Qx_MemMapped(P25Qx_HandleTypeDef *handle);
void P25Qx_ExitMemMapped(P25Qx_HandleTypeDef *handle);

#endif //__P25Q128_H
