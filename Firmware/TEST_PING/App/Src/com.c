#include "com.h"
#include "ltp.h"
#include "pwr.h"
#include "globals.h"
#include "main.h"

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

/* ── Debug-режим (TEST_PING / Debug-проект) ──────────────────────────────────
 * При DEBUG_COM Service() печатает маркеры через HAL_UART_Transmit (блокирующий TX,
 * не конфликтует с DMA RX) и посылает heartbeat каждые DBG_HB_MS мс.
 * Чтобы отключить в production-прошивке — закомментировать строку ниже.
 * ─────────────────────────────────────────────────────────────────────────*/
#define DEBUG_COM
#define DBG_HB_MS  5000u   /* интервал heartbeat, мс */

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
    cmdFlashSectorEraseHandler                  /* 0x1F  стирание сектора 4 КБ */
};
  
static LtpParser ltp_rx;          /* FSM-парсер входного потока */
static uint16_t  cur_seq;         /* SEQ текущего запроса — копируется в ответ */
static uint8_t TxBuf[1024];
static uint8_t RxBuf[512];
static uint32_t          prevFill;
/* dma_restart_needed:
 *   1 = нормальный рестарт после IDLE (DMA ещё бежит в CIRCULAR, HAL_BUSY — норма)
 *   2 = рестарт после ошибки — нужен HAL_UART_AbortReceive перед рестартом,
 *       иначе HAL_UARTEx_ReceiveToIdle_DMA вернёт HAL_BUSY:
 *       после ошибки HAL_UART_DMAAbortOnError не сбрасывает RxState→READY,
 *       и HAL считает приём "ещё идущим", хотя DMA уже остановлен. */
static volatile uint8_t  dma_restart_needed = 0;
/* Диагностические счётчики (выводятся в heartbeat): */
volatile uint32_t g_uart_error_cnt = 0; /* UART errors (FE/ORE/NE) — пишется из ISR */
volatile uint32_t g_ping_rx_cnt    = 0; /* кол-во обработанных PING */
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
//extern UART_HandleTypeDef huart2;

/* ── Вспомогательная функция диагностической печати ───────────────────────*/
#ifdef DEBUG_COM
static void dbg_print(const char *s)
{
  /* Блокирующий TX, timeout 500 мс. Безопасен параллельно с DMA RX. */
  HAL_UART_Transmit(&huart2, (const uint8_t *)s, (uint16_t)strlen(s), 500);
}
#else
#define dbg_print(s)  ((void)0)
#endif

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
  //HAL_UART_Receive_IT(&huart2, RxBuf, sizeof(RxBuf));
}

void ComDeInit()
{
  HAL_UART_DeInit(&huart2);
}

void Service()
{
  ComInit();

  /* ── DEBUG_COM: маркер «UART работает» сразу после ComInit ─────────────
   * Если эта строка видна в терминале (115200, 8N1) — USART2 TX исправен.
   * Если НЕ видна — проблема до этой точки (ComInit / тактирование / пин). */
  dbg_print("[DBG] UART_OK - ComInit done\r\n");

  /* ── DEBUG_COM: TMP117 и FRAM намеренно пропущены ──────────────────────
   * В продуктовой прошивке (LOGLSMA/com.c) здесь вызываются tmp117Activ()
   * и framActiv(), блокируя ответ на PING на 10–50 мс (I2C + SPI init).
   * В Debug-проекте периферия не нужна — пропускаем, чтобы PING отвечал
   * мгновенно после ComInit и исключить I2C/SPI как источник зависания.
   *
   * Чтобы включить инит периферии обратно (для отладки tmp117/FRAM):
   *   1. Раскомментировать блок ниже
   *   2. Проверить, что маркеры TMP117_OK и FRAM_OK появляются в терминале
   *      (если маркер не появляется — зависание именно в том init'е)
   *
   * #ifndef DEBUG_COM                  ← в production: всегда включено
   *   tmp117Activ();
   *   dbg_print("[DBG] TMP117_OK\r\n");
   *   framActiv();
   *   dbg_print("[DBG] FRAM_OK\r\n");
   * #endif
   */

  dbg_print("[DBG] entering loop (TMP117/FRAM skipped)\r\n");

#ifdef DEBUG_COM
  uint32_t last_hb = 0;  /* время последнего heartbeat */
#endif

  while(!QUIT)
  {
    ComPoll();

#ifdef DEBUG_COM
    /* Heartbeat каждые DBG_HB_MS мс — подтверждает, что цикл живой.
     * Также выводит счётчик CRC-ошибок парсера для диагностики. */
    uint32_t now = HAL_GetTick();
    if (now - last_hb >= DBG_HB_MS)
    {
      last_hb = now;
      char hb[80];
      /* g_uart_error_cnt — UART FE/ORE/NE с момента старта (или последнего heartbeat).
       * g_ping_rx_cnt    — число обработанных PING (каждый PING от PortScanner → +1).
       * Если ping_rx растёт, а LOGLSMW всё равно не находит устройство — проблема
       * на стороне ПК (PortScanner не видит ACK). Если не растёт — DMA не работает. */
      int n = snprintf(hb, sizeof(hb),
                       "[DBG] ALIVE t=%lu err=%lu ping=%lu crc=%lu pf=%lu\r\n",
                       (unsigned long)now,
                       (unsigned long)g_uart_error_cnt,
                       (unsigned long)g_ping_rx_cnt,
                       (unsigned long)ltp_rx.crc_errors,
                       (unsigned long)prevFill);
      if (n > 0)
        HAL_UART_Transmit(&huart2, (uint8_t *)hb, (uint16_t)n, 500);
    }
#endif
  }
}

void ComPoll()
{
  /* Рестарт DMA RX после IDLE — вне ISR-контекста.
   * ВАЖНО: prevFill обнуляется ТОЛЬКО если HAL_UARTEx_ReceiveToIdle_DMA
   * вернул HAL_OK (т.е. DMA реально перезапущен с адреса 0 буфера).
   * При DMA_CIRCULAR HAL может вернуть HAL_BUSY (DMA уже бежит) —
   * тогда prevFill НЕ трогаем, иначе при следующем IDLE-событии
   * парсер обработает старые байты [0..fill] повторно → CRC-ошибки. */
  if(dma_restart_needed)
  {
    dma_restart_needed = 0;
    /* После IDLE-события в CIRCULAR-режиме DMA уже бежит → HAL_BUSY норма,
     * prevFill не сбрасываем (DMA продолжает с текущей позиции).
     * После ошибки DMA перезапускается прямо в ErrorCallback (см. ниже) —
     * сюда доходит уже с работающим DMA, тоже ожидаем HAL_BUSY. */
    if(HAL_OK == HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf)))
      prevFill = 0;
  }

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
  g_ping_rx_cnt++;
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

void cmdStartRegister(LtpPacket *wc)
{
  uint8_t err = er_none;
  sendPacket(wc->addr, wc->cmd, &err, sizeof(err));  // ACK до выхода из сервиса
  QUIT = 1;
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
 * payload (23 байта): cod(1) | uptime_total u32 | ts_first u32 | ts_last u32
 *                   | ts_activation u32 | restarts_timer u16 | restarts_power u16
 *                   | mode u8 | stats_ver u8
 * Stub: uptime из HAL_GetTick(), остальное — заглушки (0xFF/0x00).
 * Полная реализация — чтение заголовка Flash/FRAM (этап 3 прошивки). */
void cmdGetStats(LtpPacket *wc)
{
  uint8_t ans[23];
  uint32_t val32;
  uint16_t val16;

  ans[0] = er_none;

  val32 = HAL_GetTick() / 1000u;             /* uptime_total — сек с момента старта */
  memcpy(&ans[1], &val32, 4);

  val32 = 0xFFFFFFFFu;                        /* ts_first — нет данных */
  memcpy(&ans[5], &val32, 4);

  val32 = 0xFFFFFFFFu;                        /* ts_last  — нет данных */
  memcpy(&ans[9], &val32, 4);

  val32 = 0xFFFFFFFFu;                        /* ts_activation — не активировано */
  memcpy(&ans[13], &val32, 4);

  val16 = 0;                                  /* restarts_timer */
  memcpy(&ans[17], &val16, 2);

  val16 = 0;                                  /* restarts_power */
  memcpy(&ans[19], &val16, 2);

  ans[21] = 0;                                /* mode: 0 = SERVICE */
  ans[22] = 1;                                /* stats_ver */

  sendPacket(wc->addr, wc->cmd, ans, sizeof(ans));
}

/* 02.07.2026 вечером: включаем реальную Flash обратно, но с маркерами на
 * каждом шаге — цель узнать, где именно застревает FLASH_READ (0x07),
 * который в продуктовой прошивке систематически не отвечает вообще
 * (не мусором — полным молчанием), тогда как FLASH_ON/WRITE/ERASE отвечают
 * всегда. dbg_print идёт по тому же USART2, что и LTP — смотреть сырым
 * терминалом (PuTTY/TeraTerm, 921600 8N1), не через LOGLSMW параллельно. */
static uint8_t s_test_flash_on = 0;

void cmdFlashOn(LtpPacket *wc)
{
  uint8_t f = er_none;
  if (!s_test_flash_on)
  {
    dbg_print("[DBG] FLASH_ON: flashActiv() start\r\n");
    flashActiv();
    dbg_print("[DBG] FLASH_ON: flashActiv() done (P25Qx_SetQPI returned)\r\n");
    s_test_flash_on = 1;
  }
  else
  {
    dbg_print("[DBG] FLASH_ON: already on, skip\r\n");
  }
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

void cmdFlashOff(LtpPacket *wc)
{
  /* TEST_PING stub: Flash не трогаем — нет QUADSPI init. */
  uint8_t f = er_none;
  sendPacket(wc->addr, wc->cmd, &f, 1);
}

void cmdReadMem(enum MemoryType mem, LtpPacket *wc)
{
  if(wc->n < 8)
  {
    dbg_print("[DBG] FLASH_READ: wc->n < 8, early return (bad payload)\r\n");
    return;
  }

  uint32_t addr = *(uint32_t*)(wc->data + 0);
  uint32_t size = *(uint32_t*)(wc->data + 4);
  addr &= 0x00FFFFFF;
  if(size > 256) size = 256;

  char dbg[96];
  snprintf(dbg, sizeof(dbg), "[DBG] FLASH_READ: addr=0x%06lX size=%lu flash_on=%d mode=%d\r\n",
           (unsigned long)addr, (unsigned long)size, s_test_flash_on, (int)flash.mode);
  dbg_print(dbg);

  uint8_t page[257];
  page[0] = er_none;
  memset(&page[1], 0xAA, size);   /* маркер "не тронуто" — если после Read
                                    * останется 0xAA, значит QSPI_Receive
                                    * ничего не записал в буфер */

  if (mem == m_eeprom)
  {
    dbg_print("[DBG] FLASH_READ: calling P25Qx_QPI_Read...\r\n");
    P25Qx_QPI_Read(&flash, addr, size, &page[1]);
    dbg_print("[DBG] FLASH_READ: P25Qx_QPI_Read returned\r\n");

    snprintf(dbg, sizeof(dbg),
             "[DBG] FLASH_READ: first 8 bytes = %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
             page[1], page[2], page[3], page[4], page[5], page[6], page[7], page[8]);
    dbg_print(dbg);
  }
  else
  {
    fm25xx_readMultiple(&hspi1, addr, &page[1], size);
    dbg_print("[DBG] FLASH_READ: fm25xx_readMultiple returned\r\n");
  }

  dbg_print("[DBG] FLASH_READ: calling sendPacket...\r\n");
  sendPacket(wc->addr, wc->cmd, page, 1+size);
  dbg_print("[DBG] FLASH_READ: sendPacket returned (LTP response sent)\r\n");
}

void cmdWriteMem(enum MemoryType mem, LtpPacket *wc)
{
  /* TEST_PING stub: запись игнорируем, ACK немедленно. */
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
  /* TEST_PING stub: TMP117 не инициализирован (I2C init пропущен).
   * Возвращаем фиксированную заглушку 0.0°C вместо реального I2C-запроса. */
  uint8_t ans[5];
  float temp = 0.0f;
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
  /* TEST_PING stub: QUADSPI не инициализирован — фиктивный ID. */
  uint8_t ans[4] = {er_none, 0xFF, 0xFF, 0xFF};
  sendPacket(wc->addr, wc->cmd, &ans, sizeof(ans));
}

void cmdFlashGetState(LtpPacket *wc)
{
  /* TEST_PING stub: WIP=0 (не занят), без обращения к Flash. */
  uint8_t ans[] = {er_none, 0x00};
  sendPacket(wc->addr, wc->cmd, &ans, sizeof(ans));
}

void cmdFlashChipEraseHandler(LtpPacket *wc)
{
  /* TEST_PING stub: стирание не выполняется. */
  uint8_t f = er_none;
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

  /* TEST_PING stub: стирание страницы не выполняется. */
  (void)pg_num;
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

  /* TEST_PING stub: стирание сектора не выполняется. */
  (void)addr;
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
  /* fill — текущая позиция DMA (число принятых байт от начала буфера).
   * prevFill — позиция при прошлом IDLE-событии.
   *
   * Нормальный случай: prevFill < fill → читаем [prevFill .. fill).
   * Переполнение кольца: DMA дошёл до конца буфера и начал с 0 снова,
   * fill < prevFill → читаем [prevFill .. sizeof(RxBuf)) ++ [0 .. fill).
   * После HAL_UART_ErrorCallback: prevFill сброшен в 0, fill > 0 →
   * попадаем в нормальный случай, лишние байты из 0 не читаем. */
  if(prevFill < fill)
    ReadReceiveBuffer(huart, prevFill, fill - prevFill);
  else if(fill < prevFill)
  {
    /* Настоящее переполнение кольцевого буфера */
    ReadReceiveBuffer(huart, prevFill, sizeof(RxBuf) - prevFill);
    ReadReceiveBuffer(huart, 0, fill);
  }
  /* fill == prevFill: нет новых байт (IDLE без данных), ничего не делаем */
  prevFill = fill;
  dma_restart_needed = 1;  /* рестарт DMA — в main loop, не из ISR */
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if(huart != &huart2) return;
  g_uart_error_cnt++;
  prevFill = 0;              /* DMA при рестарте начнёт с позиции 0 */
  ltp_parser_init(&ltp_rx);  /* сброс парсера — он мог быть в середине пакета */
  /* Сбрасываем RxState в READY вручную — HAL_UART_DMAAbortOnError не всегда делает это.
   * Затем перезапускаем DMA прямо из ISR (как в stend/com_interr.c), без флагов и
   * AbortReceive: стенд с этим же подходом восстанавливается за 0.5с, тогда как
   * вариант "флаг + ComPoll + AbortReceive" давал 15-20с задержки. */
  huart2.RxState = HAL_UART_STATE_READY;
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf));
}

void ReadReceiveBuffer(UART_HandleTypeDef *huart, uint32_t start, uint32_t size)
{
  for(uint32_t i=0; i<size; i++)
    ltp_parser_feed(&ltp_rx, RxBuf[start+i]);
}
////////////////////////////////////////////////////////////////////////////////

/* Ответ устройства: FLAGS.DIR=1, SEQ копируется из текущего запроса */
void sendPacket(uint8_t addr, uint8_t cmd, void *data, uint32_t n)
{
  int size = ltp_build(TxBuf, sizeof(TxBuf),
                       addr, cmd, LTP_FLAG_DIR, cur_seq,
                       (const uint8_t*)data, (uint16_t)n);
  if(size > 0)
    HAL_UART_Transmit(&huart2, TxBuf, (uint16_t)size, 1000);
}

/* Ошибочный ответ: FLAGS.DIR=1 + FLAGS.ERR=1, PAYLOAD = 1 байт кода ошибки */
void sendError(uint8_t addr, uint8_t cmd, uint8_t err_code)
{
  int size = ltp_build(TxBuf, sizeof(TxBuf),
                       addr, cmd, LTP_FLAG_DIR | LTP_FLAG_ERR, cur_seq,
                       &err_code, 1);
  if(size > 0)
    HAL_UART_Transmit(&huart2, TxBuf, (uint16_t)size, 1000);
}
