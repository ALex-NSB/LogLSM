#include "com.h"
#include "ltp.h"
#include "pwr.h"
#include "globals.h"
#include "main.h"
#include "iflash.h"      /* журнал событий внутренней Flash (счётчики рестартов) */

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
    cmdStartTest                                /* 0x23  CMD_START_TEST — «Тест» без сна (04.07.2026) */
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
  RTC_GetTimeDate(&r->rot.stopTimeStamp);
  uint32_t durS = RTC_SubTimeDateSec(&r->rot.stopTimeStamp,
                                     &r->rot.startTimeStamp);
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
    return;
  }
  r->totalSec += durS;
  HandleSensorData(r);
  PushCycleRecord(r);
  SaveParamOnEEPROM(r);
  r->state = REG_STATE_SLEEP;
  LSM6DSO_GYRO_Disable(r->lsm);
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
      RTC_GetTimeDate(&r->rot.startTimeStamp);
      r->rot.maxRate = r->rot.maxVibr = 0;
      r->noDetectCount = 0;
      HandleSensorData(r);
      r->state = REG_STATE_ROTATING;
    }
    else
    {
      r->confirmPolls++;
      if (r->confirmPolls >= CONFIRM_WINDOW_POLLS)
      {
        /* Окно истекло без подтверждения — ложное срабатывание
         * (удар/тряска/наклон, не вращение). Гасим гироскоп. */
        LSM6DSO_GYRO_Disable(r->lsm);
        r->state = REG_STATE_SLEEP;
      }
    }
    break;

  case REG_STATE_ROTATING:
    Poll_Sensor(r);
    if (RotationDetected(r))
    {
      r->noDetectCount = 0;
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
    ComPoll();

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
 *   [14..15] max_rpm     : uint16  — макс. скорость, об/мин
 */
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
  uint16_t max_rpm    = (uint16_t)r->rot.maxRate;

  uint8_t payload[16];
  memcpy(payload,      &r->rot.startTimeStamp, sizeof(RTC_DateTime)); /* 6 */
  memcpy(payload + 6,  &duration_s,            4);
  memcpy(payload + 10, &r->totalSec,           4);
  memcpy(payload + 14, &max_rpm,               2);

  sendPacket(LTP_DEV_ADDRESS, 0x20, payload, sizeof(payload));
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

void cmdWhoAmI(LtpPacket *wc)
{
  uint8_t id = 0;
  if(0 == LSM6DSO_ReadID(&lsm, &id))
    sendPacket(wc->addr, wc->cmd, &id, 1);
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
  }

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

void cmdSetDateTime(LtpPacket *wc)
{
  uint8_t err = er_badarg;
  if(wc->n == sizeof(RTC_DateTime))
  {
    RTC_DateTime *dt = (RTC_DateTime*)wc->data;
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

  val32 = 0xFFFFFFFFu;                        /* ts_activation — не активировано */
  memcpy(&ans[13], &val32, 4);

  /* Реальные счётчики рестартов из журнала внутренней Flash (06.07.2026,
   * раньше были жёсткие нули). Переживают потерю питания (iflash, Page A). */
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

  sendPacket(wc->addr, wc->cmd, &f, 1);
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
