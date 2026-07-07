/**
 * ltp.h — LogLSM Transport Protocol (LTP) v1.0
 *
 * Формат пакета (до stuffing):
 *   FEND | ADDR | CMD | FLAGS | SEQ[0] | SEQ[1] | LEN[0] | LEN[1] | PAYLOAD | CRC[0] | CRC[1]
 *
 *   SEQ, LEN — little-endian; CRC16 — big-endian (CRC[0] = старший байт).
 *   CRC16-CCITT: poly=0x1021, init=0xFFFF, no reflect, xor=0.
 *   CRC покрывает ADDR..PAYLOAD (сырые байты, до stuffing).
 *   Stuffing: 0xC0 -> DB DC, 0xDB -> DB DD (всё после стартового FEND, включая CRC).
 *
 * Спека: Doc/LogLSM Transport Protocol/LTP_PROTOCOL_v1.0_RU.docx
 * Копия из Firmware/LOGLSMA/App/Inc/ltp.h — без изменений, без зависимостей от HAL.
 */
#ifndef __LTP_H
#define __LTP_H

#include <stdint.h>
#include <stddef.h>

/* Framing (SLIP-совместимо, унаследовано от WAKE) */
#define LTP_FEND          0xC0
#define LTP_FESC          0xDB
#define LTP_TFEND         0xDC
#define LTP_TFESC         0xDD

/* FLAGS (спека §5) */
#define LTP_FLAG_DIR      0x01  /* 0 = запрос хоста, 1 = ответ устройства */
#define LTP_FLAG_ERR      0x02  /* 1 = PAYLOAD содержит 1 байт кода ошибки */

/* Коды ошибок (спека §9.2) */
#define LTP_ERR_OK            0x00
#define LTP_ERR_UNKNOWN_CMD   0x01
#define LTP_ERR_BAD_LEN       0x02
#define LTP_ERR_CRC           0x03
#define LTP_ERR_BUSY          0x04
#define LTP_ERR_FLASH         0x05
#define LTP_ERR_IMU           0x06
#define LTP_ERR_NO_SERVICE    0x07

#define LTP_HDR_SIZE      7     /* ADDR+CMD+FLAGS+SEQ[2]+LEN[2] (после FEND) */
#define LTP_CRC_SIZE      2
#define LTP_MAX_PAYLOAD   512   /* запас: страница Flash 256 + служебные поля */

/* Worst-case размер закодированного пакета для буфера TX */
#define LTP_MAX_FRAME(plen)  (1u + 2u*(LTP_HDR_SIZE + (plen) + LTP_CRC_SIZE))

/* Разобранный пакет. Имена addr/cmd/n/data совместимы с бывшим WakeContext */
typedef struct
{
  uint8_t  addr;
  uint8_t  cmd;
  uint8_t  flags;
  uint16_t seq;
  uint16_t n;                       /* длина PAYLOAD */
  uint8_t  data[LTP_MAX_PAYLOAD];   /* PAYLOAD */
} LtpPacket;

typedef enum
{
  LTP_WAIT_FEND = 0,
  LTP_READ_HEADER,
  LTP_READ_PAYLOAD,
  LTP_READ_CRC
} LtpFsmState;

typedef struct
{
  LtpFsmState state;
  uint8_t   stuffing;               /* принят FESC, ждём TFEND/TFESC */
  uint8_t   header[LTP_HDR_SIZE];
  uint16_t  idx;                    /* индекс внутри текущего поля */
  uint8_t   crc_buf[LTP_CRC_SIZE];
  LtpPacket pkt;
  uint8_t   packet_recognized;      /* 1 = pkt валиден (CRC OK); сбросить после обработки */
  uint32_t  crc_errors;             /* статистика для диагностики */
} LtpParser;

void     ltp_parser_init(LtpParser *p);
void     ltp_parser_feed(LtpParser *p, uint8_t byte);
uint16_t ltp_crc16(const uint8_t *buf, size_t len);

/**
 * Собрать пакет в out (со стартовым FEND и stuffing).
 * Возврат: длина кадра в байтах, либо -1 если не влезает в out_size.
 */
int ltp_build(uint8_t *out, size_t out_size,
              uint8_t addr, uint8_t cmd, uint8_t flags, uint16_t seq,
              const uint8_t *payload, uint16_t plen);

#endif /* __LTP_H */
