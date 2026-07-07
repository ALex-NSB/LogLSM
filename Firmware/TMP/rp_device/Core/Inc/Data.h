#ifndef __DATA_H
#define __DATA_H

#include "stm32l4xx_hal.h"
#include "lsm6dso.h"

#define GYRO_THRESHOLD          1000

#pragma pack(push,1)
typedef struct RTC_DateTimeS
{
  uint8_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minutes;
  uint8_t seconds;
} RTC_DateTime;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct
{
  RTC_DateTime startTimeStamp;
  RTC_DateTime stopTimeStamp;
  uint16_t maxRate;
  uint16_t maxVibr;
} RotationData;
#pragma pack(pop)

typedef struct
{
  uint8_t state;
  uint32_t totalSec;
  RotationData rot;
  LSM6DSO_Object_t *lsm;
  int32_t sensorData[256];
  int32_t avgGyro;
  uint32_t len;
} RegistratorData;

uint32_t RTC_GetTimeDate(RTC_DateTime *dt);
void RTC_SetTimeDate(RTC_DateTime *dt);
uint32_t RTC_SubTimeDateSec(RTC_DateTime *dt1, RTC_DateTime *dt2);
uint32_t Poll_Sensor(RegistratorData *r);
uint8_t RotationDetected(RegistratorData *r);
uint32_t SaveTotalSec(RegistratorData *r);
uint32_t LoadTotalSec(RegistratorData *r);
uint32_t SaveParamOnEEPROM(RegistratorData *r);
uint32_t HandleSensorData(RegistratorData *r);

#endif //__DATA_H
