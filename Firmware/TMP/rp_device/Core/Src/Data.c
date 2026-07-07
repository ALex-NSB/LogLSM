#include "Data.h"
#include "rtc.h"
#include "math.h"

/*uint32_t Poll_Sensor(RegistratorData *r)
{
  LSM6DSO_GYRO_GetAxes(r->lsm, (LSM6DSO_Axes_t*)(&(r->sensorData[0])));
  LSM6DSO_ACC_GetAxes(r->lsm, (LSM6DSO_Axes_t*)(&(r->sensorData[3])));
  r->len = 6;
  return HAL_OK;
}

uint8_t RotationDetected(RegistratorData *r)
{
  LSM6DSO_Axes_t  *gyro = (LSM6DSO_Axes_t*)(&(r->sensorData[0]));
  if(gyro->x > 2500)
    return 1;
  return 0;
}
*/

////////////////////////////////////////////////////////////////////////////////
/*uint32_t Poll_Sensor(RegistratorData *r)
{
  uint8_t status;
  LSM6DSO_Axes_t *pSensor = (LSM6DSO_Axes_t*)r->sensorData;
  r->len = 0;
  for(uint32_t i=0; i<10; i++)
  {
    status = 0;
    do
    {
      LSM6DSO_ACC_Get_DRDY_Status(r->lsm, &status);
    } while(!status);
    LSM6DSO_GYRO_GetAxes(r->lsm, &pSensor[r->len]);
    LSM6DSO_ACC_GetAxes(r->lsm, &pSensor[r->len+1]);
    r->len += 2;
  }
  return HAL_OK;
}

uint8_t RotationDetected(RegistratorData *r)
{
  LSM6DSO_Axes_t *gyro = (LSM6DSO_Axes_t*)r->sensorData;
  int32_t average = 0;
  for(uint32_t i=0; i<(r->len); i+=2)
  {
    average += gyro[i].x;
  }
  average /= ((int32_t)r->len / 2);
  if((average > 2500) || (average < -2500))
    return 1;
  return 0;
}*/

////////////////////////////////////////////////////////////////////////////////

uint32_t Poll_Sensor(RegistratorData *r)
{
  uint8_t status;
  LSM6DSO_AxesRaw_t *pSensor = (LSM6DSO_AxesRaw_t*)r->sensorData;
  r->len = 0;
  r->avgGyro = 0;
  for(uint32_t i=0; i<10; i++)
  {
    status = 0;
    do
    {
      LSM6DSO_ACC_Get_DRDY_Status(r->lsm, &status);
    } while(!status);
    LSM6DSO_GYRO_GetAxesRaw(r->lsm, &pSensor[r->len]);
    r->avgGyro += pSensor[r->len].x;
    LSM6DSO_ACC_GetAxesRaw(r->lsm, &pSensor[r->len+1]);
    r->len += 2;
  }
  r->avgGyro /= ((int32_t)r->len / 2);
  if(r->avgGyro < 0)
    r->avgGyro = -(r->avgGyro);
  return HAL_OK;
}

uint8_t RotationDetected(RegistratorData *r)
{
  if(r->avgGyro > GYRO_THRESHOLD)
    return 1;
  return 0;
}

uint32_t HandleSensorData(RegistratorData *r)
{
  float sensitivity;
  LSM6DSO_GYRO_GetSensitivity(r->lsm, &sensitivity);
  int32_t rate = (int32_t)(r->avgGyro * sensitivity * 60 / 360000);
  if(r->rot.maxRate < rate)
    r->rot.maxRate = rate;
  /*LSM6DSO_AxesRaw_t *acc = (LSM6DSO_AxesRaw_t*)&(r->sensorData[1]);
  LSM6DSO_AxesRaw_t avgAcc = {0};
  for(uint32_t i=0; i<(r->len); i+=2)
  {
    avgAcc.x += acc[i].x;
    avgAcc.y += acc[i].y;
    avgAcc.z += acc[i].z;
  }
  int32_t n = (int32_t)r->len / 2;
  avgAcc.x /= n; avgAcc.y /= n; avgAcc.z /= n;
  int32_t vibr = (int32_t)(sqrt(avgAcc.x*avgAcc.x + avgAcc.y*avgAcc.y + avgAcc.z*avgAcc.z));
  LSM6DSO_ACC_GetSensitivity(r->lsm, &sensitivity);
  vibr = (int32_t)(vibr * sensitivity);
  if(r->rot.maxVibr < vibr)
    r->rot.maxVibr = vibr;*/
  return HAL_OK;
}

uint32_t SaveParamOnEEPROM(RegistratorData *r)
{
  return HAL_OK;
}
  
uint32_t RTC_SubTimeDateSec(RTC_DateTime *dt1, RTC_DateTime *dt2)
{
  return 0;
}

uint32_t RTC_GetTimeDate(RTC_DateTime *dt)
{    
  RTC_TimeTypeDef sTime = {0};    
  RTC_DateTypeDef sDate = {0};
  HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);    
  HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN); // ????? ??????? ????? GetTime
  dt->year = sDate.Year;
  dt->month = sDate.Month;
  dt->day = sDate.Date;
  dt->hour = sTime.Hours;
  dt->minutes = sTime.Minutes;
  dt->seconds = sTime.Seconds;
  return HAL_OK;
}

void RTC_SetTimeDate(RTC_DateTime *dt)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  sTime.Hours = dt->hour;
  sTime.Minutes = dt->minutes;
  sTime.Seconds = dt->seconds;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = dt->month;
  sDate.Date = dt->day;
  sDate.Year = dt->year; // ??? ????????? ????? (00..99)
  HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
}
