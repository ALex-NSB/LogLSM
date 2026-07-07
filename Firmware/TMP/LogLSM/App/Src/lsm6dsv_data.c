/**
 * @file    lsm6dsv_data.c
 * @brief   LSM6DSV IMU sample types and raw-to-physical conversion.
 */
#include "lsm6dsv_data.h"

int16_t LSM6DSV_Raw16FromBytes(const uint8_t *lo, const uint8_t *hi)
{
  return (int16_t)((uint16_t)*lo | ((uint16_t)*hi << 8));
}

void LSM6DSV_ParseRaw6(const uint8_t *buf, LSM6DSV_RawVec3_t *out)
{
  out->x = LSM6DSV_Raw16FromBytes(&buf[0], &buf[1]);
  out->y = LSM6DSV_Raw16FromBytes(&buf[2], &buf[3]);
  out->z = LSM6DSV_Raw16FromBytes(&buf[4], &buf[5]);
}

float LSM6DSV_AccelRawToG(int16_t lsb, LSM6DSV_AccelFS_t fs)
{
  static const float mg_per_lsb[] = {0.061f, 0.122f, 0.244f, 0.488f};

  return ((float)lsb * mg_per_lsb[fs]) / 1000.0f;
}

float LSM6DSV_GyroRawToDps(int16_t lsb, LSM6DSV_GyroFS_t fs)
{
  static const float mdps_per_lsb[] = {
      4.375f, 8.750f, 17.50f, 35.0f, 70.0f, 140.0f};

  return ((float)lsb * mdps_per_lsb[fs]) / 1000.0f;
}

void LSM6DSV_ConvertSample(const LSM6DSV_RawVec3_t *accel_raw,
                           const LSM6DSV_RawVec3_t *gyro_raw,
                           LSM6DSV_AccelFS_t accel_fs,
                           LSM6DSV_GyroFS_t gyro_fs,
                           LSM6DSV_ImuData_t *out)
{
  out->accel_raw = *accel_raw;
  out->gyro_raw = *gyro_raw;

  out->accel_g.x = LSM6DSV_AccelRawToG(accel_raw->x, accel_fs);
  out->accel_g.y = LSM6DSV_AccelRawToG(accel_raw->y, accel_fs);
  out->accel_g.z = LSM6DSV_AccelRawToG(accel_raw->z, accel_fs);

  out->gyro_dps.x = LSM6DSV_GyroRawToDps(gyro_raw->x, gyro_fs);
  out->gyro_dps.y = LSM6DSV_GyroRawToDps(gyro_raw->y, gyro_fs);
  out->gyro_dps.z = LSM6DSV_GyroRawToDps(gyro_raw->z, gyro_fs);
}
