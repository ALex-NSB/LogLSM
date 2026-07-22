/* ==== CLAUDE 17.07 11:14 — версия 26.07.17 11:14. ШАГ (б) ЗАВЕРШЁН: вибрация
 * подтверждена на железе (vib1/vib2 пик+RMS ненулевые, физ. осмысленные).
 * Временная диагностика reserved убрана — reserved снова 0 (спека v2).
 * ==== CLAUDE 17.07 10:49 — версия 26.07.17 10:49. Диагностика reserved:
 * +40=0xBEEF(константа) / +42=vibCount / +44=r->len. Разводит «свежий Data.c?»
 * от «работает ли vib-цикл». Прошить после Clean/Build, см. хвост SaveParamOnEEPROM.
 * ==== CLAUDE 17.07 10:28 — версия 26.07.17 10:28 (маркер свежести Data.c:
 * реальная правка шапки → Ninja обязан пересобрать Data.c.obj, не «no work to
 * do»). Содержимое ниже — v2 + вибрация (шаг б) + временная диагностика
 * reserved[40..45]. Прошить ТОЛЬКО после Project→Clean(LOGLSMA)→rebuild, в
 * консоли убедиться в «Building … Data.c.obj». ====
 * ==== CLAUDE 13.07 19:30 — ШАГ (б): vib1/vib2 (пик+RMS) добавлены в запись
 * SaveParamOnEEPROM (v2, 48 байт). Шаг (а) [цикл накопления в HandleSensorData]
 * подтверждён на железе — версия 19:10 совпала, регистратор отвечал.
 * См. actual/session_notes_2026-07-13_v2_record_vibration.md §6.
 * Откат к шагу (а) — reserved/vib1..2 занулить обратно; откат в ноль —
 * Data_CLAUDE_new.c (корень репо) = версия совсем без вибрации. ==== */
#include "Data.h"
#include "rtc.h"
#include "math.h"
#include "globals.h"
#include "ltp.h"
#include <string.h>

/* Накопление вибрации по ОДНОМУ сэмплу accel (18.07.2026, вынесено из
 * HandleSensorData для ПЛОТНОЙ выборки): канал 1 = AC модуля |a| (EMA-DC),
 * канал 2 = jerk |Δ|a||. Пик+RMS обоих. */
static void vibAccumulate(RegistratorData *r, const LSM6DSO_AxesRaw_t *a)
{
  /* АВТОРЕЙНДЖ (18.07.2026): сэмпл у потолка шкалы (клип, |raw|>30000 из
   * 32767) → удваиваем шкалу 2→4→8→16 и обновляем чувствительность. Копим
   * в МГ — аккумуляторы неразрывны при смене шкалы. */
  int16_t mx = a->x < 0 ? -a->x : a->x;
  int16_t my = a->y < 0 ? -a->y : a->y;
  int16_t mz = a->z < 0 ? -a->z : a->z;
  int16_t mmax = mx > my ? mx : my; if (mz > mmax) mmax = mz;
  if (mmax > 30000 && r->accFs < 16u)
  {
    r->accFs = (uint8_t)(r->accFs * 2u);
    LSM6DSO_ACC_SetFullScale(r->lsm, r->accFs);
    LSM6DSO_ACC_GetSensitivity(r->lsm, &r->accSensMg);
  }
  if (r->accSensMg <= 0.0f) r->accSensMg = 0.061f;   /* страховка ±2g */
  float ax = (float)a->x * r->accSensMg,
        ay = (float)a->y * r->accSensMg,
        az = (float)a->z * r->accSensMg;
  float amag = sqrtf(ax*ax + ay*ay + az*az);   /* МГ */
  if (!r->vibDcInit) { r->vibDc = amag; r->vibDcInit = 1u; }
  float dev = amag - r->vibDc; if (dev < 0.0f) dev = -dev;
  r->vibDc += (amag - r->vibDc) * VIB_DC_ALPHA;
  if (dev > r->vibPeak) r->vibPeak = dev;    /* «пики» — по всем сэмплам */
  r->vibCount += 1u;

  /* «УРОВЕНЬ» = РОБАСТНАЯ МЕДИАНА |dev| (19.07.2026, по ТЗ «с ударами и без —
   * одинаков»). Мультипликативный median-трекер: шаг ±β·vibLvl к текущему
   * значению. Медиана не сдвигается редкими выбросами (пока ударных сэмплов
   * <50%), сколько бы удар ни звенел — на длинном цикле их единицы. Порогов и
   * вырезок не нужно; удар полностью уходит в vibPeak/«пики». */
  if (!r->vibLvlInit) { r->vibLvl = (dev > 1.0f) ? dev : 1.0f; r->vibLvlInit = 1u; }
  else
  {
    float b = r->vibLvl * VIB_LVL_BETA;
    r->vibLvl += (dev > r->vibLvl) ? b : -b;
    if (r->vibLvl < 1.0f) r->vibLvl = 1.0f;
  }
  if (r->prevAmagInit)
  {
    float jerk = amag - r->prevAmag; if (jerk < 0.0f) jerk = -jerk;
    if (jerk > r->jerkPeak) r->jerkPeak = jerk;
    r->jerkSumSq += jerk * jerk;
  }
  else r->prevAmagInit = 1u;
  r->prevAmag = amag;
}

uint32_t Poll_Sensor(RegistratorData *r)
{
  uint8_t status;
  LSM6DSO_AxesRaw_t *pSensor = (LSM6DSO_AxesRaw_t*)r->sensorData;
  r->len = 0;
  r->avgGyro = 0;
  int32_t sx = 0, sy = 0, sz = 0;
  for(uint32_t i=0; i<3; i++)
  {
    status = 0;
    uint32_t t0 = HAL_GetTick();
    do
    {
      LSM6DSO_GYRO_Get_DRDY_Status(r->lsm, &status);
      /* ПЛОТНАЯ ВЫБОРКА ВИБРАЦИИ (18.07.2026): пока ждём DRDY гироскопа
       * (~80 мс), ловим КАЖДЫЙ свежий сэмпл accel (104 Гц) — иначе удары
       * (мс-события) падали между редкими опросами (3 сэмпла/240 мс) и
       * канал 2 не превышал уровень. Только в фазе вращения. */
      if (r->state == REG_STATE_ROTATING)
      {
        uint8_t aSt = 0;
        LSM6DSO_ACC_Get_DRDY_Status(r->lsm, &aSt);
        if (aSt)
        {
          LSM6DSO_AxesRaw_t aRaw;
          LSM6DSO_ACC_GetAxesRaw(r->lsm, &aRaw);
          vibAccumulate(r, &aRaw);
        }
      }
    } while(!status && (HAL_GetTick() - t0) < 500u);
    LSM6DSO_GYRO_GetAxesRaw(r->lsm, &pSensor[r->len]);
    sx += pSensor[r->len].x;
    sy += pSensor[r->len].y;
    sz += pSensor[r->len].z;
    LSM6DSO_ACC_GetAxesRaw(r->lsm, &pSensor[r->len+1]);
    r->len += 2;
  }
  if (r->len == 0) r->len = 2;
  int32_t n = (int32_t)r->len / 2;
  sx /= n; sy /= n; sz /= n;
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

/* МНОГОТОЧЕЧНАЯ КАЛИБРОВКА СКОРОСТИ (18.07.2026, замена одноточечного
 * SPEED_CALIB_COEF). Таблица снята на стенде: 179 циклов «Случайно ±» 40–400
 * об/мин (журнал LogLSM_stend_20260718_202013.txt), коэффициент = задано/
 * измерено, узлы ПО ИЗМЕРЕННОМУ rpm, между узлами — линейная интерполяция.
 * Результат на калибровочном наборе: средняя ошибка −3.2% → −0.05%
 * (в зоне 75–340 остаток +0.1%). Ниже ~70 rpm крупный коэффициент компенсирует
 * не гироскоп, а поздний старт детекта; выше ~333 rpm гироскоп у клипа FS
 * ±2000 dps — предел измерения варианта A. */
static const struct { float r; float k; } kSpeedCal[] = {
  {  50.0f, 1.05f  },   /* 19.07: было 1.16/1.079 — включали детект-задержку
                         * ДЛИННЫХ циклов и ПЕРЕкомпенсировали короткие
                         * (+4..8% на 51/73/77). Теперь чисто гироскопная
                         * поправка: скорость меряет скорость. */
  {  70.0f, 1.045f },
  {  90.0f, 1.040f },
  { 115.0f, 1.032f },
  { 150.0f, 1.020f },
  { 195.0f, 1.016f },
  { 245.0f, 1.017f },
  { 292.0f, 1.012f },   /* 1.022→1.012 по контрольному прогону 18.07 23:06
                         * (единственные +2% были на 276 об/мин) */
  { 330.0f, 1.022f },   /* дальше клип FS — коэффициент замораживаем */
};
#define SPEED_CAL_N  (sizeof(kSpeedCal)/sizeof(kSpeedCal[0]))

static float speedCalCoef(float rpm)
{
  if (rpm <= kSpeedCal[0].r)              return kSpeedCal[0].k;
  if (rpm >= kSpeedCal[SPEED_CAL_N-1].r)  return kSpeedCal[SPEED_CAL_N-1].k;
  for (uint32_t i = 1; i < SPEED_CAL_N; i++)
    if (rpm <= kSpeedCal[i].r)
    {
      const float x0 = kSpeedCal[i-1].r, y0 = kSpeedCal[i-1].k;
      const float x1 = kSpeedCal[i].r,   y1 = kSpeedCal[i].k;
      return y0 + (y1 - y0) * (rpm - x0) / (x1 - x0);
    }
  return 1.0f;
}

uint32_t HandleSensorData(RegistratorData *r)
{
  float sensitivity;
  LSM6DSO_GYRO_GetSensitivity(r->lsm, &sensitivity);
  float rpmRaw = r->avgGyro * sensitivity * 60.0f / 360000.0f;
  int32_t rate = (int32_t)(rpmRaw * speedCalCoef(rpmRaw) + 0.5f);
  if(r->rot.maxRate < rate)
    r->rot.maxRate = rate;
  if (RotationDetected(r))
  {
    if (r->rotWarmup)
      r->rotWarmup = 0;
    else
    {
      r->rateSum   += rate;
      r->rateCount += 1u;
      /* Вибрация здесь БОЛЬШЕ НЕ копится (18.07.2026): плотная выборка идёт в
       * Poll_Sensor (каждый DRDY accel @104 Гц, см. vibAccumulate) — 3 редких
       * сэмпла отсюда давали пропуск ударов. Двойной учёт исключён. */

      /* ДЕТЕКТОР ПОЛОК (19.07.2026): скорость в полосе ±max(FRAC·avg,MIN) —
       * та же полка (обновляем её max); выход за полосу PLATEAU_DEBOUNCE
       * выборок подряд — граница: фиксируем max прошлой полки в subMax[],
       * начинаем новую. Последнюю полку финализирует PushCycleRecord. */
      if (!r->plateauInit)
      {
        r->plateauInit = 1u; r->plateauAvg = (float)rate;
        r->plateauMax = rate; r->plateauChg = 0u;
      }
      else
      {
        float band = r->plateauAvg * PLATEAU_BAND_FRAC;
        if (band < PLATEAU_BAND_MIN) band = PLATEAU_BAND_MIN;
        float dev = (float)rate - r->plateauAvg; if (dev < 0.0f) dev = -dev;
        if (dev > band)
        {
          if (++r->plateauChg >= PLATEAU_DEBOUNCE)
          {
            if (r->subCount < PLATEAU_MAX_N)
              r->subMax[r->subCount++] = (r->plateauMax > 65535) ? 65535u
                                          : (uint16_t)r->plateauMax;
            r->plateauAvg = (float)rate; r->plateauMax = rate; r->plateauChg = 0u;
          }
        }
        else
        {
          r->plateauChg = 0u;
          if (rate > r->plateauMax) r->plateauMax = rate;
          r->plateauAvg += ((float)rate - r->plateauAvg) * 0.2f;
        }
      }
    }
  }
  return HAL_OK;
}

/* Flash record write — SaveParamOnEEPROM (v2, 48 байт, БЕЗ вибрации/диагностики) */
#define LOG_START_PAGE    1u
#define RECORDS_PER_PAGE  5u
#define RECORD_BYTES      48u
#define REC_VERSION       2u
#define VARIANT_FLAG_A    0x0Au

static uint32_t s_writePage = 0u;
static uint8_t  s_writeSlot = 0u;

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

/* Секунды (база 2000-01-01) → RTC_DateTime; обратное к rtcToSec (18.07.2026,
 * для пересадки штампов цикла при переводе часов — cmdSetDateTime). */
static void secToRtc(uint32_t sec, RTC_DateTime *dt)
{
  static const uint8_t kDpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint32_t days = sec / 86400U;
  uint32_t rem  = sec % 86400U;
  dt->hour    = (uint8_t)(rem / 3600U);
  dt->minutes = (uint8_t)((rem % 3600U) / 60U);
  dt->seconds = (uint8_t)(rem % 60U);
  uint8_t yy = 0;
  for (;;)
  {
    uint32_t ydays = ((yy % 4U) == 0U) ? 366U : 365U;   /* 2000..2099: /4 достаточно */
    if (days < ydays) break;
    days -= ydays;
    yy++;
  }
  uint8_t isLeap = ((yy % 4U) == 0U) ? 1U : 0U;
  uint8_t m = 1;
  for (; m <= 12U; m++)
  {
    uint32_t md = kDpm[m - 1U] + ((m == 2U && isLeap) ? 1U : 0U);
    if (days < md) break;
    days -= md;
  }
  dt->year  = yy;
  dt->month = m;
  dt->day   = (uint8_t)(days + 1U);
}

/* out = dt минус sec секунд (не раньше нуля базы). */
void RtcSubSecFrom(const RTC_DateTime *dt, uint32_t sec, RTC_DateTime *out)
{
  uint32_t s = rtcToSec((RTC_DateTime *)dt);
  secToRtc((s > sec) ? (s - sec) : 0U, out);
}

static void flashFindWritePos(void)
{
  uint8_t  buf[4];
  uint32_t ts;
  for (uint32_t page = LOG_START_PAGE; page < 65536u; page++) {
    P25Qx_QPI_Read(&flash, page << 8u, 4u, buf);
    memcpy(&ts, buf, 4u);
    if (ts == 0xFFFFFFFFu) { s_writePage = page; s_writeSlot = 0u; return; }
    for (uint8_t slot = 1u; slot < RECORDS_PER_PAGE; slot++) {
      P25Qx_QPI_Read(&flash, (page << 8u) + slot * RECORD_BYTES, 4u, buf);
      memcpy(&ts, buf, 4u);
      if (ts == 0xFFFFFFFFu) { s_writePage = page; s_writeSlot = slot; return; }
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
  float    rpm_max       = (float)r->rot.maxRate;
  float    rpm_avg       = (float)r->avgRate;

  /* Вибрация УЖЕ В МГ (18.07.2026, авторейндж: vibAccumulate копит в мг с
   * учётом текущей шкалы) — чувствительностью здесь НЕ умножаем. */
  float invN = (r->vibCount) ? (1.0f / (float)r->vibCount) : 0.0f;
  float vib1_peak = r->vibPeak;
  float vib1_rms  = r->vibLvl;      /* «уровень» = робастная медиана |dev| (19.07.2026) */
  float vib2_peak = r->jerkPeak;
  float vib2_rms  = sqrtf(r->jerkSumSq * invN);

  memcpy(rec + 0,  &tsStart,       4u);
  memcpy(rec + 4,  &duration,      4u);
  memcpy(rec + 8,  &durationTotal, 4u);
  memcpy(rec + 12, &rpm_max,       4u);
  memcpy(rec + 16, &rpm_avg,       4u);
  memcpy(rec + 20, &vib1_peak,     4u);
  memcpy(rec + 24, &vib1_rms,      4u);
  memcpy(rec + 28, &vib2_peak,     4u);
  memcpy(rec + 32, &vib2_rms,      4u);
  /* rec[36] temperature, rec[37] status = 0 (memset, не в этом шаге) */
  rec[38] = REC_VERSION;      /* = 2 */
  rec[39] = VARIANT_FLAG_A;   /* = 0x0A */

  /* reserved[40..45] = 0 (memset), по спеке v2. Диагностика убрана 18.07 вечер:
   * подтверждено vibCount≈31/с, пик в мг сходится, шкала ±2g, state=ROTATING —
   * «нулевой» прогон был артефактом промежуточной прошивки 15:08. */

  /* rec[40..45] reserved = 0 (memset) — по спеке v2. Диагностика 0xBEEF/vibCount/
   * len убрана 17.07 11:14: вибрация подтверждена на железе (vib1/vib2 ненулевые,
   * vibCount~105-108, len=6, 0xBEEF доезжал → свежий Data.c исполняется).
   * Свежесть прошивки теперь проверяется индикатором версии (com.c) + надёжной
   * clean-rebuild сборкой в VS Code (см. CLAUDE.md, регламент 17.07). */
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
  flashFindWritePos();
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
