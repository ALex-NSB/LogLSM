#include "lsm6dso_bus.h"
#include "i2c.h"

int32_t I2C_LSMDSO_Init(void)
{
  MX_I2C1_Init();
  return HAL_OK;
}

int32_t I2C_LSMDSO_DeInit(void)
{
  return HAL_I2C_DeInit(&hi2c1);
}

int32_t I2C_LSMDSO_WriteReg(uint16_t addr, uint16_t reg, uint8_t *data, uint16_t size)
{
  return HAL_I2C_Mem_Write(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, data, size, 500);
}

int32_t I2C_LSMDSO_ReadReg(uint16_t addr, uint16_t reg, uint8_t *data, uint16_t size)
{
  return HAL_I2C_Mem_Read(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, data, size, 500);
}

int32_t LSMDSO_GetTick(void)
{
  return (int32_t)HAL_GetTick();
}
