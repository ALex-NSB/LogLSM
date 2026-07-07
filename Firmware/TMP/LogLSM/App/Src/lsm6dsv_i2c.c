/**
 * @file    lsm6dsv_i2c.c
 * @brief   LSM6DSV register access over STM32 HAL I2C.
 */
#include "lsm6dsv_i2c.h"
#include "i2c.h"

I2C_HandleTypeDef *lsm6dsv_i2c_handle = &hi2c1;

void LSM6DSV_I2C_SetHandle(I2C_HandleTypeDef *hi2c)
{
  lsm6dsv_i2c_handle = hi2c;
}

HAL_StatusTypeDef LSM6DSV_ReadReg(uint8_t reg, uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U) || (lsm6dsv_i2c_handle == NULL))
  {
    return HAL_ERROR;
  }

  return HAL_I2C_Mem_Read(lsm6dsv_i2c_handle,
                          LSM6DSV_I2C_ADDR_HAL,
                          reg,
                          I2C_MEMADD_SIZE_8BIT,
                          data,
                          len,
                          LSM6DSV_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef LSM6DSV_WriteReg(uint8_t reg, const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U) || (lsm6dsv_i2c_handle == NULL))
  {
    return HAL_ERROR;
  }

  return HAL_I2C_Mem_Write(lsm6dsv_i2c_handle,
                           LSM6DSV_I2C_ADDR_HAL,
                           reg,
                           I2C_MEMADD_SIZE_8BIT,
                           (uint8_t *)data,
                           len,
                           LSM6DSV_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef LSM6DSV_ReadReg8(uint8_t reg, uint8_t *value)
{
  return LSM6DSV_ReadReg(reg, value, 1U);
}

HAL_StatusTypeDef LSM6DSV_WriteReg8(uint8_t reg, uint8_t value)
{
  return LSM6DSV_WriteReg(reg, &value, 1U);
}

HAL_StatusTypeDef LSM6DSV_ReadGyroRaw(uint8_t buf[6])
{
  return LSM6DSV_ReadReg(LSM6DSV_REG_OUTX_L_G, buf, 6U);
}

HAL_StatusTypeDef LSM6DSV_ReadAccelRaw(uint8_t buf[6])
{
  return LSM6DSV_ReadReg(LSM6DSV_REG_OUTX_L_A, buf, 6U);
}
