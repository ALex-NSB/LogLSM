#ifndef __LSM6DSO_BUS_H
#define __LSM6DSO_BUS_H

#include "lsm6dso.h"

#define I2C_LSMDSO_ADDRESS      0xD7        

int32_t I2C_LSMDSO_Init(void);
int32_t I2C_LSMDSO_DeInit(void);
int32_t I2C_LSMDSO_WriteReg(uint16_t addr, uint16_t reg, uint8_t *data, uint16_t size);
int32_t I2C_LSMDSO_ReadReg(uint16_t addr, uint16_t reg, uint8_t *data, uint16_t size);
int32_t LSMDSO_GetTick(void);

#endif //__LSM6DSO_BUS_H