#include "Data.h"
#include "rtc.h"
#include "math.h"
/* FlashQ.h исключён: дублирует union StatusRegBits из p25q128.h
 * и не знает о QPI-режиме (sCommand.InstructionMode не инициализирован).
 * Все операции Flash — через P25Qx_QPI_* из globals.h / p25q128.h. */
#include "globals.h"
#include "ltp.h"
#include <string.h>

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
  /* 3 samplesa x 80 ms (ODR=12.5 Hz) = 240 ms per call */
  for(uint32_t i=0; i<3; i++)
  {
    status = 0;
    /* Ожидание DRDY С ТАЙМАУТОМ (04.07.2026): без него `while(!status)` при
     * пропаже data-ready (сбой датчика/ВЧ-вибрация/рассинхрон ODR после
     * Wake-Up-детекта) крутился ВЕЧНО → главный цикл заблокирован, ComPoll не
     * зовётся, вся LTP-связь умирала (симптом «регистратор завис на высоких
     * оборотах», лог 09:21/09:25). Теперь не виснем: не пришёл за 500 мс —
     * берём что есть и идём дальше, устройство остаётся на связи. HAL_GetTick
     * валиден: Poll_Sensor зовётся только в бодрых фазах (CONFIRM/ROTATING),
     * тик не приостановлен. */
    uint32_t t0 = HAL_GetTick();
    do
    {
      LSM6DSO_ACC_Get_DRDY_Status(r->lsm, &status);
    } while(!status && (HAL_GetTick() - t0) < 500u);
    LSM6DSO_GYRO_GetAxesRaw(r->lsm, &pSensor[r->len]);
    r->avgGyro += pSensor[r->len].x;
    LSM6DSO_ACC_GetAxesRaw(r->lsm, &pSensor[r->len+1]);
    r->len += 2;
  }
  if (r->len == 0) r->len = 2;   /* страховка от деления на ноль ниже */
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
  return HAL_OK;
}

/* ============================================================
 * Flash record write — SaveParamOnEEPROM
 *
 * Record layout (24 bytes, little-endian), matches LOGLSMW parser:
 *   [0..3]   tsStart       uint32  seconds since 2000-01-01
 *   [4..7]   duration      uint32  cycle duration, seconds
 *   [8..11]  durationTotal uint32  total accumulator, seconds
 *   [12..15] maxVibro      float   max vibration (0.0 until accel enabled)
 *   [16..19] maxRpm        float   max speed, rpm
 *   [20..21] reserved      0x0000
 *   [22..23] crc16         CRC16-CCITT over bytes 0..21
 *
 * Page 0 = device header, log starts at page 1.
 * 10 records per page. Page is erased before first slot write.
 * Write position is found by linear page scan at startup.
 * ============================================================ */

#define LOG_START_PAGE    1u
#define RECORDS_PER_PAGE  10u
#define RECORD_BYTES      24u

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

/* Find first free write position.
 * Reads tsStart (4 bytes) of the first slot of each page.
 * If 0xFFFFFFFF — page is empty, scan its slots to find exact slot.
 * If a page has all 10 slots filled — move to next page. */
static void flashFindWritePos(void)
{
  uint8_t  buf[4];
  uint32_t ts;

  for (uint32_t page = LOG_START_PAGE; page < 65536u; page++) {
    P25Qx_QPI_Read(&flash, page << 8u, 4u, buf);
    memcpy(&ts, buf, 4u);
    if (ts == 0xFFFFFFFFu) {
      /* Whole page empty */
      s_writePage = page;
      s_writeSlot = 0u;
      return;
    }
    /* Page has at least slot 0 — find first empty slot */
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
  /* Flash full — sentinel > max valid page index (65535) */
  s_writePage = 0x10000u;
  s_writeSlot = 0u;
}

uint32_t SaveParamOnEEPROM(RegistratorData *r)
{
  /* Позицию записи ищем ЗАНОВО по фактическому состоянию Flash при КАЖДОЙ
   * записи — не кэшируем между вызовами (04.07.2026, по прямому указанию
   * пользователя). Так внешнее стирание чипа (или любой сбой) отражается
   * немедленно: запись возвращается на первую реально свободную страницу
   * без перезагрузки МК. Записи редкие (раз в цикл вращения), поэтому цена
   * скана несущественна. */
  flashFindWritePos();

  if (s_writePage >= 0x10000u)
    return HAL_ERROR;

  uint8_t rec[RECORD_BYTES];
  memset(rec, 0, sizeof(rec));

  uint32_t tsStart       = rtcToSec(&r->rot.startTimeStamp);
  uint32_t duration      = RTC_SubTimeDateSec(&r->rot.stopTimeStamp,
                                              &r->rot.startTimeStamp);
  uint32_t durationTotal = r->totalSec;
  float    maxVibro      = (float)r->rot.maxVibr;
  float    maxRpm        = (float)r->rot.maxRate;

  memcpy(rec + 0,  &tsStart,       4u);
  memcpy(rec + 4,  &duration,      4u);
  memcpy(rec + 8,  &durationTotal, 4u);
  memcpy(rec + 12, &maxVibro,      4u);
  memcpy(rec + 16, &maxRpm,        4u);
  /* rec[20..21] = 0x00 (reserved, zeroed by memset) */
  uint16_t crc = ltp_crc16(rec, 22u);
  memcpy(rec + 22, &crc, 2u);

  if (s_writeSlot == 0u)
    P25Qx_QPI_ErasePage(&flash, s_writePage << 8u);

  uint32_t addr = (s_writePage << 8u) + s_writeSlot * RECORD_BYTES;
  P25Qx_QPI_ProgramPage(&flash, addr, rec, RECORD_BYTES);

  /* Инкремент позиции больше не ведём: следующий вызов SaveParamOnEEPROM
   * найдёт первую свободную позицию сам (flashFindWritePos выше) — только что
   * записанный слот к тому моменту уже занят, скан вернёт следующий. */
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
  /* No-op: totalSec is already written to Flash by SaveParamOnEEPROM
   * (field durationTotal, bytes 8..11 of every 24-byte record).
   * LoadTotalSec restores it from the last written record on next boot. */
  (void)r;
  return HAL_OK;
}

uint32_t LoadTotalSec(RegistratorData *r)
{
  /* Restore accumulated lifetime totalSec from the last Flash record.
   * Needed so that r->totalSec continues from where it left off after
   * power cycle — without this it would restart from 0 every session. */
  flashFindWritePos();   /* всегда по факту Flash, без кэша (04.07.2026) */

  /* Flash is empty — first ever use */
  if (s_writePage == LOG_START_PAGE && s_writeSlot == 0u) {
    r->totalSec = 0u;
    return HAL_OK;
  }

  /* Find the last written record position */
  uint32_t lastPage;
  uint8_t  lastSlot;
  if (s_writePage >= 0x10000u) {
    /* Flash full */
    lastPage = 65535u;
    lastSlot = (uint8_t)(RECORDS_PER_PAGE - 1u);
  } else if (s_writeSlot == 0u) {
    /* Write pointer is at start of a page → last record is on prev page, slot 9 */
    lastPage = s_writePage - 1u;
    lastSlot = (uint8_t)(RECORDS_PER_PAGE - 1u);
  } else {
    lastPage = s_writePage;
    lastSlot = s_writeSlot - 1u;
  }

  /* Read the full 24-byte record and verify CRC before trusting durationTotal.
   * Old-format records (written before durationTotal field existed at [8..11])
   * will fail CRC and be treated as empty — safe fallback to totalSec = 0. */
  uint8_t  rec[RECORD_BYTES];
  uint32_t addr = (lastPage << 8u) + (uint32_t)lastSlot * RECORD_BYTES;
  P25Qx_QPI_Read(&flash, addr, RECORD_BYTES, rec);

  uint16_t stored_crc;
  memcpy(&stored_crc, rec + 22u, 2u);
  if (stored_crc != ltp_crc16(rec, 22u)) {
    /* CRC mismatch: old-format record or corruption — start from zero */
    r->totalSec = 0u;
    return HAL_OK;
  }

  uint32_t total;
  memcpy(&total, rec + 8u, 4u);
  /* 0xFFFFFFFF = record slot was never written */
  r->totalSec = (total == 0xFFFFFFFFu) ? 0u : total;
  return HAL_OK;
}
