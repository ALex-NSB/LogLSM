/*
 * com_interr.c — LTP-приём + relay + управление StepDriver (Nucleo-L476RG, стенд).
 *
 * Топология:
 *   USART2 ↔ ПК         (921600, DMA1 Ch6 RX, addr 0x8C — свои команды)
 *   USART3 ↔ регистратор (921600, DMA1 Ch3 RX, addr 0x8D — ретрансляция)
 *
 * Relay PC→Reg:
 *   Байты от ПК смотрим побайтово. Пакет с ADDR ≠ LTP_OWN_ADDR (0x8C)
 *   пересылаем как есть на USART3 к регистратору.
 *   Пакет с ADDR = 0x8C разбирает ltp_parser_feed, на USART3 не шлём.
 *
 * Relay Reg→PC:
 *   Все байты от USART3 пересылаем на USART2 (ПК) без разбора.
 *
 * DMA Circular mode — один запуск в ComInit(), рестарт не нужен.
 * ErrorCallback (ORE/FE при переподключении VCP): сброс RxState + ISR-рестарт.
 *
 * Команды стенда (ADDR 0x8C):
 *   0x01 CMD_PING  → ACK (пустой payload, DIR=1)
 *   0x04 CMD_START → StepDriverSetSpeed(lastSpeed, lastCoef), ACK
 *   0x05 CMD_SPEED → payload: speed u16 LE + coef u8
 *                    → StepDriverSetSpeed(speed, coef), ACK
 *   0x06 CMD_STOP  → StepDriverSetSpeed(0, lastCoef), ACK
 */

#include "com.h"
#include "ltp.h"
#include "ring_buffer.h"
#include "StepDriver.h"
#include <string.h>
#include <stdint.h>

/* ── Команды ────────────────────────────────────────────────────────────── */
#define CMD_PING   0x01
#define CMD_START  0x04
#define CMD_SPEED  0x05
#define CMD_STOP   0x06

/* ── DMA RX буферы ──────────────────────────────────────────────────────── */
static uint8_t  RxBufPc[256];   /* USART2 ← ПК          */
static uint8_t  RxBufReg[256];  /* USART3 ← регистратор  */
static uint16_t prevFillPc;
static uint16_t prevFillReg;

/* ── LTP parser (для пакетов на 0x8C) ──────────────────────────────────── */
static LtpParser lp;

/* ── TX — кольцевые буферы для обоих UART ───────────────────────────────── */
static RingBuffer txBufPc;          /* очередь байт → ПК  (USART2) */
static RingBuffer txBufReg;         /* очередь байт → рег (USART3) */
static uint8_t   txBusyPc,  txBytePc;
static uint8_t   txBusyReg, txByteReg;

/* ── Последние параметры скорости (CMD_START после CMD_STOP) ────────────── */
static uint16_t lastSpeed;
static uint8_t  lastCoef;

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart3_rx;

/* ══════════════════════════════════════════════════════════════════════════ */
/*  TX helpers                                                                */
/* ══════════════════════════════════════════════════════════════════════════ */

static void SendBytePc(uint8_t b)
{
    RingPut(&txBufPc, b);
    if (!txBusyPc) {
        txBusyPc = 1;
        RingRead(&txBufPc, &txBytePc);
        HAL_UART_Transmit_IT(&huart2, &txBytePc, 1);
    }
}

static void SendByteReg(uint8_t b)
{
    RingPut(&txBufReg, b);
    if (!txBusyReg) {
        txBusyReg = 1;
        RingRead(&txBufReg, &txByteReg);
        HAL_UART_Transmit_IT(&huart3, &txByteReg, 1);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2) {
        if (!RingIsEmpty(&txBufPc)) {
            RingRead(&txBufPc, &txBytePc);
            HAL_UART_Transmit_IT(&huart2, &txBytePc, 1);
        } else {
            txBusyPc = 0;
        }
    } else if (huart == &huart3) {
        if (!RingIsEmpty(&txBufReg)) {
            RingRead(&txBufReg, &txByteReg);
            HAL_UART_Transmit_IT(&huart3, &txByteReg, 1);
        } else {
            txBusyReg = 0;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Отправка LTP-пакета → ПК                                                 */
/* ══════════════════════════════════════════════════════════════════════════ */

static void sendPacket(uint8_t cmd, const uint8_t *payload, uint8_t plen)
{
    uint8_t frame[LTP_MAX_FRAME(8)];
    int sz = ltp_build(frame, sizeof(frame),
                       LTP_OWN_ADDR, cmd,
                       LTP_FLAG_DIR, lp.pkt.seq,
                       payload, plen);
    if (sz > 0)
        for (int i = 0; i < sz; ++i)
            SendBytePc(frame[i]);
}

static void sendAck(uint8_t cmd)
{
    sendPacket(cmd, NULL, 0);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Обработчики команд стенда                                                 */
/* ══════════════════════════════════════════════════════════════════════════ */

static void handleCmd(void)
{
    /* КРИТИЧНО (05.07.2026, ПОДТВЕРЖДЕНО): парсеру стенда идут ВСЕ байты от ПК
     * (HAL_UARTEx_RxEventCallback), включая пакеты к РЕГИСТРАТОРУ (0x8D),
     * которые лишь ретранслируются. Коды CMD_START/SPEED/STOP (0x04/0x05/0x06)
     * совпадают с флеш-командами регистратора — без проверки ADDR стенд исполнял
     * чужие пакеты как «крути мотор» (мотор вращался сам на авто-опросе флеша).
     * Диагностический откат 05.07 подтвердил: эта проверка НЕ влияет на связь с
     * регистратором (при откате регистратор молчал так же — он спал в Stop2,
     * стенд ни при чём). Проверку вернули. Исполняем ТОЛЬКО свои пакеты (0x8C). */
    if (lp.pkt.addr != LTP_OWN_ADDR)
        return;

    switch (lp.pkt.cmd) {

    case CMD_PING:
        sendAck(CMD_PING);
        break;

    case CMD_START:
        /* Запустить мотор на последней запомненной скорости */
        if (lastSpeed > 0)
            StepDriverSetSpeed(lastSpeed, lastCoef ? lastCoef : 1);
        sendAck(CMD_START);
        break;

    case CMD_SPEED:
        /* payload: speed u16 LE + coef u8 */
        if (lp.pkt.n >= 3) {
            lastSpeed = (uint16_t)((uint16_t)lp.pkt.data[0] |
                                   (uint16_t)(lp.pkt.data[1] << 8));
            lastCoef  = lp.pkt.data[2];
            StepDriverSetSpeed(lastSpeed, lastCoef ? lastCoef : 1);
        }
        sendAck(CMD_SPEED);
        break;

    case CMD_STOP:
        StepDriverSetSpeed(0, lastCoef ? lastCoef : 1);
        sendAck(CMD_STOP);
        break;

    default:
        break;  /* неизвестная команда — игнорируем */
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Relay: байты от ПК → регистратору (если ADDR ≠ LTP_OWN_ADDR)           */
/*                                                                            */
/*  Автомат по байтам: после FEND смотрим ADDR-байт.                         */
/*  ADDR = LTP_OWN_ADDR → пакет наш, ltp_parser разберёт, relay нет.        */
/*  ADDR ≠ LTP_OWN_ADDR → шлём FEND+ADDR на USART3 и relay остального.      */
/* ══════════════════════════════════════════════════════════════════════════ */

typedef enum { RS_IDLE, RS_GOT_FEND, RS_OWN, RS_RELAY } RelayState_t;
static RelayState_t rs;

static void relay_pc_byte(uint8_t b)
{
    switch (rs) {

    case RS_IDLE:
    case RS_OWN:
        if (b == LTP_FEND)
            rs = RS_GOT_FEND;
        break;

    case RS_GOT_FEND:
        if (b == LTP_FEND)
            break;  /* двойной FEND — остаёмся ждать ADDR */
        if (b == LTP_OWN_ADDR) {
            rs = RS_OWN;    /* пакет наш — не ретранслируем */
        } else {
            /* пакет для регистратора: шлём FEND + ADDR и переключаемся в relay */
            SendByteReg(LTP_FEND);
            SendByteReg(b);
            rs = RS_RELAY;
        }
        break;

    case RS_RELAY:
        SendByteReg(b);
        if (b == LTP_FEND)
            rs = RS_GOT_FEND;  /* конец пакета = начало следующего */
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  DMA RX callbacks                                                          */
/* ══════════════════════════════════════════════════════════════════════════ */

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t fill)
{
    if (huart == &huart2) {
        /* Байты от ПК: парсим свои + relay чужих на USART3 */
        uint16_t prev = prevFillPc;
        prevFillPc = fill;

        if (prev <= fill) {
            for (uint16_t i = prev; i < fill; i++) {
                ltp_parser_feed(&lp, RxBufPc[i]);
                relay_pc_byte(RxBufPc[i]);
            }
        } else {
            /* wrap-around */
            for (uint16_t i = prev; i < (uint16_t)sizeof(RxBufPc); i++) {
                ltp_parser_feed(&lp, RxBufPc[i]);
                relay_pc_byte(RxBufPc[i]);
            }
            for (uint16_t i = 0; i < fill; i++) {
                ltp_parser_feed(&lp, RxBufPc[i]);
                relay_pc_byte(RxBufPc[i]);
            }
        }

    } else if (huart == &huart3) {
        /* Байты от регистратора: ретранслируем на ПК без разбора */
        uint16_t prev = prevFillReg;
        prevFillReg = fill;

        if (prev <= fill) {
            for (uint16_t i = prev; i < fill; i++)
                SendBytePc(RxBufReg[i]);
        } else {
            for (uint16_t i = prev; i < (uint16_t)sizeof(RxBufReg); i++)
                SendBytePc(RxBufReg[i]);
            for (uint16_t i = 0; i < fill; i++)
                SendBytePc(RxBufReg[i]);
        }
    }
}

/*
 * ErrorCallback — ORE/FE/NE после переподключения VCP или кабеля регистратора.
 * HAL aborts DMA; force-сброс RxState + рестарт из ISR.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2) {
        prevFillPc = 0;
        ltp_parser_init(&lp);
        rs = RS_IDLE;
        huart2.RxState = HAL_UART_STATE_READY;
        HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBufPc, sizeof(RxBufPc));
        __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);

    } else if (huart == &huart3) {
        prevFillReg = 0;
        huart3.RxState = HAL_UART_STATE_READY;
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, RxBufReg, sizeof(RxBufReg));
        __HAL_DMA_DISABLE_IT(&hdma_usart3_rx, DMA_IT_HT);
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Инициализация и главный цикл                                              */
/* ══════════════════════════════════════════════════════════════════════════ */

void ComInit(void)
{
    ltp_parser_init(&lp);
    RingInit(&txBufPc);
    RingInit(&txBufReg);
    txBusyPc  = 0;  txBytePc  = 0;
    txBusyReg = 0;  txByteReg = 0;
    prevFillPc  = 0;
    prevFillReg = 0;
    rs = RS_IDLE;
    lastSpeed = 0;
    lastCoef  = 1;

    /* Запускаем DMA-приём на обоих UART (circular — больше не перезапускаем) */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBufPc,  sizeof(RxBufPc));
    __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);

    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, RxBufReg, sizeof(RxBufReg));
    __HAL_DMA_DISABLE_IT(&hdma_usart3_rx, DMA_IT_HT);
}

void ComPoll(void)
{
    /* DMA Circular — не перезапускаем, только разбираем готовые пакеты */
    if (lp.packet_recognized) {
        handleCmd();
        lp.packet_recognized = 0;
    }
}
