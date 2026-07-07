/**
 * @file    lsm6dsv_i2c.h
 * @brief   LSM6DSV register access over STM32 HAL I2C.
 */
#ifndef LSM6DSV_I2C_H
#define LSM6DSV_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32l4xx_hal.h"

/** 7-bit I2C address (SA0 = GND). */
#define LSM6DSV_I2C_ADDR_7BIT       (0x6AU)
/** Address for HAL (shifted left by 1). */
#define LSM6DSV_I2C_ADDR_HAL        ((uint16_t)(LSM6DSV_I2C_ADDR_7BIT << 1U))

#define LSM6DSV_WHO_AM_I_REG        (0x0FU)
#define LSM6DSV_WHO_AM_I_ID         (0x70U)

#define LSM6DSV_REG_OUTX_L_G        (0x22U)
#define LSM6DSV_REG_OUTX_L_A        (0x28U)

#define LSM6DSV_I2C_TIMEOUT_MS      (100U)

/** I2C bus used for the sensor (change if wired to I2C2). */
extern I2C_HandleTypeDef *lsm6dsv_i2c_handle;

void LSM6DSV_I2C_SetHandle(I2C_HandleTypeDef *hi2c);

HAL_StatusTypeDef LSM6DSV_ReadReg(uint8_t reg, uint8_t *data, uint16_t len);
HAL_StatusTypeDef LSM6DSV_WriteReg(uint8_t reg, const uint8_t *data, uint16_t len);
HAL_StatusTypeDef LSM6DSV_ReadReg8(uint8_t reg, uint8_t *value);
HAL_StatusTypeDef LSM6DSV_WriteReg8(uint8_t reg, uint8_t value);

HAL_StatusTypeDef LSM6DSV_ReadGyroRaw(uint8_t buf[6]);
HAL_StatusTypeDef LSM6DSV_ReadAccelRaw(uint8_t buf[6]);

#ifdef __cplusplus
}
#endif

#endif /* LSM6DSV_I2C_H */
