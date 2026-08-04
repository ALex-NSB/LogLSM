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
#include "com.h"        /* data_flag_set_if_clear() — взвод флага при записи цикла */
#include "iflash.h"     /* iflash_journal_count(EVT_CLOCKZERO) — поколение часов */
#include "lsm6dso_temp.h" /* LSM6DSO_GetTemp() — температура IMU в запись цикла */
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
static const struct { float r; float k; } kSpeedCalDefault[] = {
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
#define SPEED_CAL_DEF_N  (sizeof(kSpeedCalDefault)/sizeof(kSpeedCalDefault[0]))

/* РАБОЧАЯ таблица в RAM: по умолчанию = вкомпилированная, но может быть
 * перезаписана поэкземплярной калибровкой со стр.123 (data_speedcal_load).
 * 03.08.2026 — раньше таблица была const, правилась только пересборкой. */
static float    s_speedR[IFLASH_SPEEDCAL_MAX];
static float    s_speedK[IFLASH_SPEEDCAL_MAX];
static uint16_t s_speedN = 0;

static void speedcal_use_default(void)
{
  s_speedN = SPEED_CAL_DEF_N;
  for (uint16_t i = 0; i < s_speedN; i++)
  { s_speedR[i] = kSpeedCalDefault[i].r; s_speedK[i] = kSpeedCalDefault[i].k; }
}

/* Загрузить таблицу калибровки скорости из стр.123, иначе — дефолт.
 * Вызывать на старте Service (после доступности внутр. Flash). */
void data_speedcal_load(void)
{
  IflashCfg cfg;
  if (iflash_cfg_read(&cfg) == 0 && cfg.speed_n >= 2u
      && cfg.speed_n <= IFLASH_SPEEDCAL_MAX)
  {
    s_speedN = cfg.speed_n;
    for (uint16_t i = 0; i < s_speedN; i++)
    { s_speedR[i] = cfg.speed_r[i]; s_speedK[i] = cfg.speed_k[i]; }
  }
  else
    speedcal_use_default();
}

/* Текущая таблица (для GET по LTP): копирует в r/k, возвращает число узлов. */
uint16_t data_speedcal_get(float *r, float *k)
{
  if (s_speedN == 0) speedcal_use_default();
  for (uint16_t i = 0; i < s_speedN; i++) { r[i] = s_speedR[i]; k[i] = s_speedK[i]; }
  return s_speedN;
}

/* Инициализировать cfg дефолтом, если валидного образа на стр.123 нет.
 * Читает существующий (сохраняя чужие поля), иначе — пустой с magic. */
static void cfg_read_or_init(IflashCfg *cfg)
{
  if (iflash_cfg_read(cfg) == 0) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->magic = IFLASH_CFG_MAGIC;
  cfg->version = IFLASH_CFG_VER;
  cfg->speed_n = 0;      /* скорость — дефолт */
  cfg->rtc_valid = 0;    /* RTC — дефолт */
}

/* Записать новую таблицу: read-modify-write (rtc-поля сохраняем) + применить. */
int data_speedcal_set(const float *r, const float *k, uint16_t n)
{
  if (n < 2u || n > IFLASH_SPEEDCAL_MAX) return -1;
  IflashCfg cfg;
  cfg_read_or_init(&cfg);
  cfg.speed_n = n;
  for (uint16_t i = 0; i < n; i++) { cfg.speed_r[i] = r[i]; cfg.speed_k[i] = k[i]; }
  if (iflash_cfg_write(&cfg) != 0) return -1;
  data_speedcal_load();                /* перечитать → применить */
  return 0;
}

/* ---- Калибровка часов RTC (smooth-calib) ---------------------------------- */
#define RTC_CAL_STEP_PPM   0.95367432f     /* шаг маскирования импульса, ppm (окно 32 c) */
#define RTC_CAL_DEFAULT_PPM (-305.2f)      /* прежний фикс CALM=320 = −305 ppm */

static float s_rtcAppliedPpm = RTC_CAL_DEFAULT_PPM;

/* ppm → (CALP, CALM) и применить. net_ppm = (512*CALP − CALM)*step. */
static void rtc_apply_ppm(float ppm)
{
  int32_t pulses = (int32_t)(ppm / RTC_CAL_STEP_PPM + (ppm >= 0 ? 0.5f : -0.5f));
  uint32_t calp, calm;
  if (pulses >= 0) {                        /* ускорить */
    if (pulses > 512) pulses = 512;
    calp = (pulses == 0) ? RTC_SMOOTHCALIB_PLUSPULSES_RESET
                         : RTC_SMOOTHCALIB_PLUSPULSES_SET;
    calm = (pulses == 0) ? 0u : (uint32_t)(512 - pulses);
  } else {                                  /* замедлить */
    int32_t m = -pulses;
    if (m > 511) m = 511;
    calp = RTC_SMOOTHCALIB_PLUSPULSES_RESET;
    calm = (uint32_t)m;
  }
  HAL_RTCEx_SetSmoothCalib(&hrtc, RTC_SMOOTHCALIB_PERIOD_32SEC, calp, calm);
  s_rtcAppliedPpm = ppm;
}

void rtc_calib_apply_from_flash(void)
{
  IflashCfg cfg;
  if (iflash_cfg_read(&cfg) == 0 && cfg.rtc_valid)
    rtc_apply_ppm(cfg.rtc_ppm);
  else
    rtc_apply_ppm(RTC_CAL_DEFAULT_PPM);
}

float rtc_calib_get_ppm(void) { return s_rtcAppliedPpm; }

int rtc_calib_set_ppm(float ppm)
{
  IflashCfg cfg;
  cfg_read_or_init(&cfg);
  cfg.rtc_ppm = ppm;
  cfg.rtc_valid = 1u;
  if (iflash_cfg_write(&cfg) != 0) return -1;
  rtc_apply_ppm(ppm);
  return 0;
}

/* ---- Паспорт устройства (стр.123) ---------------------------------------- */
/* Прочитать паспорт. Возврат 1 = паспорт задан (поля заполнены), 0 = не задан
 * (поля обнулены). Калибровки не затрагиваются. */
int data_passport_get(char serial[16], uint8_t *variant,
                      uint16_t *year, uint8_t *month, uint8_t *day)
{
  IflashCfg cfg;
  if (iflash_cfg_read(&cfg) == 0 && cfg.passport_valid == 1u)
  {
    memcpy(serial, cfg.serial, 16);
    serial[15] = '\0';
    *variant = cfg.variant;
    *year = cfg.rel_year; *month = cfg.rel_month; *day = cfg.rel_day;
    return 1;
  }
  memset(serial, 0, 16);
  *variant = 0; *year = 0; *month = 0; *day = 0;
  return 0;
}

/* Записать паспорт: read-modify-write (калибровки скорости/RTC сохраняем). */
int data_passport_set(const char serial[16], uint8_t variant,
                      uint16_t year, uint8_t month, uint8_t day)
{
  IflashCfg cfg;
  cfg_read_or_init(&cfg);
  memcpy(cfg.serial, serial, 16);
  cfg.serial[15] = '\0';
  cfg.variant = variant;
  cfg.rel_year = year; cfg.rel_month = month; cfg.rel_day = day;
  cfg.passport_valid = 1u;
  if (iflash_cfg_write(&cfg) != 0) return -1;
  return 0;
}

static float speedCalCoef(float rpm)
{
  if (s_speedN == 0) speedcal_use_default();
  if (rpm <= s_speedR[0])            return s_speedK[0];
  if (rpm >= s_speedR[s_speedN-1])   return s_speedK[s_speedN-1];
  for (uint16_t i = 1; i < s_speedN; i++)
    if (rpm <= s_speedR[i])
    {
      const float x0 = s_speedR[i-1], y0 = s_speedK[i-1];
      const float x1 = s_speedR[i],   y1 = s_speedK[i];
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
    /* МАКСИМУМ температуры IMU за цикл (02.08.2026, идеология maxRpm/maxVibro).
     * Опрос РЕДКИЙ: температура кристалла меняется медленно, снимать на каждом
     * сэмпле незачем — берём раз в TEMP_SAMPLE_EVERY выборок и держим максимум. */
    if (r->tempThrottle == 0u)
    {
      float tImu = 0.0f;
      if (LSM6DSO_GetTemp(r->lsm, &tImu) == LSM6DSO_OK)
      {
        int16_t tc = (int16_t)lrintf(tImu);
        if (tc > r->tempMax) r->tempMax = tc;
        r->tempThrottle = TEMP_SAMPLE_EVERY;   /* успех → дальше прореживаем */
      }
      /* При СБОЕ чтения throttle НЕ взводим — повторим на следующем сэмпле.
       * (02.08.2026: раньше взводили всегда и игнорировали результат — один
       * сбойный I2C-опрос ронял tImu=0 → температуру всего цикла в 0, отсюда
       * «температура только у части циклов».) */
    }
    else
      r->tempThrottle--;

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

/* Flash record write — SaveParamOnEEPROM.
 * ФОРМАТ СЛОВА (28.07.2026): слово = страница 256 Б, байт [0] = маркёр типа,
 * запись(и) цикла (48 Б) с offset 1. Формат задаётся REC_FORMAT (0x31), декодер
 * выбирает по маркёру [0] (единое маркёрное пространство с Логгером):
 *   0xFF — пусто/конец журнала;
 *   0xF5 — БАЗОВЫЙ: 1 запись цикла на слово ([1..48], хвост FF);
 *   0xF3 — УПЛОТНЁННЫЙ: до 5 записей на слово ([1..48],[49..96],…[193..240],
 *          хвост [241..255] FF) — «старая» плотная упаковка, ×5 ёмкость;
 *   0xF4 — ПОДРОБНЫЙ (резерв): базовая запись + хвост [49..255], состав позже.
 * «Базовый» 48-байтный формат записи НЕ меняется во всех режимах — меняется
 * только сколько их в слове и маркёр. */
#define LOG_START_PAGE    1u
#define RECORD_BYTES      48u   /* базовая запись цикла (без маркёра) */
#define COMPACT_RECS      5u    /* уплотнённый: записей на слово (5×48=240, +маркёр) */
#define REC_MARK_BASIC    0xF5u /* маркёр слова: базовая (1 запись/слово) */
#define REC_MARK_COMPACT  0xF3u /* маркёр слова: уплотнённая (до 5 записей/слово) */
#define REC_MARK_EMPTY    0xFFu /* пустое слово = конец журнала */
#define REC_VERSION       2u
#define VARIANT_FLAG_A    0x0Au

static uint32_t s_writePage = 0u;
static uint8_t  s_writeSlot = 0u;   /* слот внутри слова (0..4 для уплотнённого; 0 базовый) */
static uint8_t  s_writeNewWord = 1u;/* 1 = целевое слово пустое (писать маркёр); 0 = дозапись в компакт-слово */

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

/* Позиция записи по маркёру слова:
 *  0xFF пусто → начать НОВОЕ слово (маркёр по текущему формату), слот 0;
 *  0xF5 базовое → слово занято 1 записью, дальше;
 *  0xF3 уплотнённое → искать первый свободный слот (ts==FF) из COMPACT_RECS;
 *       все заняты → дальше;
 *  прочее (0xF4 и т.п.) → слово занято, дальше. */
static void flashFindWritePos(void)
{
  uint8_t mark;
  for (uint32_t page = LOG_START_PAGE; page < 65536u; page++) {
    P25Qx_QPI_Read(&flash, page << 8u, 1u, &mark);
    if (mark == REC_MARK_EMPTY) {
      s_writePage = page; s_writeSlot = 0u; s_writeNewWord = 1u; return;
    }
    if (mark == REC_MARK_COMPACT) {
      for (uint8_t slot = 0u; slot < COMPACT_RECS; slot++) {
        uint8_t b4[4]; uint32_t ts;
        P25Qx_QPI_Read(&flash, (page << 8u) + 1u + (uint32_t)slot * RECORD_BYTES, 4u, b4);
        memcpy(&ts, b4, 4u);
        if (ts == 0xFFFFFFFFu) {
          s_writePage = page; s_writeSlot = slot; s_writeNewWord = 0u; return;
        }
      }
      continue;   /* все 5 слотов заняты — следующее слово */
    }
    /* 0xF5 базовое (1 запись) / иной маркёр — слово занято, идём дальше */
  }
  s_writePage = 0x10000u;   /* чип заполнен */
  s_writeSlot = 0u; s_writeNewWord = 0u;
}

/* ---- Флаговое слово в служебной стр.0 NOR (03.08.2026) ---------------------
 * Замена рискованной внутренней Flash STM стр.122 (DATAFLAG). Каждый флаг —
 * ОТДЕЛЬНЫЙ байт стр.0: база 0x55 (узор теста активации) / 0xFF, взведён = 0x00
 * (добит program-ом бит 1→0 БЕЗ стирания — безопасно на батарее: ни erase, ни
 * ECC). Все флаги SET-ONLY; сброс — только стиранием данных (стр.0 в одном 4КБ-
 * секторе с данными, LOG_START_PAGE=1): 0x2A перезаписывает стр.0 базой, полное
 * стирание чипа гасит всё. Номера по смыслу (в Data.h). Активация ДУБЛИРУЕТСЯ в
 * стр.121 (журнал ts) — там история/калибровка; здесь [1] — быстрый булев. */

/* 1 = флаг взведён (байт == 0x00), 0 = нет. idx = смещение байта в стр.0. */
int data_norflag_get(uint8_t idx)
{
  uint8_t b = 0xFFu;
  P25Qx_QPI_Read(&flash, (uint32_t)idx, 1u, &b);
  return (b == 0x00u) ? 1 : 0;
}

/* Взвести флаг: добить байт idx → 0x00 (один program, без стирания). Идемпотентно
 * (0x00 поверх 0x00 — no-op для NOR). Вызывать только когда флаг реально нужен. */
void data_norflag_set(uint8_t idx)
{
  uint8_t b = 0x00u;
  P25Qx_QPI_ProgramPage(&flash, (uint32_t)idx, &b, 1u);
}

uint32_t SaveParamOnEEPROM(RegistratorData *r)
{
  flashFindWritePos();
  if (s_writePage >= 0x10000u)
    return HAL_ERROR;

  /* Строим базовую 48-байтную запись отдельно; как она ляжет в слово — решает
   * ветка записи ниже (новое слово с маркёром / дозапись в уплотнённый слот). */
  uint8_t rec[RECORD_BYTES];
  memset(rec, 0, RECORD_BYTES);

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
  /* rec[36] = МАКСИМУМ температуры IMU LSM за цикл (02.08.2026, идеология
   * maxRpm/maxVibro). Кодирование: байт = °C + 60 (uint8 → −60..+195 °C, с
   * запасом под предел датчика ~+150). Максимум копится в HandleSensorData
   * редким опросом (TEMP_SAMPLE_EVERY). Если цикл был так короток, что ни разу
   * не сняли (tempMax == INT16_MIN) — берём одиночный отсчёт как фолбэк. LSM уже
   * включён. Декодер LOGLSMW читает rec[36]-60. rec[37] status = 0. */
  {
    int32_t tC;
    if (r->tempMax != INT16_MIN) {
      tC = r->tempMax;                 /* максимум за цикл (штатный путь) */
    } else {
      /* Ни одного удачного отсчёта за цикл (очень короткий цикл) — одиночный
       * фолбэк с проверкой результата; при сбое пишем sentinel −60 (rec[36]=0),
       * декодер покажет как «нет температуры», а не мусор. */
      float tImu = 0.0f;
      tC = (LSM6DSO_GetTemp(&lsm, &tImu) == LSM6DSO_OK) ? (int32_t)lrintf(tImu) : -60;
    }
    int32_t tRaw = tC + 60;
    if (tRaw < 0)        tRaw = 0;
    else if (tRaw > 255) tRaw = 255;
    rec[36] = (uint8_t)tRaw;
  }
  rec[38] = REC_VERSION;      /* = 2 */
  rec[39] = VARIANT_FLAG_A;   /* = 0x0A */

  /* ПОКОЛЕНИЕ ЧАСОВ (02.08.2026) — u16 в rec[40..41], бывшие reserved.
   * = число событий обнуления часов (EVT_CLOCKZERO) в журнале внутренней Flash.
   * Все записи одной эпохи несут одинаковый номер; после полевого разрыва
   * питания часы падают к нулю И номер +1 → при последовательном чтении смена
   * этого номера = ТОЧКА СТЫКА (время рестартовало). Размер записи (48 Б),
   * маркёр слова и последовательность вывода НЕ меняются; CRC ниже (46 Б) уже
   * покрывает эти байты. Старые записи (reserved=0) читаются как эпоха 0 —
   * совместимо. См. clock_epoch_seam_spec_v1.md. */
  uint16_t clockEpoch = iflash_journal_count(EVT_CLOCKZERO);
  memcpy(rec + 40, &clockEpoch, 2u);

  /* reserved[42..45] = 0 (memset), по спеке v2. Диагностика убрана 18.07 вечер:
   * подтверждено vibCount≈31/с, пик в мг сходится, шкала ±2g, state=ROTATING —
   * «нулевой» прогон был артефактом промежуточной прошивки 15:08. */

  /* rec[42..45] reserved = 0 (memset) — по спеке v2 (rec[40..41] = поколение
   * часов, см. выше). Диагностика 0xBEEF/vibCount/
   * len убрана 17.07 11:14: вибрация подтверждена на железе (vib1/vib2 ненулевые,
   * vibCount~105-108, len=6, 0xBEEF доезжал → свежий Data.c исполняется).
   * Свежесть прошивки теперь проверяется индикатором версии (com.c) + надёжной
   * clean-rebuild сборкой в VS Code (см. CLAUDE.md, регламент 17.07). */
  uint16_t crc = ltp_crc16(rec, 46u);
  memcpy(rec + 46, &crc, 2u);

  /* ⚠ 28.07.2026: erase страницы УБРАН из рабочего пути записи цикла.
   * Идеология (data_format_spec_v1.md §«Общая архитектура»): стирание — ТОЛЬКО
   * в сервисе (полное стирание чипа = проверяемое условие активации). В работе
   * на батарейке пишем только program в уже-FF ячейки; при заполнении журнала
   * запись прекращается, поверх не пишем. Erase здесь нарушал правило и создавал
   * окно порчи при обрыве питания (батарейка в зажиме + вибрация). Страница уже
   * чиста после сервисного стирания перед выездом — program достаточно. */
  if (s_writeNewWord) {
    /* НОВОЕ слово: маркёр текущего формата [0] + запись в слот 0 ([1..48]),
     * один program 49 байт. Базовый → 1 запись/слово; уплотнённый → это первая
     * из пяти, остальные допишутся в след. циклы веткой else. */
    uint8_t word[1u + RECORD_BYTES];
    word[0] = rec_format_marker();
    memcpy(word + 1u, rec, RECORD_BYTES);
    P25Qx_QPI_ProgramPage(&flash, s_writePage << 8u, word, 1u + RECORD_BYTES);
  } else {
    /* ДОЗАПИСЬ в уплотнённое слово: маркёр [0]=0xF3 уже стоит, пишем только
     * запись в свой слот ([1 + slot*48 ..]). */
    uint32_t addr = (s_writePage << 8u) + 1u + (uint32_t)s_writeSlot * RECORD_BYTES;
    P25Qx_QPI_ProgramPage(&flash, addr, rec, RECORD_BYTES);
  }

  /* Данные появились → взвести флаг «данные_есть» ([1]) в служебной стр.0 NOR.
   * Один program байта 0x55/0xFF→0x00 без стирания — безопасно на батарее.
   * Проверено на железе (дамп: байт [1]=0x00 после цикла) — путь записи НЕ виснет.
   * (Зависание было при ЧТЕНИИ после сна: обесточенный флеш → вечный Wait_Busy;
   * лечится таймаутом Wait_Busy, см. p25q128.c 14:55.) Идемпотентно. */
  if (!data_norflag_get(NOR_FLAG_DATA))
    data_norflag_set(NOR_FLAG_DATA);
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
  if (s_writePage == LOG_START_PAGE && s_writeNewWord) {   /* ничего не записано */
    r->totalSec = 0u;
    return HAL_OK;
  }
  /* Последняя запись:
   *  - дозаполняем уплотнённое слово (slot>0) → предыдущий слот этого же слова;
   *  - иначе (новое пустое слово / чип полон) → предыдущее ПОЛНОСТЬЮ занятое
   *    слово; его последний слот по маркёру (уплотнённое → COMPACT_RECS-1). */
  uint32_t lastPage; uint8_t lastSlot;
  if (!s_writeNewWord && s_writeSlot > 0u) {
    lastPage = s_writePage;
    lastSlot = s_writeSlot - 1u;
  } else {
    lastPage = (s_writePage >= 0x10000u) ? 65535u : (s_writePage - 1u);
    uint8_t m;
    P25Qx_QPI_Read(&flash, lastPage << 8u, 1u, &m);
    lastSlot = (m == REC_MARK_COMPACT) ? (uint8_t)(COMPACT_RECS - 1u) : 0u;
  }
  uint8_t  rec[RECORD_BYTES];
  uint32_t addr = (lastPage << 8u) + 1u + (uint32_t)lastSlot * RECORD_BYTES;
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
