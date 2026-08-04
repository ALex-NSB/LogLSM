#include "com.h"
#include "ltp.h"
#include "pwr.h"
#include "globals.h"
#include "main.h"
#include "iflash.h"      /* журнал событий внутренней Flash (счётчики рестартов) */
#include "Data.h"        /* NOR-флаги стр.0 (data_norflag_*), калибровки */

/* FlashQ.h исключён: дублирует union StatusRegBits из p25q128.h */
void FlashOn(void);
void FlashOff(void);
#include "quadspi.h"
#include "fm25xx.h"
#include "spi.h"
#include "tmp117.h"
#include "i2c.h"
#include "string.h"
#include "lsm6dso_temp.h"
#include "stm_temp.h"

#include "usart.h"
#include "dma.h"
#include "rtc.h"

#include "stdarg.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "stdint.h"

#define WORD_PAR(X, Y)          (X << 8 | Y)

typedef void (*cmd_callback_t)(LtpPacket *wc);

/* errors (коды в байте cod PAYLOAD — прикладной уровень, не FLAGS.ERR) */
enum {er_none, er_timeout, er_badarg, er_not_impl, er_dead_imu};

enum MemoryType {m_eeprom, m_fram};

void invoke_cmd_handler(LtpPacket *wc);
void sendPacket(uint8_t addr, uint8_t cmd, void *data, uint32_t n);
void sendError(uint8_t addr, uint8_t cmd, uint8_t err_code);
void ReadReceiveBuffer(UART_HandleTypeDef *huart, uint32_t start, uint32_t stop);

void cmdPing(LtpPacket *wc);                          /* 0x01 */
void cmdWhoAmI(LtpPacket *wc);                        /* 0x02 */

void cmdFlashReadIDHandler(LtpPacket *wc);            //C0 8D 04 00 00
void cmdFlashChipEraseHandler(LtpPacket *wc);
void cmdFlashDataEraseHandler(LtpPacket *wc);         /* 0x2A — «Стереть данные»: чип БЕЗ служебной стр.0 */
void FlashDataEraseTick(void);                        /* дозавершение 0x2A — дёргать из Service() */
void cmdFlashSetSpiModeHandler(LtpPacket *wc);        /* 0x2B — режим SPI (0=SPI, 1=SPIx4), только Сервис */
void cmdFlashSetFreqHandler(LtpPacket *wc);          /* 0x2C — частота флеша: делитель от 80 МГц (0..4=80/40/20/10/5), только Сервис */
void cmdFlashSpeedTestHandler(LtpPacket *wc);        /* 0x2D — замер скорости записи/чтения области, только Сервис */
void cmdDataFlagHandler(LtpPacket *wc);              /* 0x30 — флаг «есть непрочитанные данные» (внутр. Flash стр.122) */
void cmdRecFormatHandler(LtpPacket *wc);            /* 0x31 — формат записи цикла (маркёр слова): 0 базовый/1 уплотн/2 подроб */
void cmdClearJournals(LtpPacket *wc);              /* 0x27 — «Очистить журналы»: стереть стр.121 (активации) + 124..127 (рестарты), Сервис */
void activation_cache_init(void);                  /* старт Service: журнал активаций стр.121 → кэш s_actts (один раз) */
void cmdActHistory(LtpPacket *wc);                 /* 0x2E — история активаций: список ts со стр.121, Сервис */
void cmdSpeedCalGet(LtpPacket *wc);                /* 0x2F — таблица калибровки скорости (стр.123) → ПК, Сервис */
void cmdSpeedCalSet(LtpPacket *wc);                /* 0x32 — записать таблицу калибровки скорости на стр.123, Сервис */
void cmdRtcCalibGet(LtpPacket *wc);                /* 0x33 — чтение поправки RTC ppm, Сервис */
void cmdRtcCalibSet(LtpPacket *wc);                /* 0x34 — запись поправки RTC ppm на стр.123, Сервис */
void cmdPassportGet(LtpPacket *wc);                /* 0x35 — чтение паспорта (стр.123) → ПК, Сервис */
void cmdPassportSet(LtpPacket *wc);                /* 0x36 — запись паспорта на стр.123, Сервис */
void cmdFlashPageEraseHandler(LtpPacket *wc);
void cmdFlashSectorEraseHandler(LtpPacket *wc);       /* 0x1F */
void cmdFlashWritePageHandler(LtpPacket *wc);         //C0 8D 05 00 00 00 00 00
void cmdFlashReadHandler(LtpPacket *wc);              //C0 8D 07 00 08 00 00 00 00 00 01 00 00
void cmdFlashGetState(LtpPacket *wc);                 //C0 8D 09 00 00
void cmdGetTemp(LtpPacket *wc);
void cmdSetBaud(LtpPacket *wc);
void cmdFramWritePageHandler(LtpPacket *wc);
void cmdFramReadHandler(LtpPacket *wc);
void cmdFlashOn(LtpPacket *wc);
void cmdFlashOff(LtpPacket *wc);
void cmdGetTempIMU(LtpPacket *wc);
void cmdGetTempChip(LtpPacket *wc);
//void cmdSetAcquisitionData(LtpPacket *wc);
//void cmdGetAcquisitionData(LtpPacket *wc);

void cmdAccSetOdr(LtpPacket *wc);
void cmdAccSetFullScale(LtpPacket *wc);
void cmdGyroSetOdr(LtpPacket *wc);
void cmdGyroSetFullScale(LtpPacket *wc);
void cmdAccGetOdr(LtpPacket *wc);
void cmdAccGetFullScale(LtpPacket *wc);
void cmdGyroGetOdr(LtpPacket *wc);
void cmdGyroGetFullScale(LtpPacket *wc);
void cmdGetAxesRaw(LtpPacket *wc);


void cmdSetDateTime(LtpPacket *wc);
void cmdGetDateTime(LtpPacket *wc);

void cmdStartRegister(LtpPacket *wc);
void cmdGetStats(LtpPacket *wc);           /* 0x1E */
void cmdResetTotal(LtpPacket *wc);         /* 0x21 */
void cmdResetStats(LtpPacket *wc);         /* 0x24 — сброс счётчиков перезапусков */
void cmdWdgTest(LtpPacket *wc);            /* 0x26 — тест IWDG: отключить рефреш */
void cmdStopRegister(LtpPacket *wc);       /* 0x22 */
void cmdStartTest(LtpPacket *wc);          /* 0x23 */

/* Указатель на данные регистратора — устанавливается в Service(), нужен
 * cmdResetTotal, т.к. callback-функции не получают r напрямую. */
static RegistratorData *s_reg = NULL;

static cmd_callback_t CMD[128] = {
  NULL,                                         // 0x0
  cmdPing,                                      // 0x1  CMD_PING
  cmdWhoAmI,                                    // 0x2  CMD_WHO_AM_I
  NULL,                                         // 0x3
  cmdFlashReadIDHandler,                        // 0x4
  cmdFlashChipEraseHandler,                     // 0x5
  cmdFlashPageEraseHandler,                     // 0x6
  cmdFlashReadHandler,                          // 0x7
  cmdFlashWritePageHandler,                     // 0x8
  cmdFlashGetState,                             //0x9
  cmdGetTemp,                                   //0xA
  cmdSetBaud,                                   //0xB
  cmdFramWritePageHandler,                      /* 0xC  */
  cmdFramReadHandler,                           /* 0xD  */
  cmdFlashOn,                                   /* 0xE  */
  cmdFlashOff,                                  /* 0xF  */
  cmdGetTempIMU,                                /* 0x10 */
  cmdGetTempChip,                               /* 0x11 */
  cmdAccSetOdr,                                 /* 0x12 */
  cmdAccSetFullScale,                           /* 0x13 */
  cmdGyroSetOdr,                                /* 0x14 */
  cmdGyroSetFullScale,                          /* 0x15 */
  cmdAccGetOdr,                                 /* 0x16 */
  cmdAccGetFullScale,                           /* 0x17 */
  cmdGyroGetOdr,                                /* 0x18 */
  cmdGyroGetFullScale,                          /* 0x19 */
  cmdGetAxesRaw,                                 /* 0x1A */
    cmdGetDateTime,                             /* 0x1B */
    cmdSetDateTime,                              /* 0x1C */
    cmdStartRegister,                           /* 0x1D */
    cmdGetStats,                                /* 0x1E  GET_STATS */
    cmdFlashSectorEraseHandler,                 /* 0x1F  стирание сектора 4 КБ */
    NULL,                                       /* 0x20  CMD_CYCLE_PUSH — только исходящий */
    cmdResetTotal,                              /* 0x21  CMD_RESET_TOTAL */
    cmdStopRegister,                            /* 0x22  CMD_STOP_REGISTER (03.07.2026) */
    cmdStartTest,                               /* 0x23  CMD_START_TEST — «Тест» без сна (04.07.2026) */
    cmdResetStats,                              /* 0x24  сброс счётчиков перезапусков (журнал iflash) */
    NULL,                                       /* 0x25  WDG_KICK — только исходящий */
    cmdWdgTest,                                 /* 0x26  тест IWDG: отключить рефреш → сброс через ~32 c */
    cmdClearJournals,                           /* 0x27  «Очистить журналы»: стереть стр.121 (активации) + 124..127 (рестарты), стр.123 цела — Сервис (03.08.2026) */
    /* 0x28 — зарезервирован под паспорт/заводскую команду (см. session_notes
     * 18.07.2026, §11) — ещё не реализован, оставлен NULL.
     * 0x29 — SUBSPEED_PUSH, только исходящий (как CYCLE_PUSH 0x20), NULL. */
    [0x2A] = cmdFlashDataEraseHandler,          /* 0x2A  «Стереть данные» — чип БЕЗ служебной стр.0 (21.07.2026) */
    [0x2B] = cmdFlashSetSpiModeHandler,         /* 0x2B  режим SPI: 0=SPI(x1), 1=SPIx4 — только Сервис (22.07.2026) */
    [0x2C] = cmdFlashSetFreqHandler,            /* 0x2C  частота флеша (делитель от 80 МГц) — только Сервис (26.07.2026) */
    [0x2D] = cmdFlashSpeedTestHandler,          /* 0x2D  замер скорости записи/чтения — только Сервис (26.07.2026) */
    [0x2E] = cmdActHistory,                      /* 0x2E  история активаций: список ts со стр.121 — Сервис (03.08.2026) */
    [0x2F] = cmdSpeedCalGet,                      /* 0x2F  калибровка скорости: чтение таблицы (стр.123) — Сервис (03.08.2026) */
    [0x32] = cmdSpeedCalSet,                      /* 0x32  калибровка скорости: запись таблицы (стр.123) — Сервис (03.08.2026) */
    [0x33] = cmdRtcCalibGet,                      /* 0x33  калибровка RTC: чтение ppm — Сервис (03.08.2026) */
    [0x34] = cmdRtcCalibSet,                      /* 0x34  калибровка RTC: запись ppm (стр.123) — Сервис (03.08.2026) */
    [0x30] = cmdDataFlagHandler,                /* 0x30  флаг «непрочитанные данные» (внутр. Flash стр.122) — Сервис */
    [0x31] = cmdRecFormatHandler,               /* 0x31  формат записи цикла (маркёр слова) — Сервис */
    [0x35] = cmdPassportGet,                     /* 0x35  паспорт: чтение (стр.123) — Сервис (03.08.2026) */
    [0x36] = cmdPassportSet                      /* 0x36  паспорт: запись (стр.123) — Сервис (03.08.2026) */
};
  
static LtpParser ltp_rx;          /* FSM-парсер входного потока */
static uint16_t  cur_seq;         /* SEQ текущего запроса — копируется в ответ */
static uint8_t TxBuf[1024];
static uint8_t RxBuf[512];
static uint32_t prevFill;
extern DMA_HandleTypeDef hdma_usart2_rx;
//static uint32_t TxSize;
static uint8_t FlashChipHasErased;

//lsm
/*
static struct AcqusitionDataStruct
{
  LSM6DSO_AxesRaw_t acc;
  LSM6DSO_AxesRaw_t gyro;
  int32_t rot;
} AcqusitionData;*/

static int32_t rot;
static uint32_t N;
static uint8_t QUIT;
/* Флаг инициализации Flash: предотвращает повторный flashActiv() при
 * переподключении — Flash в QPI-режиме не понимает SPI-инициализацию,
 * повторный вызов → QUADSPI таймаут 5-16 с. Используется совместно
 * Service() и cmdFlashOn(). */
static uint8_t s_flash_on = 0;
//extern UART_HandleTypeDef huart2;

void ComInit()
{
  HAL_UART_DeInit(&huart2);
  MX_DMA_Init();
  MX_USART2_UART_Init();
  
  N = 0;
  rot = 0;
  QUIT = 0;
  
  ltp_parser_init(&ltp_rx);
  cur_seq = 0;
  prevFill = 0;
  FlashChipHasErased = 0;
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf));
  __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);
  //HAL_UART_Receive_IT(&huart2, RxBuf, sizeof(RxBuf));
}

void ComDeInit()
{
  HAL_UART_DeInit(&huart2);
}

/* Общий шаг SLEEP->CONFIRM->ROTATING — см. com.h. 02.07.2026, финальная
 * версия по прямому указанию пользователя (после нескольких неверных
 * попыток — записано, чтобы не переигрывать заново):
 *
 * СОН ОДИН И ТОТ ЖЕ ВСЕГДА, НЕЗАВИСИМО ОТ КАБЕЛЯ. Это не вопрос экономии
 * энергии (на кабеле питание и так внешнее) — это вопрос ПРОВЕРКИ:
 * реальный Stop2 и условия выхода из него (WKUP1/WKUP2) должны
 * отрабатывать ОДИНАКОВО что на столе с кабелем, что в поле. Никакого
 * отдельного «программного псевдосна» или «подмены состояния, лишь бы
 * гироскоп не выключался» здесь НЕТ и не должно быть — если гироскоп
 * всё время включён, это уже другой режим сна, а не «тот же самый»
 * (см. также «Мониторинг» ниже).
 *
 * Единственная разница между «автономно» и «сервисно» — ВНУТРИ активной
 * фазы, в момент завершения цикла (см. REG_STATE_ROTATING ниже):
 * PushCycleRecord() сам проверяет текущий уровень WKUP1 — если кабель
 * есть, дублирует данные цикла по LTP (активная фаза чуть удлиняется на
 * время передачи), если кабеля нет — просто ничего не шлёт. В ОБОИХ
 * случаях после этого — назад в REG_STATE_SLEEP, «намертво», без
 * исключений.
 *
 * Эта функция вызывается ТОЛЬКО из main.c (WORK/PASSIVE-цикл, реальный
 * Stop2) — Service() (см. ниже) больше не занимается распознаванием
 * вращения вообще, только обслуживанием LTP-команд по запросу
 * («Мониторинг» — отдельная, уже существующая функция для постоянного
 * опроса гироскопа, когда это осознанно нужно — не путать с этим
 * автоматом). */
/* Штатное завершение цикла ROTATING: метка стопа, наработка, дублирование
 * по LTP при кабеле (PushCycleRecord сама смотрит WKUP1), запись во Flash,
 * возврат в SLEEP с гашением гироскопа. Вынесено 03.07.2026 — используется
 * двумя путями: (1) дебаунс-веткой RotationStateStep() ниже (вращение
 * физически закончилось) и (2) cmdStopRegister() (остановка теста со
 * стенда пришла, пока цикл ещё идёт — цикл закрывается тем, что успело
 * накопиться, а не выбрасывается). */
static void RotationFinishCycle(RegistratorData *r)
{
  /* Стоп = время ПОСЛЕДНЕГО детекта вращения (r->rot.lastRotStamp), а НЕ
   * «сейчас» (11.07.2026). С переходом Poll_Sensor на DRDY гироскопа опрос
   * стал ~240 мс, и дебаунс ROTATION_DEBOUNCE_N=6 давал ~1.4 c «хвоста» после
   * реальной остановки мотора → durS раздувался (+2 c, «Общее» уползало
   * +1/+3/+5/+7). lastRotStamp фиксирует конец РЕАЛЬНОГО вращения, дебаунс в
   * длительность не входит. */
  /* Цикл завершился → возвращаемся в Пассивный «со свежими ушами»: троттл в
   * минимум + pending, чтобы следующая вибрация после паузы поймалась сразу
   * (12.07.2026). Ставим ДО ветвления — общее для обоих исходов (обрывок < MIN
   * и полноценный цикл). */
  r->gyroRecheckSec   = GYRO_RECHECK_MIN_SEC;
  r->gyroCheckPending = 1;
  r->rot.stopTimeStamp = r->rot.lastRotStamp;
  uint32_t durS = RTC_SubTimeDateSec(&r->rot.stopTimeStamp,
                                     &r->rot.startTimeStamp);
  /* САНИТИ-КЛАМП длительности (18.07.2026, 2-я линия после фикса
   * cmdSetDateTime): цикл «длиннее суток» физически невозможен на стенде —
   * это порванные штампы (перевод часов посреди цикла и т.п.). Отбрасываем
   * цикл целиком, чтобы наработку не взорвало (реальный случай: +232 705 ч). */
  if (durS > 86400u)
  {
    r->state = REG_STATE_SLEEP;
    LSM6DSO_GYRO_Disable(r->lsm);
    return;
  }
  /* МЕТОД 2 против дробления (06.07.2026): циклы короче MIN_CYCLE_SEC не пишем
   * во Flash и не пушим — это обрывки (durS=0/1) с разгона стенда, не реальные
   * циклы. Просто возвращаемся в SLEEP и гасим гироскоп. totalSec на них не
   * наращиваем (доли секунды разгона пренебрежимы; реальный цикл далее
   * посчитается сам). Работает поверх дебаунса ROTATION_DEBOUNCE_N (метод 1) —
   * оба независимы, друг другу не мешают. */
  if (durS < MIN_CYCLE_SEC)
  {
    r->state = REG_STATE_SLEEP;
    LSM6DSO_GYRO_Disable(r->lsm);
    if (r->accFs != 2u)   /* авторейндж поднимал шкалу → назад ±2g (Wake-Up) */
    {
      r->accFs = 2u;
      LSM6DSO_ACC_SetFullScale(r->lsm, 2);
    }
    return;
  }
  r->totalSec += durS;
  /* Отчётная скорость = СРЕДНЕЕ по фазе вращения, не пик. Пик (rot.maxRate)
   * латчит стартовый выброс шаговика (резонанс на малых об/мин) и держит его
   * весь цикл: в «Тесте» непрерывный опрос его ловит, в «Работе» Stop2 его
   * просыпает — отсюда была разница «10 задал → 20 измерил». Пик сохраняем
   * отдельным полем как «Пиковое». Прежний HandleSensorData(r) здесь убран —
   * он кормил в среднее выборку УЖЕ ПОСЛЕ дебаунса «нет вращения» (тянула вниз). */
  if (r->rateCount)
    r->avgRate = (uint16_t)(r->rateSum / (int32_t)r->rateCount);
  /* Вибрация цикла → maxVibr (mg): ПИК отклонения модуля accel (LSB) ×
   * чувствительность accel (mg/LSB). Закрывает старую заглушку maxVibro=0.0.
   * (RMS = √(Σdev²/N)×sens уже считается в vibSumSq/vibCount, но в 24-байт
   * запись пока нет поля — добавить при расширении формата рядом с maxVibro.) */
  if (r->vibCount)
  {
    /* vibPeak уже в МГ (авторейндж, 18.07.2026). */
    r->rot.maxVibr = (r->vibPeak > 65535.0f) ? 65535u : (uint16_t)(r->vibPeak + 0.5f);
  }
  PushCycleRecord(r);
  SaveParamOnEEPROM(r);
  r->state = REG_STATE_SLEEP;
  LSM6DSO_GYRO_Disable(r->lsm);
  if (r->accFs != 2u)   /* авторейндж поднимал шкалу → назад ±2g (Wake-Up) */
  {
    r->accFs = 2u;
    LSM6DSO_ACC_SetFullScale(r->lsm, 2);
  }
}

uint8_t RotationStateStep(RegistratorData *r)
{
  switch (r->state)
  {
  case REG_STATE_SLEEP:
    /* Ничего не делаем — см. комментарий к функции выше. */
    break;

  case REG_STATE_CONFIRM:
    Poll_Sensor(r);
    if (RotationDetected(r))
    {
      /* Подтверждено — реальное вращение, не ложное срабатывание
       * акселерометра. Гироскоп остаётся включённым, дальше — обычный
       * старт цикла. */
      /* СТАРТ цикла = момент ПОДТВЕРЖДЕНИЯ вращения — для ОБОИХ режимов
       * (12.07.2026, вечер). Раньше только «Тест» метил здесь, а «Работа» — в
       * момент ПРОБУЖДЕНИЯ (main.c, «нет мёртвого времени»), что резервировало
       * прогрев+CONFIRM в длительность → +1 c на верхах. По решению: «Работа» =
       * «Тест» → старт по CONFIRM. Плюс: верхи ровно 0 c (как эталон «Тест»).
       * Цена (осознанно): на низах честная −задержка (слабая вибрация реально
       * подтверждается позже прогрева/опросов). Перетирает пре-штамп main.c. */
      RTC_GetTimeDate(&r->rot.startTimeStamp);
      r->rot.lastRotStamp = r->rot.startTimeStamp;
      r->rot.maxRate = r->rot.maxVibr = 0;
      r->rateSum = 0; r->rateCount = 0; r->avgRate = 0;
      r->rotWarmup = 1;
      r->vibDc = 0; r->vibDcInit = 0;      /* сброс аккумуляторов вибрации цикла */
      r->vibPeak = 0; r->vibSumSq = 0; r->vibCount = 0;
      r->vibLvl = 0; r->vibLvlInit = 0;    /* «уровень» — median-трекер */
      r->plateauInit = 0; r->subCount = 0; r->plateauChg = 0;  /* детектор полок */
      r->jerkPeak = 0; r->jerkSumSq = 0; r->prevAmagInit = 0;  /* канал 2 (jerk) */
      r->tempMax = INT16_MIN; r->tempThrottle = 0;   /* макс. температура за цикл */
      r->noDetectCount = 0;
      r->idleChecks = 0;  /* активность подтверждена — счётчик покоя обнуляем */
      r->deepSleep  = 0;  /* остаёмся в «Проверке» (RTC), не в глубоком сне */
      r->gyroRecheckSec = GYRO_RECHECK_MIN_SEC;  /* вращение реальное — сбрасываем
                                                  * эскалацию троттла в минимум */
      /* АВТОРЕЙНДЖ шкалы accel (18.07.2026): старт цикла всегда с ±2g (макс.
       * чувствительность, экономичная точка 26 Гц LP не меняется); при клипе
       * сэмпла vibAccumulate сам удвоит шкалу до ±16g. Вибрация копится в мг.
       * Возврат ±2g — при уходе в сон (Wake-Up порог завязан на шкалу).
       * Канал ударов по-настоящему — вариант B (high-g). */
      r->accFs = 2u;
      LSM6DSO_ACC_SetFullScale(r->lsm, 2);
      LSM6DSO_ACC_GetSensitivity(r->lsm, &r->accSensMg);
      HandleSensorData(r);
      r->state = REG_STATE_ROTATING;
    }
    else
    {
      r->confirmPolls++;
      if (r->confirmPolls >= CONFIRM_WINDOW_POLLS)
      {
        /* Окно истекло без подтверждения — вращения нет (тряска/пусто).
         * Гасим гироскоп. 3-уровневый автомат (11.07.2026): считаем пустые
         * проверки; после IDLE_CHECKS_TO_DEEP подряд уходим в ГЛУБОКИЙ СОН
         * (deepSleep=1 → main.c армит accel-wake без RTC), чтобы не опрашивать
         * в покое. */
        LSM6DSO_GYRO_Disable(r->lsm);
        if (r->idleChecks < 0xFFu) r->idleChecks++;
        /* Порог ухода в глубокий сон: с кабелем — большой (сервисное окно,
         * достижимы по LTP для Flash-scan/стопа), без кабеля — быстрый сон
         * (автономность в поле). См. IDLE_CHECKS_TO_DEEP_CABLE в Data.h. */
        uint8_t idleLim = wkup1_pin_set() ? IDLE_CHECKS_TO_DEEP_CABLE
                                          : IDLE_CHECKS_TO_DEEP;
        if (r->idleChecks >= idleLim)
        {
          r->deepSleep = 1;
          /* Целая ПРОВЕРКА-сессия не нашла вращения (устойчивое «нет») —
           * ЭСКАЛИРУЕМ интервал гироскоп-перепроверки (удвоение до CAP), чтобы на
           * упорной вибрации без вращения повторные входы в Активный редели → ток
           * ограничен. Эскалируем ЗДЕСЬ, а НЕ на каждый промах CONFIRM: иначе
           * маргинальный низкооборотный СТАРТ (первые опросы гироскопа ещё ниже
           * порога) наказывался бы ростом интервала и срезал цикл (был −8 c на
           * 7 об/мин). Сброс в MIN — при подтверждённом вращении и после цикла. */
          if (r->gyroRecheckSec < GYRO_RECHECK_MAX_SEC)
          {
            uint32_t nx = (uint32_t)r->gyroRecheckSec << 1;
            r->gyroRecheckSec = (nx > GYRO_RECHECK_MAX_SEC) ? GYRO_RECHECK_MAX_SEC
                                                            : (uint16_t)nx;
          }
        }
        r->state = REG_STATE_SLEEP;
      }
    }
    break;

  case REG_STATE_ROTATING:
    Poll_Sensor(r);
    if (RotationDetected(r))
    {
      r->noDetectCount = 0;
      RTC_GetTimeDate(&r->rot.lastRotStamp);   /* конец цикла = этот детект */
      HandleSensorData(r);
    }
    else
    {
      /* Дебаунс: требуем ROTATION_DEBOUNCE_N подряд «нет детекта»
       * (~720 мс при N=3) перед завершением цикла. */
      r->noDetectCount++;
      if (r->noDetectCount < ROTATION_DEBOUNCE_N)
      {
        HandleSensorData(r);
      }
      else
      {
        RotationFinishCycle(r);
      }
    }
    break;

  default:
    r->state = REG_STATE_SLEEP;
    break;
  }

  return r->state;
}

void Service(RegistratorData *r)
{
  ComInit();

  /* SERVICE: питание внешнее — включаем периферию платы MA00
   * (TMP117 + FRAM). Раньше включалась только в WAKE-ветке (command.c),
   * из-за чего GET_TEMP (0x0A) возвращал 0.00 °C. */
  tmp117Activ();
  framActiv();
  /* Flash нужен для SaveParamOnEEPROM в цикле вращения.
   * flashActiv() (pwr.c): питание + MX_QUADSPI_Init + flash.hqspi + P25Qx_Init + P25Qx_SetQPI.
   * Флаг s_flash_on общий с cmdFlashOn() — повторный вызов при переподключении
   * к уже инициализированной в QPI Flash → QUADSPI таймаут 5-16 с. */
  if (!s_flash_on) { flashActiv(); s_flash_on = 1; }

  /* Кэш активации: журнал стр.121 читаем ОДИН раз здесь (внешнее питание есть),
   * дальше GET_STATS отдаёт ts_activation из кэша s_actts — горячего чтения
   * внутренней Flash нет (именно это роняло прибор в старой версии). 03.08.2026. */
  activation_cache_init();
  data_speedcal_load();   /* таблица калибровки скорости стр.123 → RAM (дефолт если нет) */

  /* Внутренний термодатчик STM32 — ВРЕМЕННО ОТКЛЮЧЁН (12.06):
   * calc_vref()/ADC1_Temp_Channel_Init() вешали контроллер до входа в SERVICE
   * (COM «нет ответа»). ADC bring-up требует отдельной отладки: проверить
   * источник тактирования ADC (RCC ADCSEL/CCIPR), т.к. MX_ADC1_Init не
   * вызывается. STM-температура — наименее ценный датчик (есть TMP117). */
  // calc_vref();
  // ADC1_Temp_Channel_Init();

  /* 02.07.2026: Service() распознаванием вращения БОЛЬШЕ НЕ занимается —
   * убрано по прямому указанию пользователя (см. развёрнутый комментарий
   * у RotationStateStep() выше). Единственный автомат SLEEP->CONFIRM->
   * ROTATING — в main.c, с настоящим Stop2, работает независимо от того,
   * входили мы когда-либо в Service() или нет. Здесь только LoadTotalSec
   * (нужна для GET_STATS) и обслуживание LTP-команд ниже. */
  LoadTotalSec(r);     /* восстановить lifetime-наработку из Flash (поле durationTotal,
                        * bytes 8..11 последней записи). totalSec накапливается между
                        * сеансами — не обнуляется. LOGLSMW снимает снапшот при Старте
                        * циклограммы (GET_STATS → m_preTestRegTotS) и вычисляет
                        * «Сессию» как delta; RESET_TOTAL больше не вызывается. */
  s_reg = r;           /* разрешить cmdResetTotal обнулять totalSec по команде с ПК */

  /* Гироскоп на время Service — ВКЛЮЧЁН (03.07.2026). lsm6dso_init()
   * гироскоп сознательно не включает (он нужен только автомату режима A,
   * который сам им управляет в CONFIRM/ROTATING) — но из-за этого вкладка
   * «Мониторинг» LOGLSMW в сервисе читала по гироскопу нули (GET_AXES_RAW
   * честно возвращал 0,0,0 с выключенного датчика — замечено на железе
   * 03.07). В SERVICE питание внешнее, экономить нечего — включаем на
   * входе, гасим на выходе (выход = уход в режим A или снятие кабеля,
   * в обоих случаях гироскоп должен быть выключен до решения автомата). */
  LSM6DSO_GYRO_Enable(r->lsm);

  /* Выход из сервиса по PA0 (WKUP1) — 02.07.2026, включено обратно (было
   * отключено if(0) для стендовой отладки, когда WKUP1 физически не
   * детектировался на тогдашнем макете). На стенде теперь есть тумблер,
   * реально управляющий этим уровнем — нужен для проверки настоящего
   * Stop2/пробуждения только по WKUP2 (устройство должно суметь вернуться
   * в SLEEP и оттуда в main.c, если кабель/питание убрали).
   * Дебаунс 3 c непрерывного LOW, НЕ блокируя цикл — старая версия делала
   * blocking while() прямо тут и на время просадки замораживала вообще
   * весь Service() (включая ComPoll()/приём LTP), что не нужно. */
  uint32_t wkup1LowSince = 0;

  while(!QUIT)
  {
    iwdg_kick();   /* рефреш сторожа в сервисном цикле; kick-метку 0x25 в
                    * Сервисе НЕ шлём (18.07.2026) — только «Работа», реальные
                    * RTC-глажения (PushWdgKick из main.c после пробуждения). */
    ComPoll();
    FlashDataEraseTick();   /* дозавершение «Стереть данные» (0x2A), см. выше */

    if (!wkup1_pin_set())
    {
      if (wkup1LowSince == 0)
        wkup1LowSince = HAL_GetTick();
      else if ((HAL_GetTick() - wkup1LowSince) > 3000)
        QUIT = 1;
    }
    else
    {
      wkup1LowSince = 0;
    }
  }

  /* Выход из Service (уход в режим A по 0x1D или снятие кабеля) — гироскоп
   * гасим: дальше им управляет только автомат (см. включение выше). */
  LSM6DSO_GYRO_Disable(r->lsm);
}

/* CMD 0x20 — unsolicited push записи цикла вращения на ПК/стенд.
 * Вызывается после каждого завершённого цикла пока WKUP1=1.
 *
 * Payload (16 байт, little-endian):
 *   [0..5]   start_ts    : RTC_DateTime (year,month,day,hour,min,sec)
 *   [6..9]   duration_s  : uint32  — длительность цикла, секунды
 *   [10..13] total_s     : uint32  — суммарная наработка, секунды
 *   [14..15] avg_rpm     : uint16  — СРЕДНЯЯ скорость за цикл, об/мин («Скорость»)
 */
/* Kick-метка 0x25 из «РАБОТЫ» (18.07.2026): зовётся из main.c при каждом
 * RTC-пробуждении (глажение IWDG). Шлём ТОЛЬКО при кабеле (WKUP1=1) — LOGLSMW
 * моргает строкой «по таймеру». Без кабеля — молчим (эфир пустой, ток целее). */
void PushWdgKick(void)
{
  /* Троттл 20 c (18.07.2026): метка не чаще раза в 20 c — LOGLSMW вспыхивает
   * «по таймеру» на ~2 c раз в 20, а не мигает ежесекундно. Общий для Сервиса
   * и «Работы». По RTC-календарю (идёт всегда). */
  static RTC_DateTime s_lastKick;
  static uint8_t      s_lastKickValid = 0;
  if (!wkup1_pin_set())
    return;
  RTC_DateTime now;
  RTC_GetTimeDate(&now);
  if (s_lastKickValid && RTC_SubTimeDateSec(&now, &s_lastKick) < 20u)
    return;
  s_lastKick      = now;
  s_lastKickValid = 1;
  uint8_t z = 0;
  sendPacket(LTP_DEV_ADDRESS, 0x25, &z, 1);
}

void PushCycleRecord(RegistratorData *r)
{
  /* 02.07.2026: проверка WKUP1 возвращена — теперь функция вызывается НЕ
   * только из Service() (кабель гарантированно есть), но и из общей
   * RotationStateStep(), которую зовёт и автономный WORK-цикл (main.c)
   * без кабеля вообще. Раньше (см. старый комментарий, убран) проверку
   * убрали, т.к. на тогдашнем макете PA0 физически не детектировался
   * (обход if(1||...) в main.c) — сейчас этого обхода в коде уже нет,
   * wkup1_pin_set() отражает реальный уровень пина. Без этой проверки
   * автономный режим на каждом завершённом цикле тратил бы время/энергию
   * на holостую передачу по UART в никуда (кабеля нет — некому слушать). */
  if (!wkup1_pin_set())
    return;

  uint32_t duration_s = RTC_SubTimeDateSec(&r->rot.stopTimeStamp,
                                           &r->rot.startTimeStamp);
  uint16_t avg_rpm  = r->avgRate;        /* [14..15] СРЕДНЕЕ — отчётная «Скорость» */
  /* Расширение пуша (18.07.2026, живые «Данные» во время «Работы»): в ХВОСТ
   * добавлены [16..17] max_rpm, [18..19] vib1_peak мг, [20..21] vib2_peak мг —
   * LOGLSMW дописывает цикл в графики дашборда сразу, не дожидаясь «Стоп»
   * (журнал во сне не читается). Старый разбор (16 байт) не ломается. */
  uint16_t max_rpm  = r->rot.maxRate;
  uint16_t v1p = (r->vibPeak  > 65535.0f) ? 65535u : (uint16_t)(r->vibPeak  + 0.5f);
  uint16_t v2p = (r->jerkPeak > 65535.0f) ? 65535u : (uint16_t)(r->jerkPeak + 0.5f);
  /* [22..23] vib1_RMS мг (18→19.07): «уровень» на графике = RMS (не реагирует
   * на одиночный удар), «пики» = v1p. RMS = sqrt(sumSq/count). */
  float v1rms_f = r->vibLvl;   /* «уровень» = робастная медиана |dev| (19.07.2026) */
  uint16_t v1rms = (v1rms_f > 65535.0f) ? 65535u : (uint16_t)(v1rms_f + 0.5f);

  uint8_t payload[24];
  memcpy(payload,      &r->rot.startTimeStamp, sizeof(RTC_DateTime)); /* 6 */
  memcpy(payload + 6,  &duration_s,            4);
  memcpy(payload + 10, &r->totalSec,           4);
  memcpy(payload + 14, &avg_rpm,               2);
  memcpy(payload + 16, &max_rpm,               2);
  memcpy(payload + 18, &v1p,                   2);
  memcpy(payload + 20, &v2p,                   2);
  memcpy(payload + 22, &v1rms,                 2);

  sendPacket(LTP_DEV_ADDRESS, 0x20, payload, sizeof(payload));

  /* СУБ-СКОРОСТИ ПОЛОК (0x29, 19.07.2026) — ТОЛЬКО в сервис/тест (мы уже под
   * гейтом wkup1 выше), в Flash НЕ уходят. Финализируем последнюю полку и шлём
   * список максимумов полок: LOGLSMW разложит по заданным скоростям цикла.
   * Формат: [count][rpm0_LE..rpmN_LE]. Шлём при >1 полке (одиночная скорость —
   * ничего нового). */
  if (r->plateauInit && r->subCount < PLATEAU_MAX_N)
    r->subMax[r->subCount++] = (r->plateauMax > 65535) ? 65535u
                                : (uint16_t)r->plateauMax;
  if (r->subCount > 1u)
  {
    uint8_t sp[1 + 2 * PLATEAU_MAX_N];
    sp[0] = r->subCount;
    for (uint8_t i = 0; i < r->subCount; i++)
      memcpy(sp + 1 + 2 * i, &r->subMax[i], 2);
    sendPacket(LTP_DEV_ADDRESS, 0x29, sp, (uint16_t)(1u + 2u * r->subCount));
  }
}

void ComPoll()
{
  /* circular DMA работает непрерывно, перезапуск не нужен —
   * dma_restart_needed убран (был источником ошибки: сбрасывал prevFill=0
   * после каждого IDLE, вызывая повторную обработку старых байт; и вызов
   * HAL_UARTEx_ReceiveToIdle_DMA на работающем circular DMA давал HAL_BUSY
   * но уже испортив prevFill). Единственный перезапуск — в ErrorCallback. */

  if(ltp_rx.packet_recognized)
  {
    LtpPacket *pkt = &ltp_rx.pkt;
    /* обрабатываем только запросы (DIR=0) на наш адрес */
    if(pkt->addr == LTP_DEV_ADDRESS && !(pkt->flags & LTP_FLAG_DIR))
    {
      cur_seq = pkt->seq;          /* SEQ запроса копируется в ответ */
      invoke_cmd_handler(pkt);
    }
    ltp_rx.packet_recognized = 0;
  }
}

void invoke_cmd_handler(LtpPacket *wc)
{
  if(wc->cmd < sizeof(CMD)/sizeof(CMD[0]) && CMD[wc->cmd] != NULL)
    CMD[wc->cmd](wc);
  else
    sendError(wc->addr, wc->cmd, LTP_ERR_UNKNOWN_CMD);
}

void cmdPing(LtpPacket *wc)
{
  /* CMD_PING: пустой PAYLOAD, DIR=1 — подтверждение */
  sendPacket(wc->addr, wc->cmd, NULL, 0);
}

/* ⬇⬇⬇ ВЕРСИЯ ПРОШИВКИ — ПРОСТО ЧИСЛА, МЕНЯЮ РУКАМИ ⬇⬇⬇
 * Формат ГГ ММ ДД ЧЧ ММ. Claude ставит сюда текущее время каждый раз, когда
 * заканчивает правку прошивки. Никакого CMake/__TIME__/автогенерации.
 * Прошил → увидел эти числа в «Данные» → значит в контроллере именно эта
 * правка. Не совпало — прошит старый ELF (пересобери и прошей заново). */
#define FW_YY  26   /* год  */
#define FW_MM   8   /* мес  */
#define FW_DD   3   /* число*/
#define FW_HH  22   /* часы */
#define FW_MI  25   /* мин  — активация АВТО-ЗАКРЫВАЕТ предыдущую незакрытую жизнь (END перед новым START) — незакрытых «(идёт)» в середине истории быть не должно; паспорт 0x35/0x36, 03.08.2026 */

/* ИМЯ УСТРОЙСТВА — хранится в КОНТРОЛЛЕРЕ (паспорт; позже перенесём во
 * внутреннюю Flash STM32 как настраиваемый серийник/вариант). LOGLSMW просто
 * показывает, что пришло. Дизайнерский вид: LogLSMA (2 строчные «og»).
 * (финальная проверка индикатора — метка пересборки #2) */
#define DEV_NAME  "LogLSMa"   /* финальная «a» СТРОЧНАЯ — разделитель перед модулями: LogLSMaE00 */

void cmdWhoAmI(LtpPacket *wc)
{
  uint8_t id = 0;
  if(0 == LSM6DSO_ReadID(&lsm, &id))
  {
    /* Ответ: [0]=WHO_AM_I датчика, [1..5]=версия=время сборки ГГ ММ ДД ЧЧ ММ,
     * [6..]=имя устройства (ASCII, с завершающим нулём). Старый разбор
     * (только байт 0) не ломается — лишние байты игнорируются. */
    uint8_t resp[6 + sizeof(DEV_NAME)];
    resp[0] = id;
    resp[1] = FW_YY;
    resp[2] = FW_MM;
    resp[3] = FW_DD;
    resp[4] = FW_HH;
    resp[5] = FW_MI;
    memcpy(&resp[6], DEV_NAME, sizeof(DEV_NAME));   /* с '\0' */
    sendPacket(wc->addr, wc->cmd, resp, sizeof(resp));
  }
  else
    sendError(wc->addr, wc->cmd, LTP_ERR_IMU);
}

/* 0x1D — CMD_START_REGISTER: запуск автомата SLEEP->CONFIRM->ROTATING
 * (режим A, main.c). 02.07.2026: раньше только выходила из Service()
 * (QUIT=1), сам автомат ещё не существовал — main.c и так безусловно
 * крутил WORK-петлю ниже. Теперь автомат запускается ТОЛЬКО этой
 * командой (не по состоянию WKUP1 — см. main.c) — добавлена явная
 * инициализация monitoringActive и чистого состояния SLEEP. */
void cmdStartRegister(LtpPacket *wc)
{
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));  // ACK до выхода из сервиса

  if (s_reg)
  {
    s_reg->monitoringActive = 1;
    s_reg->testNoSleep      = 0;   /* «Работа» — со сном (Standby/Stop2) */
    s_reg->state            = REG_STATE_SLEEP;
    s_reg->confirmPolls     = 0;
    s_reg->noDetectCount    = 0;
    /* Старт СРАЗУ в глубоком сне (deepSleep=1). История 12.07.2026: сначала думали,
     * что deepSleep=1 теряет 1-й цикл — но диагностика на ПОСТОЯННОЙ скорости
     * показала, что «нет ответа» на первых циклах давала СЛАБАЯ ВИБРАЦИЯ низов
     * (5–7 об/мин на тихом стенде), а НЕ позиция и НЕ холодный accel-wake: все
     * прежние прогоны были по ВОЗРАСТАНИЮ, где 1-я позиция = самая медленная =
     * самая слабая вибрация (два фактора совпали). На 10/30 об/мин 1-й цикл
     * приходит всегда. При deepSleep=0 он шёл с −2 c (ждал RTC-тик «Проверки»);
     * deepSleep=1 будит движением в момент реального пуска → 1-й цикл в 0 c, как
     * циклы 2+. Если на холодном старте accel-wake реально не выстрелит — вернуть 0. */
    s_reg->deepSleep        = 1;
    s_reg->idleChecks       = 0;
    /* Гироскоп-троттл (12.07.2026): стартуем с минимального интервала, первый
     * детект — БЕЗ троттла (pending=1), чтобы вибрация после команды поймалась
     * сразу. lastGyroCheck засеваем «сейчас» (валидная метка до первой проверки). */
    s_reg->gyroRecheckSec   = GYRO_RECHECK_MIN_SEC;
    s_reg->gyroCheckPending = 1;
    RTC_GetTimeDate(&s_reg->lastGyroCheck);
  }

  /* ⚠ Wake-Up ПОКОМАНДНО (11.07.2026, ит. 2). Пробуждение по движению ИЗ Stop2
   * подтверждено (05.07) ТОЛЬКО на конфигурации, которую ставит сам
   * Enable_Wake_Up_Detection на 417 Гц HP: не только ODR, но и slope-фильтр,
   * WAKE_DUR/threshold и маршрут wake_up→INT1. 104 Гц LP (загрузочный дефолт
   * main.c) будит детект «Теста» на ходу, но НЕ поднимает ядро из Stop2 —
   * «Работа» не просыпалась (нет ни одного цикла даже во Flash).
   * Ит. 1 (просто SetOutputDataRate 417 HP) НЕ помогла — одного ODR мало, надо
   * ПЕРЕАРМИРОВАТЬ весь детект. Поэтому здесь заново вызываем
   * Enable_Wake_Up_Detection (переустановит и slope-фильтр, и INT1-маршрут),
   * затем явно пиним 417 HP. «Тест» остаётся на 104 LP (cmdStartTest).
   * Ток 417 HP (~170 мкА) — оптимизацию (208/104/52 LP) откладываем. См.
   * session_notes_2026-07-11_work_mode_bench.md. */
  LSM6DSO_ACC_Enable_Wake_Up_Detection(&lsm, LSM6DSO_INT1_PIN);
  /* Порог Wake-Up (12.07.2026). Дефолт драйвера WK_THS=2 (≈62 мг при ±2g) слишком
   * груб для медленного вращения на 1.6 Гц LP: на 6–7 об/мин детект срабатывал
   * поздно (записалось 2–3 с из 10). Снижаем до 1 (≈31 мг, ×2 чувствительность) —
   * раньше срабатывает → и порог по об/мин ниже, и меньше мёртвого времени.
   * Ложные пробуждения безопасны: фаза «Проверка» отсеет их гироскопом.
   * duration=0 (мин.) оставляем — время накопления не нужно. Если 1 даст ложняки
   * от фона стенда/вибрации — вернуть 2 или поднять duration. */
  LSM6DSO_ACC_Set_Wake_Up_Threshold(&lsm, 1);
  LSM6DSO_ACC_Set_Wake_Up_Duration(&lsm, 0);
  /* ODR accel под Wake-Up глубокого сна — ТОК (спуск по лесенке LP, 11–12.07.2026).
   * 417 HP=~170 мкА → 104 LP=~15–17 мкА (×10) → 52 LP=~9.5 мкА (подтв. на стенде
   * 12.07: 10/42/74 об/мин просыпаются) → ПРОБУЕМ 26 LP=~7 мкА. Poll_Sensor ждёт
   * DRDY ГИРОСКОПА → ODR accel на измерение НЕ влияет, нужен лишь для slope-детекта
   * Wake-Up (выборка раз в 1/ODR: 26 Гц ≈ 38 мс). Спин не грозит (EXTI13 армится
   * только в глубоком сне, мотор стоит). ТЕСТ: циклы после пауз/на холодном старте
   * ловятся → рабочая точка; теряются → шаг назад по лесенке.
   * Спуск 12.07: 104→52 (10/42/74 ОК)→26 (времена в нулях, ОК)→1.6 LP=4.5 мкА.
   * 1.6 на 5–7 об/мин теряет циклы, НО это НИЖЕ целевого минимума 10 об/мин —
   * проверяем 1.6 на рабочем диапазоне (10+). Рычаг чувствительности, если надо
   * вытянуть 1.6 на низах — порог Wake-Up (WK_THS/duration), см. ниже. 26 LP=7 мкА
   * — подтверждённый безопасный fallback, если 1.6 не пройдёт и на 10+. */
  /* 12.07.2026: A/B на стенде показал, что 1.6 Гц LP пропускает старт на 8 об/мин
   * (тихий стенд), тогда как «Тест» на 104 Гц LP тот же 8 об/мин ловит +0.0%.
   * Причина — редкая выборка slope-детекта (1.6 Гц ≈ раз в 625 мс проскакивает
   * короткую слабую вибрацию старта). Поднимаем ODR с самого низкого шага лесенки:
   * 26 Гц LP (~7 мкА, выборка ~38 мс) — задокументированный безопасный fallback.
   * Если 26 всё ещё маргинально ловит низы 8–10 — следующий шаг 52 (~9.5) / 104 (~17). */
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 26.0f,
                                          LSM6DSO_ACC_LOW_POWER_NORMAL_MODE);

  QUIT = 1;
}

/* 0x23 — CMD_START_TEST: запуск ТОГО ЖЕ автомата SLEEP->CONFIRM->ROTATING,
 * что и «Работа», но БЕЗ сна (04.07.2026). Отличие ровно одно: флаг
 * testNoSleep=1 → в фазе SLEEP main.c не уходит в Stop2, а бодрствует,
 * обслуживает UART (ComPoll) и ловит фронт INT1/WKUP2 опросом флага WUF2
 * на ходу. Контроллер всё время на связи, видно факт прерывания;
 * детект/CONFIRM/ROTATING/анализ цикла/запись во Flash/пуш — общие с
 * «Работой». Остановка — общая, CMD_STOP_REGISTER (0x22). */
void cmdStartTest(LtpPacket *wc)
{
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));  /* ACK до выхода из сервиса */

  if (s_reg)
  {
    s_reg->monitoringActive = 1;
    s_reg->testNoSleep      = 1;   /* «Тест» — без сна */
    s_reg->state            = REG_STATE_SLEEP;
    s_reg->confirmPolls     = 0;
    s_reg->noDetectCount    = 0;
  }

  /* ODR акселерометра под «Тест» — 104 Гц LP (см. покомандный выбор в
   * cmdStartRegister). Детект по WUF2 на ходу работает на 104 LP; 417 HP,
   * наоборот, ломает детект в run-режиме. Ставим явно на случай, если
   * предыдущая «Работа» подняла ODR до 417 HP. */
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 104.0f,
                                          LSM6DSO_ACC_LOW_POWER_NORMAL_MODE);

  /* Чистый «взвод»: сбрасываем накопленные фронты, чтобы стартовать с
   * ожидания СВЕЖЕГО фронта INT1/WKUP2, а не сработать на старый флаг. */
  __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
  __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF2);

  QUIT = 1;
}

/* 0x22 — CMD_STOP_REGISTER: остановка автомата SLEEP->CONFIRM->ROTATING
 * (режим A) и возврат в Service (03.07.2026). ОСНОВНОЙ путь завершения
 * теста на стенде (согласовано с пользователем): в бодрых фазах автомата
 * (CONFIRM/ROTATING) main.c попутно зовёт ComPoll() — команда доходит.
 * В SLEEP (Stop2) устройство для LTP недостижимо — там дополнительный
 * путь: фронт WKUP1 (перещёлкнуть тумблер, если он есть в конкретной
 * сборке стенда — фишка, не основной механизм; см. WUF1-ветку main.c),
 * либо просто дождаться следующего вращения и послать стоп в бодрой фазе.
 * Идущий цикл НЕ выбрасывается — закрывается тем, что успело накопиться
 * (RotationFinishCycle: запись во Flash + пуш по кабелю).
 * После monitoringActive=0 верхняя проверка main.c на следующей итерации
 * сама заведёт устройство в Service() по уровню WKUP1. */
void cmdStopRegister(LtpPacket *wc)
{
  uint8_t err = er_none;

  if (s_reg)
  {
    if (s_reg->state == REG_STATE_ROTATING)
    {
      RotationFinishCycle(s_reg);
    }
    else if (s_reg->state == REG_STATE_CONFIRM)
    {
      LSM6DSO_GYRO_Disable(s_reg->lsm);
      s_reg->state = REG_STATE_SLEEP;
    }
    s_reg->monitoringActive = 0;
    s_reg->testNoSleep      = 0;   /* общий стоп и для «Теста», и для «Работы» */
  }

  /* Возврат ODR акселерометра на низкопотребляющий дефолт 104 LP после
   * «Работы» (та поднимала до 417 HP ради пробуждения из Stop2) — чтобы в
   * простое/сервисе не жечь ~170 мкА. Для «Теста» это и так было 104 LP. */
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 104.0f,
                                          LSM6DSO_ACC_LOW_POWER_NORMAL_MODE);

  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

/* 0x21 — CMD_RESET_TOTAL: сброс счётчика totalSec по команде с ПК.
 * Вызывается LOGLSMW при нажатии «Старт» циклограммы стенда —
 * гарантирует что следующий CMD_CYCLE_PUSH начнёт с 0 независимо
 * от предстартового вращения мотора. */
void cmdResetTotal(LtpPacket *wc)
{
  if (s_reg) {
    s_reg->totalSec = 0;
    s_reg->state    = 0;  /* сброс текущего цикла: startTimeStamp устарел,
                           * следующий детект вращения запишет чистый старт */
  }
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

/* 0x24 — сброс счётчиков перезапусков: стираем журнал событий внутренней Flash
 * (iflash, стр.124–127) → «по питанию»/«по таймеру» в GET_STATS обнуляются.
 * Для чистой полевой статистики перед выездом. (17.07.2026) */
void cmdResetStats(LtpPacket *wc)
{
  iflash_journal_reset();
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

static uint32_t s_actts;   /* форвард (кэш активации; определение ниже) */

/* 0x27 — «ОЧИСТИТЬ ЖУРНАЛЫ» — ТОЛЬКО очистка ЗАПИСЕЙ журналов, к СОСТОЯНИЮ прибора
 * отношения НЕ имеет (03.08.2026, по уточнению пользователя). Стирает журнал
 * активаций (стр.121, история «жизней») + журнал рестартов (стр.124..127,
 * счётчики). НЕ трогает: NOR-флаги стр.0 (они и есть признак состояния —
 * активирован/данные/сохранение), заводскую стр.123, кэш состояния. Флаг и
 * журнал НЕ связаны: журнал = записи истории, флаг = текущее состояние.
 * Отличие от 0x24 «Сброс WDT» (только счётчики рестартов). */
void cmdClearJournals(LtpPacket *wc)
{
  iflash_journals_clear_all();   /* стр.121 + 124..127, стр.123 и NOR-флаги нетронуты */
  /* s_actts (кэш состояния) НЕ трогаем — состояние ведёт NOR-флаг стр.0, не журнал. */
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

/* 0x2F — GET калибровки скорости: таблица узлов со стр.123 (или дефолт).
 * payload ответа: err(1) | n u8 | n×[ r float(LE) | k float(LE) ]. 03.08.2026. */
void cmdSpeedCalGet(LtpPacket *wc)
{
  float r[IFLASH_SPEEDCAL_MAX], k[IFLASH_SPEEDCAL_MAX];
  uint16_t n = data_speedcal_get(r, k);
  uint8_t ans[2 + IFLASH_SPEEDCAL_MAX * 8];
  ans[0] = er_none;
  ans[1] = (uint8_t)n;
  for (uint16_t i = 0; i < n; i++)
  {
    memcpy(&ans[2 + i*8],     &r[i], 4);
    memcpy(&ans[2 + i*8 + 4], &k[i], 4);
  }
  sendPacket(wc->addr, wc->cmd, ans, (uint16_t)(2 + n*8));
}

/* 0x32 — SET калибровки скорости: записать таблицу на стр.123 + применить.
 * payload запроса: n u8 | n×[ r float(LE) | k float(LE) ]. Ответ: err(1). */
void cmdSpeedCalSet(LtpPacket *wc)
{
  uint8_t err = er_none;
  if (wc->n < 1) { err = er_badarg; sendPacket(wc->addr, wc->cmd, &err, 1); return; }
  uint16_t n = wc->data[0];
  if (n < 2 || n > IFLASH_SPEEDCAL_MAX || wc->n < (uint16_t)(1 + n*8))
  { err = er_badarg; sendPacket(wc->addr, wc->cmd, &err, 1); return; }
  float r[IFLASH_SPEEDCAL_MAX], k[IFLASH_SPEEDCAL_MAX];
  for (uint16_t i = 0; i < n; i++)
  {
    memcpy(&r[i], &wc->data[1 + i*8],     4);
    memcpy(&k[i], &wc->data[1 + i*8 + 4], 4);
  }
  if (data_speedcal_set(r, k, n) != 0) err = er_not_impl;   /* стирание/запись стр.123 не удалась */
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

/* 0x33 — GET калибровки RTC: текущая применённая поправка ppm (float).
 * payload ответа: err(1) | ppm float(LE). 03.08.2026. */
void cmdRtcCalibGet(LtpPacket *wc)
{
  float ppm = rtc_calib_get_ppm();
  uint8_t ans[5];
  ans[0] = er_none;
  memcpy(&ans[1], &ppm, 4);
  sendPacket(wc->addr, wc->cmd, ans, 5);
}

/* 0x34 — SET калибровки RTC: сохранить ppm на стр.123 + применить сразу.
 * payload запроса: ppm float(LE). Ответ: err(1). */
void cmdRtcCalibSet(LtpPacket *wc)
{
  uint8_t err = er_none;
  if (wc->n < 4) { err = er_badarg; sendPacket(wc->addr, wc->cmd, &err, 1); return; }
  float ppm; memcpy(&ppm, wc->data, 4);
  if (rtc_calib_set_ppm(ppm) != 0) err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

/* 0x35 — GET паспорта: серийник/вариант/дата выпуска со стр.123 (03.08.2026).
 * Ответ: err(1) | valid u8 | serial[16] | variant u8 | year u16(LE) | month u8 | day u8.
 * valid=0 → паспорт не задан (поля обнулены). */
void cmdPassportGet(LtpPacket *wc)
{
  char serial[16]; uint8_t variant, month, day; uint16_t year;
  int valid = data_passport_get(serial, &variant, &year, &month, &day);
  uint8_t ans[1 + 1 + 16 + 1 + 2 + 1 + 1];
  ans[0] = er_none;
  ans[1] = (uint8_t)(valid ? 1 : 0);
  memcpy(&ans[2], serial, 16);
  ans[18] = variant;
  memcpy(&ans[19], &year, 2);
  ans[21] = month;
  ans[22] = day;
  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}

/* 0x36 — SET паспорта: записать серийник/вариант/дату на стр.123 (03.08.2026).
 * Запрос: serial[16] | variant u8 | year u16(LE) | month u8 | day u8 = 21 байт.
 * Калибровки в той же странице сохраняются (read-modify-write). Ответ: err(1). */
void cmdPassportSet(LtpPacket *wc)
{
  uint8_t err = er_none;
  if (wc->n < 21) { err = er_badarg; sendPacket(wc->addr, wc->cmd, &err, 1); return; }
  char serial[16];
  memcpy(serial, &wc->data[0], 16);
  serial[15] = '\0';
  uint8_t variant = wc->data[16];
  uint16_t year; memcpy(&year, &wc->data[17], 2);
  uint8_t month = wc->data[19];
  uint8_t day   = wc->data[20];
  if (data_passport_set(serial, variant, year, month, day) != 0) err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

/* 0x2E — ИСТОРИЯ АКТИВАЦИЙ: события «жизней» со стр.121 (03.08.2026).
 * payload ответа: err(1) | count u16(LE) | count×[ type u8 | ts u32(LE) | restarts u16(LE) ].
 * type = EVT_ACTIVATION(0xF3) начало / EVT_ACT_END(0xF4) конец; restarts = общий
 * счётчик перезапусков на момент события (разница конец−начало = за «жизнь»).
 * W разбивает на строки начало→конец. Ограничение — 36 событий (7 Б каждое). */
void cmdActHistory(LtpPacket *wc)
{
  uint8_t  types[36];
  uint32_t ts[36];
  uint16_t rst[36];
  uint16_t n = iflash_activation_events(types, ts, rst, 36);
  uint8_t ans[3 + 36 * 7];
  ans[0] = er_none;
  memcpy(&ans[1], &n, 2);
  for (uint16_t i = 0; i < n; i++)
  {
    ans[3 + i * 7]     = types[i];
    memcpy(&ans[3 + i * 7 + 1], &ts[i],  4);
    memcpy(&ans[3 + i * 7 + 5], &rst[i], 2);
  }
  sendPacket(wc->addr, wc->cmd, ans, (uint16_t)(3 + n * 7));
}

/* 0x26 — ТЕСТ IWDG (18.07.2026): отключаем рефреш сторожа (g_iwdg_on=0) →
 * через ~32 c IWDG бьёт сброс → журнал ловит IWDGRST → счётчик «по таймеру» +1.
 * После сброса рефреш снова включён (boot). Только для проверки сторожа. */
void cmdWdgTest(LtpPacket *wc)
{
  g_iwdg_on = 0;
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

void cmdSetDateTime(LtpPacket *wc)
{
  uint8_t err = er_badarg;
  if(wc->n == sizeof(RTC_DateTime))
  {
    RTC_DateTime *dt = (RTC_DateTime*)wc->data;
    /* Перевод часов ВО ВРЕМЯ идущего цикла (18.07.2026): штампы цикла остаются
     * в СТАРОЙ шкале → длительность рвётся (реальный случай: старт при часах
     * 2000-го после потери питания, ⌚ в бодрой фазе, стоп в 2026-м →
     * durS ≈ 26 лет → наработка 232 705 ч). Пересаживаем все живые штампы на
     * дельту нового времени. */
    if (s_reg && s_reg->state != REG_STATE_SLEEP)
    {
      RTC_DateTime oldNow;
      RTC_GetTimeDate(&oldNow);
      uint32_t elapStart = RTC_SubTimeDateSec(&oldNow, &s_reg->rot.startTimeStamp);
      uint32_t elapLast  = RTC_SubTimeDateSec(&oldNow, &s_reg->rot.lastRotStamp);
      uint32_t elapGyro  = RTC_SubTimeDateSec(&oldNow, &s_reg->lastGyroCheck);
      RTC_SetTimeDate(dt);
      RtcSubSecFrom(dt, elapStart, &s_reg->rot.startTimeStamp);
      RtcSubSecFrom(dt, elapLast,  &s_reg->rot.lastRotStamp);
      RtcSubSecFrom(dt, elapGyro,  &s_reg->lastGyroCheck);
    }
    else
      RTC_SetTimeDate(dt);
    err = er_none;
    
    /*RTC_DateTime dt2;
    RTC_GetTimeDate(&dt2);
    if(0 != memcmp(dt, &dt2, sizeof(RTC_DateTime)))
    {
      err = 128;
    }*/
  }
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

void cmdGetDateTime(LtpPacket *wc)
{
  DateTimeResponse resp;
  RTC_GetTimeDate(&resp.datetime);
  resp.cod = er_none;
  sendPacket(wc->addr, wc->cmd, (uint8_t*)&resp, sizeof(resp));
}

void cmdAccSetOdr(LtpPacket *wc)
{
  uint8_t err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

void cmdGyroSetOdr(LtpPacket *wc)
{
  uint8_t err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

void cmdAccSetFullScale(LtpPacket *wc)
{
  uint8_t err = er_none;
  int32_t *fullscale = (int32_t*)wc->data;
  if(wc->n == sizeof(fullscale))
  {
    switch(*fullscale)
    {
    case 2:
    case 4:
    case 8:
    case 16:
      LSM6DSO_ACC_SetFullScale(&lsm, *fullscale);
      break;
    default:
      err = er_badarg;
    }
  }
  else
  {
    err = er_badarg;
  }
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

void cmdGyroSetFullScale(LtpPacket *wc)
{
  uint8_t err = er_none;
  int32_t *fullscale = (int32_t*)wc->data;
  if(wc->n == sizeof(fullscale))
  {
    switch(*fullscale)
    {
    case 125:
    case 250:
    case 500:
    case 1000:
    case 2000:
      LSM6DSO_GYRO_SetFullScale(&lsm, *fullscale);
      break;
    default:
      err = er_badarg;
    }
  }
  else
  {
    err = er_badarg;
  }
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
}

void cmdAccGetOdr(LtpPacket *wc)
{
  uint8_t err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

void cmdGyroGetOdr(LtpPacket *wc)
{
  uint8_t err = er_not_impl;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

void cmdAccGetFullScale(LtpPacket *wc)
{
  struct FullScale fs = { 0 };
  LSM6DSO_ACC_GetFullScale(&lsm, &fs.fullscale);
  sendPacket(wc->addr, wc->cmd, &fs, sizeof(fs));
}

void cmdGyroGetFullScale(LtpPacket *wc)
{
  struct FullScale fs = { 0 };
  LSM6DSO_GYRO_GetFullScale(&lsm, &fs.fullscale);
  sendPacket(wc->addr, wc->cmd, &fs, sizeof(fs));
}

void cmdGetAxesRaw(LtpPacket *wc)
{
  struct AxesRaw axes = { 0 };
  LSM6DSO_ACC_GetAxesRaw(&lsm, &axes.acc);
  LSM6DSO_GYRO_GetAxesRaw(&lsm, &axes.gyro);
  sendPacket(wc->addr, wc->cmd, &axes, sizeof(axes));
}

void printff(const char *format, ...)
{
  if(format != NULL)
  {
    va_list args1;
    va_start(args1, format);
    int len = vsnprintf((char *)TxBuf, sizeof(TxBuf), format, args1);
    va_end(args1);
    HAL_UART_Transmit(&huart2, TxBuf, len, 1000);
  }
}

void print(char *str)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)str, strlen(str), 1000);
}

void printHex(uint8_t *src, uint32_t size, uint32_t width)
{
  for(uint32_t i=0; i<size; i++)
  {
    printff("%02X ", src[i]);
    if(((i+1) % width) == 0)
      print("\r\n");
  }
}

uint32_t send(uint8_t *buff, uint32_t size)
{
  return HAL_UART_Transmit(&huart2, buff, size, 1000);
}

/*void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == INT1_Pin)
  {
    uint8_t Status1, Status2;
    LSM6DSO_ACC_Get_DRDY_Status(&lsm, &Status1);
    LSM6DSO_GYRO_Get_DRDY_Status(&lsm, &Status2);
    
    if(Status1 || Status2)
    {
      LSM6DSO_ACC_GetAxesRaw(&lsm, &AcqusitionData.acc);
      LSM6DSO_GYRO_GetAxesRaw(&lsm, &AcqusitionData.gyro);
      rot += AcqusitionData.gyro.y;
      N++;
      if(N >= 10)
      {
        rot /= N;
        ///сохранить rot для считывания
        AcqusitionData.rot = rot;
        rot = 0;
        N = 0;
      }
    }
  }
}

void cmdSetAcquisitionData(LtpPacket *wc)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  uint8_t err = er_none;
  uint8_t imuEnable = wc->data[0];
  if(imuEnable != 0)
  {
    ///Проверка датчика
    uint8_t id;
    LSM6DSO_ReadID(&lsm, &id);
    if(id != LSM6DSO_ID)
    {
      err = er_dead_imu;
      sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
      return;
    }
    
    ///Настройка датчика
    LSM6DSO_DRDY_Set_Mode(&lsm, LSM6DSO_DRDY_PULSED);
    LSM6DSO_ACC_Enable_DRDY_On_INT1(&lsm);
    
    ///Прерывание
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = INT1_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(INT1_GPIO_Port, &GPIO_InitStruct);
    
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  }
  else
  {
    HAL_GPIO_DeInit(INT1_GPIO_Port, INT1_Pin);
    HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
    LSM6DSO_ACC_Disable_DRDY_On_INT1(&lsm);
  }
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err)); 
}

void cmdGetAcquisitionData(LtpPacket *wc)
{
  struct AcqusitionDataStruct data;
  
  HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
  memcpy(&data, &AcqusitionData, sizeof(data));
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  sendPacket(wc->addr, wc->cmd, &data, sizeof(data));
}*/

/* ts_activation — метка времени активации.
 * 03.08.2026: ПЕРСИСТЕНТНА снова, но БЕЗОПАСНО — журнал активаций во внутр.
 * Flash стр.121 (iflash_activation_*), append-only. История прошлой попытки:
 * старая метка на стр.121 ЧИТАЛАСЬ в GET_STATS при КАЖДОМ опросе (~5 с); если
 * запись прерывалась питанием (батарейка+вибрация) → битый ECC → NMI/hardfault
 * на каждом GET_STATS, и перепрошивка НЕ лечила (стр.121 не стиралась). Поэтому
 * метку убирали в ОЗУ. ТЕПЕРЬ безопасно: (1) стр.121 читается ОДИН раз на старте
 * Service в кэш s_actts, GET_STATS отдаёт из кэша (горячего чтения Flash нет);
 * (2) пишем ТОЛЬКО append одного слова и ТОЛЬКО под кабелем в Сервисе (питание
 * стабильно, прерывание записи исключено); (3) «Очистить журналы» слепо стирает
 * стр.121 и лечит любой легаси-битый ECC. Деактивации как операции НЕТ (§2.4):
 * журнал накапливает только активации, «не активирован» = стёртая стр.121.
 * 0xFFFFFFFF = не активировано. Кэш инициализируется в activation_cache_init(). */
static uint32_t s_actts = 0xFFFFFFFFu;

static uint32_t actts_read(void)  { return s_actts; }
/* Активация (начало жизни): дописать ts в журнал (persist) + кэш. Под кабелем. */
static void     actts_write(uint32_t ts) { iflash_activation_append(ts); s_actts = ts; }
/* Конец жизни (сохранение/снятие с активации): дописать END + закрыть кэш. */
static void     actts_end(uint32_t ts)   { iflash_activation_end(ts);    s_actts = 0xFFFFFFFFu; }

/* Инициализация кэша активации на старте Service (внешнее питание): один раз
 * прочитать журнал стр.121 → s_actts. ensure_ready лечит мусорную страницу. */
void activation_cache_init(void)
{
  iflash_activation_ensure_ready();
  /* Текущее состояние активации = NOR-байт [1] стр.0 (ОСНОВНОЙ признак). ts берём
   * из журнала стр.121 только когда байт взведён; иначе «не активирован». Так
   * состояние не расходится с байтом после стирания данных. 03.08.2026. */
  s_actts = data_norflag_get(NOR_FLAG_ACTIVATED)
              ? iflash_activation_last() : 0xFFFFFFFFu;
}

/* 0x1E — GET_STATS
 * payload (23 байта): cod(1) | total_sec u32 | ts_first u32 | ts_last u32
 *                   | ts_activation u32 | restarts_timer u16 | restarts_power u16
 *                   | mode u8 | stats_ver u8
 * [1..4] total_sec — lifetime наработка в секундах (из s_reg->totalSec,
 *   загружается из Flash при старте Service, растёт с каждым циклом вращения;
 *   поле дублирует durationTotal последней Flash-записи, но всегда актуально
 *   из ОЗУ даже если текущий цикл ещё не завершён).
 * Остальные поля — заглушки (0xFF/0x00). */
void cmdGetStats(LtpPacket *wc)
{
  uint8_t ans[23];
  uint32_t val32;
  uint16_t val16;

  ans[0] = er_none;

  val32 = s_reg ? s_reg->totalSec : 0u;     /* [1..4] lifetime total_sec из ОЗУ */
  memcpy(&ans[1], &val32, 4);

  val32 = 0xFFFFFFFFu;                        /* ts_first — нет данных */
  memcpy(&ans[5], &val32, 4);

  val32 = 0xFFFFFFFFu;                        /* ts_last  — нет данных */
  memcpy(&ans[9], &val32, 4);

  val32 = actts_read();                       /* ts_activation из КЭША s_actts (журнал стр.121 прочитан на старте Service — БЕЗ горячего чтения Flash) */
  memcpy(&ans[13], &val32, 4);

  /* Реальные счётчики рестартов из журнала внутренней Flash (06.07.2026,
   * раньше были жёсткие нули). Переживают потерю питания (iflash, Page A).
   * (Диагностику Stop2 через это поле откатили 11.07: питание стирает и ОЗУ, и
   * backup, поэтому счётчик, читаемый после передёргивания, бесполезен —
   * заменён на живую метку 0x7E при выходе из Stop2, см. main.c.) */
  val16 = iflash_journal_count(EVT_WATCHDOG);  /* restarts_timer (зависоны, сторож) */
  memcpy(&ans[17], &val16, 2);

  val16 = iflash_journal_count(EVT_POWERLOSS); /* restarts_power (потери питания) */
  memcpy(&ans[19], &val16, 2);

  ans[21] = 0;                                /* mode: 0 = SERVICE */
  ans[22] = 1;                                /* stats_ver */

  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}

void cmdFlashOn(LtpPacket *wc)
{
  /* s_flash_on — файловая переменная, общая с Service():
   * если Flash уже инициализирована там, повторный flashActiv() не нужен. */
  uint8_t f = er_none;
  if (!s_flash_on)
  {
    flashActiv();
    s_flash_on = 1;
  }
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

void cmdFlashOff(LtpPacket *wc)
{
  uint8_t f;
  FlashOff();
  f = er_none;
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

void cmdReadMem(enum MemoryType mem, LtpPacket *wc)
{
  uint8_t page[257];
  
  if(wc->n < 8)
    return;
  
  uint32_t addr = *(uint32_t*)(wc->data + 0);
  uint32_t size = *(uint32_t*)(wc->data + 4);
  addr &= 0x00FFFFFF;
  if(size > 256)
    size = 256;
  
  page[0] = er_none;
  
  if(mem == m_eeprom)
    P25Qx_QPI_Read(&flash, addr, size, &page[1]);
  else
    fm25xx_readMultiple(&hspi1, addr, &page[1], size);
  
  sendPacket(wc->addr, wc->cmd, page, 1+size);
}

void cmdWriteMem(enum MemoryType mem, LtpPacket *wc)
{
  if(wc->n < 2)
    return;
  
  uint16_t pg_num = *(uint16_t*)(wc->data + 0);
  
  uint32_t nWrite = wc->n-2;
  if(nWrite > 256)
    nWrite = 256;
  
  if(mem == m_eeprom)  
    //FlashPageProgram(&wc->data[2], (pg_num << 8), nWrite);
    P25Qx_QPI_ProgramPage(&flash, (pg_num << 8), &wc->data[2], nWrite);
  else
    fm25xx_writeMultiple(&hspi1, (pg_num << 8), &wc->data[2], nWrite);
  
  uint8_t f = er_none;
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

void cmdFramWritePageHandler(LtpPacket *wc)
{
  cmdWriteMem(m_fram, wc);
}

void cmdFramReadHandler(LtpPacket *wc)
{
  cmdReadMem(m_fram, wc);
}

void cmdSetBaud(LtpPacket *wc)
{
  uint8_t err = er_none;
  if(wc->n < 4)
    return;
  
  uint32_t newBaud = *(uint32_t*)(wc->data + 0);
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));
  
  HAL_UART_DMAStop(&huart2);
  HAL_UART_DeInit(&huart2);
  huart2.Init.BaudRate = newBaud;
  HAL_UART_Init(&huart2);
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf));
}

void cmdGetTemp(LtpPacket *wc)
{
  uint8_t ans[5];
  float temp = tmp117_get_Temp(&hi2c2);
  ans[0] = er_none;
  memcpy(&ans[1], &temp, 4);
  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}

void cmdGetTempIMU(LtpPacket *wc)
{
  uint8_t ans[5];
  float temp;
  LSM6DSO_GetTemp(&lsm, &temp);
  ans[0] = er_none;
  memcpy(&ans[1], &temp, 4);
  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}
/////////////////////////////////////////////////////////////////////////////////
void cmdGetTempChip(LtpPacket *wc)
{
  /* 0x11: cod | temp float(4) | vdda float(4) — STM32 кристалл + питание.
   * stm_adc_read безопасен (по запросу, с таймаутами, не вешает прибор). */
  uint8_t ans[9];
  float temp = 0.0f, vdda = 0.0f;
  int ok = stm_adc_read(&vdda, &temp);
  ans[0] = ok ? er_none : er_timeout;
  memcpy(&ans[1], &temp, 4);
  memcpy(&ans[5], &vdda, 4);
  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}

void cmdFlashReadIDHandler(LtpPacket *wc)
{
  uint32_t id = P25Qx_QPI_ReadID(&flash);
  uint8_t *pID = (uint8_t*)&id;
  uint8_t ans[4] = {er_none, pID[0], pID[1], pID[2]};
  sendPacket(wc->addr, wc->cmd, &ans, sizeof(ans));
}

void cmdFlashGetState(LtpPacket *wc)
{
  union StatusRegBits st;
  st.reg[1] = P25Qx_ReadSR(&flash, 1);
  uint8_t ans[] = {er_none, st.flag.WIP};
  sendPacket(wc->addr, wc->cmd, &ans, sizeof(ans));
}

void cmdFlashChipEraseHandler(LtpPacket *wc)
{
  uint8_t f = er_none;

  P25Qx_QPI_EraseChip(&flash);

  /* Стёрли чип = устройство «с нуля» → lifetime-наработку обнуляем.
   * ВАЖНО: НЕ через LoadTotalSec — стирание чипа АСИНХРОННОЕ (P25Qx_QPI_EraseChip
   * лишь запускает bulk erase, WIP держится ~30 с, ПК потом опрашивает
   * FLASH_STATE). Чтение Flash в этот момент вернуло бы ещё НЕ стёртую
   * последнюю запись → старые 352 ч (баг первой версии фикса, 04.07.2026).
   * Полное стирание = Flash точно пуст → просто обнуляем totalSec напрямую.
   * Курсор записи не трогаем: SaveParamOnEEPROM всегда сканирует Flash заново
   * (flashFindWritePos), после завершения стирания сам найдёт стр. 1. */
  if (s_reg)
    s_reg->totalSec = 0u;

  data_flag_clear();   /* нет данных → флаг «несохранённые» сброшен (зелёный) */

  sendPacket(wc->addr, wc->cmd, &f, 1);
}

/* 0x2A — «Стереть данные» (21.07.2026, по замечанию: стр.0 — служебная
 * информация/паспорт, реальные данные регистратора со стр.1, обычное
 * «Стереть» 0x05 стирает и её).
 *
 * P25Q не умеет стирать «весь чип, кроме одной страницы» одной командой —
 * bulk-erase (0x60) стирает всё. Секторное/постраничное стирание вручную по
 * циклу заняло бы значительно дольше (4096 секторов) — неприемлемо.
 * Поэтому: читаем стр.0 в буфер ДО стирания, запускаем обычный bulk-erase (тот
 * же, что и 0x05), и ПОСЛЕ того как чип освободится (WIP=0) — пишем стр.0
 * назад. Само стирание асинхронное (Wait_Busy тут не зовём, см. EraseChip),
 * поэтому запись стр.0 нельзя делать сразу — чип ещё занят bulk-erase, любая
 * PROGRAM PAGE в это время не пройдёт. Дозавершение — FlashDataEraseTick(),
 * дёргается из главного цикла Service() (while(!QUIT), после ComPoll()). */
static uint8_t          s_page0Backup[P25Q_PAGE_SIZE];
static volatile uint8_t s_dataEraseArmed = 0;

void cmdFlashDataEraseHandler(LtpPacket *wc)
{
  uint8_t f = er_none;

  P25Qx_QPI_Read(&flash, 0, P25Q_PAGE_SIZE, s_page0Backup);
  /* Стёрли данные = новая «жизнь»: сбросить ВСЕ NOR-флаги стр.0 [1]активирован /
   * [2]данные / [3]сохранение (в восстанавливаемой стр.0 пишем базу 0xFF). Текущее
   * состояние = «не активирован» до повторной активации; ts-история в стр.121
   * сохраняется (отдельная память, чистится «Очистить журналы» 0x27). 03.08.2026. */
  s_page0Backup[NOR_FLAG_ACTIVATED] = 0xFFu;
  s_page0Backup[NOR_FLAG_DATA]      = 0xFFu;
  s_page0Backup[NOR_FLAG_SAVED]     = 0xFFu;
  s_actts = 0xFFFFFFFFu;   /* кэш активации → «не активирован» (байт [1] сброшен) */

  P25Qx_QPI_EraseChip(&flash);
  s_dataEraseArmed = 1;   /* дозавершение — см. FlashDataEraseTick() */

  /* Как и при обычном стирании чипа — lifetime-наработка обнуляется. */
  if (s_reg)
    s_reg->totalSec = 0u;

  data_flag_clear();   /* нет данных → флаг «несохранённые» сброшен (зелёный) */

  sendPacket(wc->addr, wc->cmd, &f, 1);
}

/* Вызывать из Service() на каждом обороте главного цикла (после ComPoll()).
 * Пока чип ещё занят стиранием — просто выходит сразу (дешёвая проверка флага). */
void FlashDataEraseTick(void)
{
  if (!s_dataEraseArmed)
    return;

  union StatusRegBits st;
  st.reg[1] = P25Qx_ReadSR(&flash, 1);
  if (st.flag.WIP)
    return;   /* чип ещё стирается — проверим на следующем обороте */

  P25Qx_QPI_ProgramPage(&flash, 0, s_page0Backup, P25Q_PAGE_SIZE);
  s_dataEraseArmed = 0;
}

/* 0x2B — режим SPI, ТОЛЬКО для ручной проверки в Сервисе (22.07.2026, по
 * просьбе). В «Работе» команда не шлётся вообще — там автоматический выбор
 * (см. session_notes про энергорежимы: минимум периферии → одна линия SPI,
 * standart_mode). SPIx2 (dual_mode) исключён — понадобится, может быть,
 * только при большом составе периферии в некоторых конфигурациях; когда
 * дойдёт — доделаем тогда же, сейчас в enum P25Qx_Mode он есть, но нигде не
 * реализован (ни в одной read/write/erase-функции p25q128.c).
 * payload[0]: 0 = SPI (standart_mode, все линии по одной),
 *             1 = SPIx4 (quadspi_mode, данные по 4 линиям — как раньше).
 *
 * ФИКС (22.07.2026, по факту с железа): первая версия просто переставляла
 * ПРОГРАММНЫЙ флаг flash.mode — а САМ ЧИП физически оставался в том
 * протоколе, в котором был раньше (например, в QPI/4 линии после сна). Софт
 * и чип расходились → последующие команды сыпались (SPI: чтение обрывалось
 * на середине; SPIx4: сплошной мусор 0xDD). P25Qx_Reset() шлёт команду
 * сброса ОБОИМИ протоколами (сначала 4-линейно, потом 1-линейно) — реально
 * выводит чип из QPI независимо от того, в каком состоянии он был, и кладёт
 * его в заводской SPI-дефолт. Зовём её перед КАЖДЫМ переключением — тогда
 * софт и чип гарантированно синхронны. */
void cmdFlashSetSpiModeHandler(LtpPacket *wc)
{
  uint8_t f = er_none;
  switch (wc->data[0]) {
  case 0:
    P25Qx_Reset(&flash);        /* физически выводит чип из QPI → SPI (x1) */
    break;
  case 1:
    P25Qx_Reset(&flash);        /* тот же чистый старт, независимо от прошлого состояния */
    P25Qx_SetQuadSpi(&flash);   /* включает QE, переводит в quadspi_mode */
    break;
  default:
    f = er_badarg;
    break;
  }
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

/* 0x2C — частота флеша (26.07.2026). Сервис работает на 80 МГц (см.
 * ServiceClock_Config), QSPI тактируется от HCLK=80. ClockPrescaler задаёт
 * делитель: F_флеша = 80/(Prescaler+1). payload[0] — индекс частоты:
 *   0 → 80 МГц (Prescaler 0), 1 → 40 (1), 2 → 20 (3), 3 → 10 (7), 4 → 5 (15).
 * Меняем делитель и переинициализируем QSPI; чип после reinit сбрасываем и
 * (для SPIx4) заново включаем quad, чтобы софт и чип были синхронны. */
static const uint8_t s_freqPrescaler[5] = { 0, 1, 3, 7, 15 };  /* 80/40/20/10/5 МГц */

void cmdFlashSetFreqHandler(LtpPacket *wc)
{
  uint8_t f = er_none;
  if (wc->n < 1 || wc->data[0] > 4) { f = er_badarg; sendPacket(wc->addr, wc->cmd, &f, 1); return; }

  uint8_t wasQuad = (flash.mode == quadspi_mode);   /* сохранить протокол */

  HAL_QSPI_DeInit(&hqspi);
  hqspi.Init.ClockPrescaler = s_freqPrescaler[wc->data[0]];
  if (HAL_QSPI_Init(&hqspi) != HAL_OK) { f = er_timeout; sendPacket(wc->addr, wc->cmd, &f, 1); return; }

  /* чип и софт синхронизируем как в 0x2B */
  P25Qx_Reset(&flash);
  if (wasQuad) P25Qx_SetQuadSpi(&flash);

  sendPacket(wc->addr, wc->cmd, &f, 1);
}

/* 0x30 — флаг «есть непрочитанные данные» во ВНУТРЕННЕЙ Flash (27.07.2026).
 * Отдельная страница 122 (НЕ трогаем паспорт стр.123). Взводится на стенде по
 * окончании инициализации, сбрасывается при считывании архива. Пишется только в
 * Сервисе (внешнее питание); в рабочем цикле на батарее не трогаем. L4 ECC:
 * страница пишется раз после стирания → взвод = erase+program магии, сброс =
 * erase (страница = 0xFF). Приложение по этому флагу блокирует стирание, пока
 * данные не считаны. payload[0]: 0=прочитать, 1=взвести, 2=сбросить.
 * ответ: [0]=err, [1]=состояние (1=взведён/непрочитано, 0=сброшен/прочитано). */
/* Битовая маска состояния (для ответа 0x30 и внутренних решений).
 * 0x01 активирован (стр.121 ts — авторитет), 0x02 данные_есть, 0x04 сохранено. */
static uint8_t data_flags_mask(void)
{
  /* ОДНО чтение первых 3 байт стр.0 вместо трёх отдельных QSPI-read (меньше
   * нагрузки на шину; вызывается на каждый 0x30, W шлёт их часто). 03.08.2026. */
  uint8_t hdr[3] = { 0xFFu, 0xFFu, 0xFFu };
  P25Qx_QPI_Read(&flash, 0u, 3u, hdr);
  uint8_t m = 0;
  if (hdr[NOR_FLAG_ACTIVATED] == 0x00u) m |= 0x01u;   /* offset 0 */
  if (hdr[NOR_FLAG_DATA]      == 0x00u) m |= 0x02u;   /* offset 1 */
  if (hdr[NOR_FLAG_SAVED]     == 0x00u) m |= 0x04u;   /* offset 2 */
  return m;
}

/* Взвести «данные_есть» (шим совместимости; напр. после загрузки образа). */
void data_flag_set_if_clear(void)
{
  if (!data_norflag_get(NOR_FLAG_DATA))
    data_norflag_set(NOR_FLAG_DATA);
}

/* Сброс NOR-флагов данных/сохранения делается СТИРАНИЕМ стр.0 (в erase-
 * обработчиках через базу), поштучно занулять нельзя. Здесь — заглушка
 * совместимости для старых вызовов из путей стирания. */
void data_flag_clear(void) { /* NOR: сброс = стирание стр.0, см. 0x2A/EraseChip */ }

/* 0x30 — ФЛАГИ СОСТОЯНИЯ в служебной стр.0 NOR (03.08.2026, замена внутр. Flash
 * STM стр.122 DATAFLAG — та была опасна: erase+program + ECC на батарее). Байты
 * стр.0 (Data.h) = ОСНОВНОЙ признак состояния: [1]=активирован, [2]=данные_есть,
 * [3]=сохранение(=конец жизни). Журнал стр.121 хранит ts-историю (не дубль байта).
 * Взвод = program байта →0x00 без стирания (безопасно на батарее). Сброс всех —
 * стиранием данных (новая жизнь). Активация: байт [1] + ts в стр.121.
 * payload[0]: 0=прочитать, 1=активация(+ts[1..4]), 2/3=сохранение, 4=данные появились.
 * ответ [0]=err, [1]=маска (0x01 активирован, 0x02 данные, 0x04 сохранено). */
void cmdDataFlagHandler(LtpPacket *wc)
{
  uint8_t resp[2] = { er_none, 0 };
  uint8_t action = (wc->n >= 1) ? wc->data[0] : 0;
  if (action == 1) {                 /* активация: NOR-байт [0] (осн. признак) +
                                        START в журнал стр.121. ⚠ 03.08.2026:
                                        активация АВТО-ЗАКРЫВАЕТ предыдущую жизнь,
                                        если её не закрыли сохранением — незакрытых
                                        «(идёт)» в середине истории быть не должно. */
    data_norflag_set(NOR_FLAG_ACTIVATED);
    /* Журнал рестартов (стр.124..127) НЕ сбрасываем — он копит ВСЕ перезапуски
     * (общий счётчик). «За цикл/жизнь» считается РАЗНИЦЕЙ: текущий счётчик −
     * счётчик на момент этой активации (сохранён в слоте стр.121 [6..7]). */
    if (wc->n >= 5) {
      uint32_t ts; memcpy(&ts, wc->data + 1, 4);
      /* предыдущая жизнь ещё открыта (последнее событие = START)? закрыть END
       * той же меткой (конец старой = начало новой), затем открыть новую. */
      if (iflash_activation_last() != 0xFFFFFFFFu) iflash_activation_end(ts);
      actts_write(ts);
    }
  } else if (action == 2) {          /* сохранение (жизнь НЕ закрываем) → флаг [3] */
    data_norflag_set(NOR_FLAG_SAVED);
  } else if (action == 3) {          /* сохранение + СНЯТИЕ с активации: END в
                                        журнал стр.121 (ts из payload) + флаг [3]. */
    if (wc->n >= 5) { uint32_t ts; memcpy(&ts, wc->data + 1, 4); actts_end(ts); }
    data_norflag_set(NOR_FLAG_SAVED);
  } else if (action == 4) {          /* данные появились (загрузка образа) → [2] */
    data_norflag_set(NOR_FLAG_DATA);
  }
  resp[1] = data_flags_mask();
  sendPacket(wc->addr, wc->cmd, resp, 2);
}

/* 0x31 — ФОРМАТ ЗАПИСИ ЦИКЛА (маркёр слова). Приложение (Настройки) задаёт:
 * 0 базовый / 1 уплотнённый / 2 подробный. Хранится в ОЗУ (Stop2 сохраняет);
 * персистентности пока нет — приложение шлёт формат при каждом подключении.
 * Data.c::SaveParamOnEEPROM берёт маркёр через rec_format_marker(). 28.07.2026.
 * ⚠ ТЕЛО уплотнённой/подробной записи пока = базовое 48Б (состав TODO) — сейчас
 * различается только маркёр [0]; байтовую раскладку уплотн/подроб зададим позже. */
static uint8_t s_recFormat = 0u;   /* 0 базовый по умолчанию */

uint8_t rec_format_marker(void)
{
  switch (s_recFormat) {
    case 1u:  return 0xF3u;   /* уплотнённый */
    case 2u:  return 0xF4u;   /* подробный */
    default:  return 0xF5u;   /* базовый */
  }
}

void cmdRecFormatHandler(LtpPacket *wc)
{
  uint8_t err = er_none;
  if (wc->n >= 1u && wc->data[0] <= 2u)
    s_recFormat = wc->data[0];
  else
    err = er_badarg;
  sendPacket(wc->addr, wc->cmd, &err, 1);
}

/* 0x2D — замер скорости флеша по ЭТАЛОННОЙ СЕКУНДЕ RTC (26.07.2026, финал).
 * Никаких тактов и делителей: синхронизируемся на границу секунды RTC, ровно
 * одну секунду гоняем операции с чипом по кругу в заданной области, считаем
 * сколько страниц успели. Результат — страниц/с, приложение × 256 = байт/с.
 * payload: [0]=режим (0=запись, 1=чтение), [1..4]=старт.страница, [5..8]=стр.
 * ответ: [0]=err, [1..4]=число страниц за секунду (uint32 LE). */
void cmdFlashSpeedTestHandler(LtpPacket *wc)
{
  uint8_t resp[5] = {0};
  if (wc->n < 9) { resp[0] = er_badarg; sendPacket(wc->addr, wc->cmd, resp, 5); return; }

  uint8_t  mode      = wc->data[0];
  uint32_t startPage = *(uint32_t*)(wc->data + 1);
  uint32_t nPages    = *(uint32_t*)(wc->data + 5);
  if (nPages == 0) { resp[0] = er_badarg; sendPacket(wc->addr, wc->cmd, resp, 5); return; }

  static uint8_t buf[256];
  for (uint16_t i = 0; i < 256; ++i) buf[i] = (uint8_t)i;

  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  /* синхронизация на границу секунды RTC */
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  uint8_t s0 = t.Seconds;
  while (1) {
    HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
    if (t.Seconds != s0) break;
  }
  uint8_t  sPrev = t.Seconds;
  uint32_t secs  = 0;              /* сколько секунд эталона прошло */

  /* ровно 5 секунд операций по кругу в области (точнее, чем 1 с) */
  uint32_t count = 0, p = 0;
  static uint8_t vbuf[256];

  /* ЧТЕНИЕ — через memory-mapped (26.07.2026): контроллер сам подкачивает по
   * AHB, без CPU-поллинга FIFO (прежний потолок ~820 КБайт/с). Вход один раз
   * до цикла, выход после. Запись (mode 0) остаётся indirect. */
  if (mode == 1) P25Qx_MemMapped(&flash);

  while (1) {
    uint32_t a = (startPage + p) << 8;
    if (mode == 0) {
      /* Реальная скорость ЗАПИСИ: erase+program, затем ПОДТВЕРЖДЕНИЕ чтением-
       * сверкой. Ожидание WIP в драйвере ненадёжно — выходит на преждевременном
       * WIP=0 сразу после команды, из-за чего счётчик крутился вхолостую
       * (абсурдные 50000 стр/с > скорости чтения). Единицу считаем ТОЛЬКО когда
       * страница реально записалась (readback == эталон) — это не зависит от WIP
       * и даёт честное число завершённых записей в секунду. */
      P25Qx_QPI_ErasePage(&flash, a);
      P25Qx_QPI_ProgramPage(&flash, a, buf, 256);
      P25Qx_QPI_Read(&flash, a, 256, vbuf);
      uint8_t ok = 1;
      for (uint16_t i = 0; i < 256; ++i) if (vbuf[i] != buf[i]) { ok = 0; break; }
      if (ok) { ++count; if (++p >= nPages) p = 0; }   /* не подтвердилось — повторим ту же стр. */
    } else {
      /* ЧТЕНИЕ — последовательный проход ВСЕЙ области большими бёрстами:
       * memory-mapped стримит непрерывно (CS держится, адрес авто-инкремент),
       * доминирует фаза данных → видна разница линий/частоты и реальный потолок
       * дампа, а не постраничные накладные (из-за них 1 и 4 линии давали одно). */
      const volatile uint32_t *mp = (const volatile uint32_t*)(P25Q_MMAP_BASE + (startPage << 8));
      uint32_t words = (nPages << 8) >> 2;   /* nPages*256/4 слов */
      volatile uint32_t acc = 0;
      for (uint32_t i = 0; i < words; ++i) acc += mp[i];
      (void)acc;
      count += nPages;                        /* прочитали всю область за проход */
    }

    HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
    if (t.Seconds != sPrev) { sPrev = t.Seconds; if (++secs >= 5) break; }
  }

  /* КРИТЕРИЙ ЦЕЛОСТНОСТИ ЧТЕНИЯ (26.07.2026): один проход сверки прочитанного
   * с эталоном buf[i]=i, который фаза записи (mode 0) положила в ту же область.
   * Скорость мерена чистым чтением выше (сверка её не занижает), а здесь
   * подтверждаем ДАННЫЕ. Не сошлось → выбранная конфигурация (частота/линии)
   * для чтения ненадёжна — цифру скорости показывать нельзя. */
  uint32_t badWords = 0;
  if (mode == 1) {
    const volatile uint32_t *mp = (const volatile uint32_t*)(P25Q_MMAP_BASE + (startPage << 8));
    uint32_t words = (nPages << 8) >> 2;
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t bo  = (i << 2) & 0xFF;                 /* смещение байта в странице, кратно 4 */
      uint32_t exp = bo | ((bo+1)<<8) | ((bo+2)<<16) | ((bo+3)<<24);
      if (mp[i] != exp) ++badWords;
    }
  }

  if (mode == 1) P25Qx_ExitMemMapped(&flash);   /* вернуть indirect для обычных команд */

  resp[0] = er_none;
  if (mode == 0 && count == 0)    resp[0] = er_timeout;  /* запись реально не идёт */
  else if (mode == 1 && badWords) resp[0] = er_timeout;  /* чтение не сошлось с эталоном */
  *(uint32_t*)(resp + 1) = count / 5;   /* страниц в секунду */
  sendPacket(wc->addr, wc->cmd, resp, 5);
}

void cmdFlashReadHandler(LtpPacket *wc)
{
  cmdReadMem(m_eeprom, wc);
}

void cmdFlashWritePageHandler(LtpPacket *wc)
{
  cmdWriteMem(m_eeprom, wc);
}

void cmdFlashPageEraseHandler(LtpPacket *wc)
{
  uint8_t f = er_none;

  if(wc->n < 2)
    return;

  uint16_t pg_num = *(uint16_t*)(wc->data + 0);

  P25Qx_QPI_ErasePage(&flash, pg_num << 8);
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

/* 0x1F — стирание сектора 4 КБ = 16 страниц
 * payload: sector_num uint16 LE (сектор 0..4095 для P25Q128H 16 МБ)
 * addr = sector_num * 4096 */
void cmdFlashSectorEraseHandler(LtpPacket *wc)
{
  uint8_t f = er_none;

  if (wc->n < 2)
    return;

  uint16_t sec_num = *(uint16_t *)(wc->data + 0);
  uint32_t addr    = (uint32_t)sec_num * 4096u;

  P25Qx_QPI_EraseSector(&flash, addr);
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

////////////////////////////////////////////////////////////////////////////////
/*void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
  ReadReceiveBuffer(huart, prevFill, sizeof(RxBuf)/2-prevFill);
  prevFill = sizeof(RxBuf)/2;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  ReadReceiveBuffer(huart, prevFill, sizeof(RxBuf)-prevFill);
  prevFill = 0;
}*/

/*void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  HAL_UART_Receive_IT(&huart2, RxBuf, sizeof(RxBuf));
}*/

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t fill)
{
  if(prevFill <= fill)
  {
    /* Обычный случай: новые данные в [prevFill, fill) */
    ReadReceiveBuffer(huart, prevFill, fill - prevFill);
  }
  else
  {
    /* Wrap-around: читаем [prevFill, конец) + [0, fill) */
    ReadReceiveBuffer(huart, prevFill, sizeof(RxBuf) - prevFill);
    ReadReceiveBuffer(huart, 0, fill);
  }
  prevFill = fill;
  /* circular DMA продолжает работать сам — перезапуск не нужен */
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if(huart != &huart2) return;
  /* ORE/FE/NE после переподключения VCP: сбрасываем позицию и парсер,
   * принудительно переводим RxState в READY (HAL_UART_DMAAbortOnError
   * не всегда делает это), перезапускаем DMA прямо из ISR — тот же
   * паттерн, что в стенде (восстановление за ~0.5 с). */
  prevFill = 0;
  ltp_parser_init(&ltp_rx);
  huart2.RxState = HAL_UART_STATE_READY;
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf));
  __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);
}

void ReadReceiveBuffer(UART_HandleTypeDef *huart, uint32_t start, uint32_t size)
{
  for(uint32_t i=0; i<size; i++)
    ltp_parser_feed(&ltp_rx, RxBuf[start+i]);
}
////////////////////////////////////////////////////////////////////////////////

/* Найдено 02.07.2026 вечером диагностикой по сырым LTP-байтам (в обход
 * LOGLSMW, PowerShell-скрипт напрямую на COM-порт): FLASH_READ (0x07) на
 * size=8..240 отвечает всегда корректно, а на size>=250 (полный пакет
 * ответа >~250-260 байт) ответ обрывается — приходит только "хвост"
 * (число долетевших байт растёт линейно 1-в-1 с size сверх порога, значит
 * начало пакета теряется, а не искажается). Это классическая картина
 * переполнения буфера дешёвого USB-UART моста при непрерывной передаче
 * длинного куска на 921600 без аппаратного flow control
 * (HwFlowCtl=UART_HWCONTROL_NONE, usart.c) — мост не успевает сливать
 * данные по USB и роняет середину пакета. LOGLSMW всегда запрашивает
 * целую страницу (256 байт) → отсюда систематическое "нет ответа" на
 * FLASH_READ, которое казалось багом прошивки/QSPI, а на самом деле
 * проявлялось только на этапе передачи готового ответа по UART.
 * Лечится программно: не слать весь TxBuf одним HAL_UART_Transmit, а
 * бить на некрупные куски с небольшой паузой между ними — даёт мосту
 * время слить внутренний буфер по USB, не меняя сам протокол LTP и не
 * требуя изменений на стороне LOGLSMW. */
static void UartTransmitChunked(uint8_t *buf, uint16_t len)
{
  const uint16_t CHUNK = 64;
  uint16_t sent = 0;
  while (sent < len)
  {
    uint16_t n = (uint16_t)((len - sent > CHUNK) ? CHUNK : (len - sent));
    HAL_UART_Transmit(&huart2, buf + sent, n, 1000);
    sent = (uint16_t)(sent + n);
    if (sent < len)
      HAL_Delay(2);
  }
}

/* Ответ устройства: FLAGS.DIR=1, SEQ копируется из текущего запроса */
void sendPacket(uint8_t addr, uint8_t cmd, void *data, uint32_t n)
{
  int size = ltp_build(TxBuf, sizeof(TxBuf),
                       addr, cmd, LTP_FLAG_DIR, cur_seq,
                       (const uint8_t*)data, (uint16_t)n);
  if(size > 0)
    UartTransmitChunked(TxBuf, (uint16_t)size);
}

/* Ошибочный ответ: FLAGS.DIR=1 + FLAGS.ERR=1, PAYLOAD = 1 байт кода ошибки */
void sendError(uint8_t addr, uint8_t cmd, uint8_t err_code)
{
  int size = ltp_build(TxBuf, sizeof(TxBuf),
                       addr, cmd, LTP_FLAG_DIR | LTP_FLAG_ERR, cur_seq,
                       &err_code, 1);
  if(size > 0)
    UartTransmitChunked(TxBuf, (uint16_t)size);
}
