/* ==== МАРКЕР ЗАПИСИ CLAUDE 13.07 v2 — заменённый файл. Вибрация+BEEF внутри. ==== */
#include "Data.h"
#include "rtc.h"
#include "math.h"
/* FlashQ.h исключён: дублирует union StatusRegBits из p25q128.h
 * и не знает о QPI-режиме (sCommand.InstructionMode не инициализирован).
 * Все операции Flash — через P25Qx_QPI_* из globals.h / p25q128.h. */
#include "globals.h"
#include "ltp.h"
#include <string.h>

uint32_t Poll_Sensor(RegistratorData *r)
{
  uint8_t status;
  LSM6DSO_AxesRaw_t *pSensor = (LSM6DSO_AxesRaw_t*)r->sensorData;
  r->len = 0;
  r->avgGyro = 0;
  int32_t sx = 0, sy = 0, sz = 0;   /* ЗНАКОВЫЕ суммы по 3 осям гироскопа */
  /* 3 samples x 80 ms (ODR=12.5 Hz) = 240 ms per call */
  for(uint32_t i=0; i<3; i++)
  {
    status = 0;
    /* Ожидание DRDY ГИРОСКОПА с таймаутом 500 мс (04.07/11.07.2026). */
    uint32_t t0 = HAL_GetTick();
    do
    {
      LSM6DSO_GYRO_Get_DRDY_Status(r->lsm, &status);
    } while(!status && (HAL_GetTick() - t0) < 500u);
    LSM6DSO_GYRO_GetAxesRaw(r->lsm, &pSensor[r->len]);
    sx += pSensor[r->len].x;
    sy += pSensor[r->len].y;
    sz += pSensor[r->len].z;
    LSM6DSO_ACC_GetAxesRaw(r->lsm, &pSensor[r->len+1]);
    r->len += 2;
  }
  if (r->len == 0) r->len = 2;   /* страховка от деления на ноль ниже */
  int32_t n = (int32_t)r->len / 2;
  sx /= n; sy /= n; sz /= n;
  /* Модуль вектора угловой скорости = sqrt(x^2+y^2+z^2), устойчив к ориентации. */
  float mag = sqrtf((float)sx * (float)sx +
                    (float)sy * (float)sy +
                    (float)sz * (float)sz);
  r->avgGyro = (int32_t)mag;
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
  int32_t rate = (int32_t)(r->avgGyro * sensitivity * 60.0f / 360000.0f
                           * SPEED_CALIB_COEF + 0.5f);
  if(r->rot.maxRate < rate)      /* ПИКОВОЕ — короткие выбросы (диагностика) */
    r->rot.maxRate = rate;
  if (RotationDetected(r))
  {
    if (r->rotWarmup)
      r->rotWarmup = 0;   /* первая выборка цикла — разгонное окно, пропускаем */
    else
    {
      r->rateSum   += rate;
      r->rateCount += 1u;
      /* ВИБРАЦИЯ: AC-компонента модуля accel по 3 сырым отсчётам этого опроса.
       * accel-отсчёты — на НЕЧЁТНЫХ индексах sensorData (чётные — гироскоп). */
      LSM6DSO_AxesRaw_t *ps = (LSM6DSO_AxesRaw_t*)r->sensorData;
      for (uint32_t i = 1u; i < r->len; i += 2u)
      {
        float ax = (float)ps[i].x, ay = (float)ps[i].y, az = (float)ps[i].z;
        float amag = sqrtf(ax*ax + ay*ay + az*az);
        if (!r->vibDcInit) { r->vibDc = amag; r->vibDcInit = 1u; }
        float dev = amag - r->vibDc; if (dev < 0.0f) dev = -dev;
        r->vibDc += (amag - r->vibDc) * VIB_DC_ALPHA;
        if (dev > r->vibPeak) r->vibPeak = dev;
        r->vibSumSq += dev * dev;
        r->vibCount += 1u;
        /* КАНАЛ 2 (вариант A): импульсность = |Δ|a|| между соседними отсчётами. */
        if (r->prevAmagInit)
        {
          float jerk = amag - r->prevAmag; if (jerk < 0.0f) jerk = -jerk;
          if (jerk > r->jerkPeak) r->jerkPeak = jerk;
          r->jerkSumSq += jerk * jerk;
        }
        else r->prevAmagInit = 1u;
        r->prevAmag = amag;
      }
    }
  }
  return HAL_OK;
}

/* ============================================================
 * Flash record write — SaveParamOnEEPROM (запись v2, 48 байт)
 * см. data_format_spec_v1.md «Структура записи v2». ============ */

#define LOG_START_PAGE    1u
#define RECORDS_PER_PAGE  5u    /* v2: 256/48 = 5 записей на страницу */
#define RECORD_BYTES      48u   /* v2 (12.07.2026), было 24 */
#define REC_VERSION       2u
#define VARIANT_FLAG_A    0x0Au

static uint32_t s_writePage = 0u;
static uint8_t  s_writeSlot = 0u;

/* Seconds since 2000-01-01 00:00:00 */
static uint32_t rtcToSec(const RTC_DateTime *dt)
{
  static const uint8_t kDpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t  yy     = dt->year;
  uint8_t  isLeap = (yy % 4U == 0U) ? 1U : 0U;
  uint32_t days   = (uint32_t)yy * 365U;
  if (yy > 0U) days += (uint32_t)((yy - 1U) / 4U) + 1U;
  for (uint8_t m = 1U; m < dt->month; m++) {
    days += kDpm[m - 1U];
    if (m == 2U && isLeap) days += 1U;
  }
  days += (uint32_t)dt->day - 1U;
  return days * 86400U
       + (uint32_t)dt->hour    * 3600U
       + (uint32_t)dt->minutes *   60U
       + (uint32_t)dt->seconds;
}

uint32_t RTC_SubTimeDateSec(RTC_DateTime *dt1, RTC_DateTime *dt2)
{
  uint32_t s1 = rtcToSec(dt1);
  uint32_t s2 = rtcToSec(dt2);
  return (s1 >= s2) ? (s1 - s2) : 0U;
}

/* Find first free write position. */
static void flashFindWritePos(void)
{
  uint8_t  buf[4];
  uint32_t ts;

  for (uint32_t page = LOG_START_PAGE; page < 65536u; page++) {
    P25Qx_QPI_Read(&flash, page << 8u, 4u, buf);
    memcpy(&ts, buf, 4u);
    if (ts == 0xFFFFFFFFu) {
      s_writePage = page;
      s_writeSlot = 0u;
      return;
    }
    for (uint8_t slot = 1u; slot < RECORDS_PER_PAGE; slot++) {
      P25Qx_QPI_Read(&flash, (page << 8u) + slot * RECORD_BYTES, 4u, buf);
      memcpy(&ts, buf, 4u);
      if (ts == 0xFFFFFFFFu) {
        s_writePage = page;
        s_writeSlot = slot;
        return;
      }
    }
  }
  s_writePage = 0x10000u;
  s_writeSlot = 0u;
}

uint32_t SaveParamOnEEPROM(RegistratorData *r)
{
  flashFindWritePos();

  if (s_writePage >= 0x10000u)
    return HAL_ERROR;

  uint8_t rec[RECORD_BYTES];
  memset(rec, 0, sizeof(rec));

  uint32_t tsStart       = rtcToSec(&r->rot.startTimeStamp);
  uint32_t duration      = RTC_SubTimeDateSec(&r->rot.stopTimeStamp,
                                              &r->rot.startTimeStamp);
  uint32_t durationTotal = r->totalSec;
  float    rpm_max       = (float)r->rot.maxRate;   /* ПИК скорости */
  float    rpm_avg       = (float)r->avgRate;       /* СРЕДНЕЕ — отчётная «Скорость» */

  /* Вибрация → mg: пик/RMS (LSB) × чувствительность accel. */
  float sens_mg = 0.061f;   /* fallback ±2g */
  LSM6DSO_ACC_GetSensitivity(r->lsm, &sens_mg);
  float invN = (r->vibCount) ? (1.0f / (float)r->vibCount) : 0.0f;
  float vib1_peak = r->vibPeak  * sens_mg;
  float vib1_rms  = sqrtf(r->vibSumSq  * invN) * sens_mg;
  float vib2_peak = r->jerkPeak * sens_mg;
  float vib2_rms  = sqrtf(r->jerkSumSq * invN) * sens_mg;

  memcpy(rec + 0,  &tsStart,       4u);
  memcpy(rec + 4,  &duration,      4u);
  memcpy(rec + 8,  &durationTotal, 4u);
  memcpy(rec + 12, &rpm_max,       4u);
  memcpy(rec + 16, &rpm_avg,       4u);
  memcpy(rec + 20, &vib1_peak,     4u);
  memcpy(rec + 24, &vib1_rms,      4u);
  memcpy(rec + 28, &vib2_peak,     4u);
  memcpy(rec + 32, &vib2_rms,      4u);
  /* rec[36] temperature, rec[37] status = 0 (memset) */
  rec[38] = REC_VERSION;      /* = 2 */
  rec[39] = VARIANT_FLAG_A;   /* = 0x0A */
  /* ⚠ ВРЕМЕННАЯ ДИАГНОСТИКА в reserved[40..45] (УБРАТЬ после разбора вибрации):
   *   [40..41] = МАРКЕР 0xBEEF (в дампе байты `EF BE`) — свежесть прошивки;
   *   [42..43] = vibCount (число выборок за цикл);
   *   [44..45] = r->len (ждём 6). */
  uint16_t dMark = 0xBEEFu;
  uint16_t dVib  = (r->vibCount > 65535u) ? 65535u : (uint16_t)r->vibCount;
  uint16_t dLen  = (r->len      > 65535u) ? 65535u : (uint16_t)r->len;
  memcpy(rec + 40, &dMark, 2u);
  memcpy(rec + 42, &dVib,  2u);
  memcpy(rec + 44, &dLen,  2u);
  uint16_t crc = ltp_crc16(rec, 46u);
  memcpy(rec + 46, &crc, 2u);

  if (s_writeSlot == 0u)
    P25Qx_QPI_ErasePage(&flash, s_writePage << 8u);

  uint32_t addr = (s_writePage << 8u) + s_writeSlot * RECORD_BYTES;
  P25Qx_QPI_ProgramPage(&flash, addr, rec, RECORD_BYTES);

  return HAL_OK;
}

uint32_t RTC_GetTimeDate(RTC_DateTime *dt)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  dt->year    = sDate.Year;
  dt->month   = sDate.Month;
  dt->day     = sDate.Date;
  dt->hour    = sTime.Hours;
  dt->minutes = sTime.Minutes;
  dt->seconds = sTime.Seconds;
  return HAL_OK;
}

void RTC_SetTimeDate(RTC_DateTime *dt)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  sTime.Hours   = dt->hour;
  sTime.Minutes = dt->minutes;
  sTime.Seconds = dt->seconds;
  sDate.Year    = dt->year;
  sDate.Month   = dt->month;
  sDate.Date    = dt->day;
  HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
}

uint32_t SaveTotalSec(RegistratorData *r)
{
  (void)r;
  return HAL_OK;
}

uint32_t LoadTotalSec(RegistratorData *r)
{
  flashFindWritePos();   /* всегда по факту Flash, без кэша */

  if (s_writePage == LOG_START_PAGE && s_writeSlot == 0u) {
    r->totalSec = 0u;
    return HAL_OK;
  }

  uint32_t lastPage;
  uint8_t  lastSlot;
  if (s_writePage >= 0x10000u) {
    lastPage = 65535u;
    lastSlot = (uint8_t)(RECORDS_PER_PAGE - 1u);
  } else if (s_writeSlot == 0u) {
    lastPage = s_writePage - 1u;
    lastSlot = (uint8_t)(RECORDS_PER_PAGE - 1u);
  } else {
    lastPage = s_writePage;
    lastSlot = s_writeSlot - 1u;
  }

  /* Читаем 48-байт v2 запись, CRC по offset 46. */
  uint8_t  rec[RECORD_BYTES];
  uint32_t addr = (lastPage << 8u) + (uint32_t)lastSlot * RECORD_BYTES;
  P25Qx_QPI_Read(&flash, addr, RECORD_BYTES, rec);

  uint16_t stored_crc;
  memcpy(&stored_crc, rec + 46u, 2u);
  if (stored_crc != ltp_crc16(rec, 46u)) {
    r->totalSec = 0u;
    return HAL_OK;
  }

  uint32_t total;
  memcpy(&total, rec + 8u, 4u);
  r->totalSec = (total == 0xFFFFFFFFu) ? 0u : total;
  return HAL_OK;
}
