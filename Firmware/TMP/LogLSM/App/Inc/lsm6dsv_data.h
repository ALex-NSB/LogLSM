/**
 * @file    lsm6dsv_data.h
 * @brief   LSM6DSV IMU sample types and raw-to-physical conversion.
 */
#ifndef LSM6DSV_DATA_H
#define LSM6DSV_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
  LSM6DSV_ACCEL_FS_2G = 0,
  LSM6DSV_ACCEL_FS_4G,
  LSM6DSV_ACCEL_FS_8G,
  LSM6DSV_ACCEL_FS_16G
} LSM6DSV_AccelFS_t;

typedef enum {
  LSM6DSV_GYRO_FS_125DPS = 0,
  LSM6DSV_GYRO_FS_250DPS,
  LSM6DSV_GYRO_FS_500DPS,
  LSM6DSV_GYRO_FS_1000DPS,
  LSM6DSV_GYRO_FS_2000DPS,
  LSM6DSV_GYRO_FS_4000DPS
} LSM6DSV_GyroFS_t;

typedef struct {
  int16_t x;
  int16_t y;
  int16_t z;
} LSM6DSV_RawVec3_t;

typedef struct {
  float x;
  float y;
  float z;
} LSM6DSV_Vec3f_t;

/** Raw and converted accelerometer + gyroscope sample. */
typedef struct {
  LSM6DSV_RawVec3_t accel_raw;
  LSM6DSV_RawVec3_t gyro_raw;
  LSM6DSV_Vec3f_t accel_g;
  LSM6DSV_Vec3f_t gyro_dps;
} LSM6DSV_ImuData_t;

int16_t LSM6DSV_Raw16FromBytes(const uint8_t *lo, const uint8_t *hi);

void LSM6DSV_ParseRaw6(const uint8_t *buf, LSM6DSV_RawVec3_t *out);

void LSM6DSV_ConvertSample(const LSM6DSV_RawVec3_t *accel_raw,
                           const LSM6DSV_RawVec3_t *gyro_raw,
                           LSM6DSV_AccelFS_t accel_fs,
                           LSM6DSV_GyroFS_t gyro_fs,
                           LSM6DSV_ImuData_t *out);

float LSM6DSV_AccelRawToG(int16_t lsb, LSM6DSV_AccelFS_t fs);
float LSM6DSV_GyroRawToDps(int16_t lsb, LSM6DSV_GyroFS_t fs);

#ifdef __cplusplus
}
#endif

#endif /* LSM6DSV_DATA_H */
