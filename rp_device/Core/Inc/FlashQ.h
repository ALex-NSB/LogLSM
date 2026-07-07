#ifndef __flashq_h_
#define __flashq_h_

/*

!!! DESCRIPTION !!!

*/

#include "stm32l4xx_hal.h"

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

union ConfigRegBits
{
  struct 
  {
    uint8_t Reserved    :       2;
    uint8_t WPS         :       1;
    uint8_t MPM0        :       1;
    uint8_t MPM1        :       1;
    uint8_t DRV0        :       1;
    uint8_t DRV1        :       1;
    uint8_t HOLD_RST    :       1;
  }flag;
  uint8_t reg;
};

void FlashOn();
void FlashOff();
void FlashReadID(uint8_t *ID);
void FlashReadStat(union StatusRegBits *status);
void FlashWriteStat(union StatusRegBits *status);
void FlashReadConfig(union ConfigRegBits *config);
void FlashWriteConfig(union ConfigRegBits *config);
void FlashWriteEnable();
void FlashFastRead(uint8_t *buffer, uint32_t address, uint32_t size);
void FlashQuadRead(uint8_t *buffer, uint32_t address, uint32_t size);
void FlashPageErase(uint32_t address);
void FlashSectorErase(uint32_t address);

void FlashChipErase(uint32_t timeout);

void FlashPageProgram(uint8_t *buffer, uint32_t address, uint32_t size);

#if 0
void FlashProgramStart(uint32_t addr, uint32_t size);
void FlashWriteByte(uint8_t byte);
void FlashProgramStop();

void FlashReadStart(uint32_t addr, uint32_t size);
void FlashReadByte(uint8_t *byte);
void FlashReadStop();
#endif

void FlashQuadPageProgram(uint8_t *buffer, uint32_t address, uint32_t size);

#endif //__flashq_h_
