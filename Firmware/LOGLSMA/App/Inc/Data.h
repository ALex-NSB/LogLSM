#ifndef __DATA_H
#define __DATA_H

#include "stm32l4xx_hal.h"
#include "lsm6dso.h"

#define GYRO_THRESHOLD          600

/* Состояния RegistratorData.state — общий автомат SLEEP->CONFIRM->ROTATING
 * (RotationStateStep(), com.c). 02.07.2026, финальная версия: сон один и
 * тот же ВСЕГДА (настоящий Stop2), независимо от того, подключён кабель
 * или нет — не вопрос экономии энергии, а вопрос проверки: условия
 * выхода из сна (WKUP1/WKUP2) должны отрабатывать одинаково что на
 * стенде, что в поле. RotationStateStep() вызывается ТОЛЬКО из main.c;
 * Service()/BENCH (com.c) распознаванием вращения не занимается вообще
 * (только LTP-команды) — единственная разница между «автономно» и
 * «сервисно» — внутри активной фазы, в момент завершения цикла:
 * PushCycleRecord() сама проверяет текущий уровень WKUP1 и либо
 * дублирует данные цикла по LTP, либо нет; в обоих случаях далее —
 * назад в SLEEP, без исключений.
 *
 * SLEEP — гироскоп выключен, включён только акселерометр (Wake-Up
 * Detection) — реальный Stop2 (~2 мкА), будит аппаратно WKUP2/PC13.
 * Такое пробуждение может быть ложным (удар, тряска — не вращение
 * колеса), поэтому гироскоп не включается сразу на постоянно.
 * CONFIRM — включаем гироскоп на короткое ОКНО (CONFIRM_WINDOW_POLLS
 * опросов): если за это окно RotationDetected() (обычный гироскопный
 * порог GYRO_THRESHOLD) подтвердит реальное вращение — переходим в
 * ROTATING; если нет — гироскоп выключается назад, возврат в SLEEP.
 * ROTATING — вращение подтверждено, идёт цикл, дебаунс
 * ROTATION_DEBOUNCE_N перед завершением. */
#define REG_STATE_SLEEP     0u   /* в т.ч. Stop2 — акселерометр сам будит */
#define REG_STATE_ROTATING  1u   /* вращение подтверждено, идёт цикл */
#define REG_STATE_CONFIRM   2u   /* проснулись по акселерометру, гироскоп
                                   * включён, проверяем окно */

/* Длина окна проверки CONFIRM в опросах Poll_Sensor() (каждый ~240 мс при
 * ODR=12.5 Гц, см. Data.c). НЕ откалибровано — подобрать на стенде так,
 * чтобы окно было заметно длиннее одного цикла шума, но не расходовало
 * заряд на ложные срабатывания дольше необходимого. */
#define CONFIRM_WINDOW_POLLS   5u

/* Дебаунс «нет детекта» перед завершением цикла ROTATING.
 * 06.07.2026: поднят 3→6 (~720 мс → ~1.44 с) — МЕТОД 1 против дробления
 * циклов: на разгоне стенда с нуля и при смене скорости показания гироскопа
 * кратковременно проседают ниже порога, и при N=3 один физический поворот
 * рвался на обрывки. Больший N держит цикл цельным через короткие просадки.
 * ⚠ Значение тюнингуемое: слишком большое — медленно ловит реальный стоп
 * (цикл кажется длиннее на N опросов); подобрать на стенде. */
#define ROTATION_DEBOUNCE_N    6u

/* МЕТОД 2 против дробления (06.07.2026): минимальная длительность цикла (сек).
 * Циклы короче НЕ пишутся во Flash и не пушатся — это фрагменты (durS=0/1) с
 * разгона, не реальные циклы. Страховка поверх дебаунса: даже если обрывок
 * проскочил, он не засоряет Flash/журнал/график. Совпадает с фильтром ≥2 с на
 * стороне LOGLSMW (там прячем с глаз, здесь чистим в корне). */
#define MIN_CYCLE_SEC          2u

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
  uint8_t noDetectCount;   /* дебаунс: сколько подряд «нет детекта» в state=1 */
  uint8_t confirmPolls;    /* счётчик опросов в REG_STATE_CONFIRM (02.07.2026) */
  uint8_t monitoringActive; /* 02.07.2026: автомат SLEEP->CONFIRM->ROTATING
                             * (режим A) запускается ТОЛЬКО явной командой
                             * CMD_START_REGISTER (0x1D, cmdStartRegister() в
                             * com.c) — не автоматически по состоянию WKUP1.
                             * main.c проверяет этот флаг перед тем, как
                             * вообще трогать автомат; 0 по умолчанию при
                             * старте прошивки. */
  uint8_t testNoSleep;      /* Режим «Тест» (CMD_START_TEST 0x23): тот же
                             * автомат, что и «Работа», но в фазе SLEEP НЕ
                             * уходим в Stop2 — бодрствуем, обслуживаем UART
                             * (ComPoll) и ловим фронт INT1/WKUP2 опросом
                             * флага WUF2 на ходу. 0 = «Работа» (со сном),
                             * 1 = «Тест». Снимается в cmdStopRegister(). */
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
