/**
 * ltp.c — LogLSM Transport Protocol (LTP) v1.0
 * FSM-парсер + сборщик пакетов. Без зависимостей от HAL — чистый C11.
 * Тест-вектор CRC: "123456789" -> 0x29B1.
 * Копия из Firmware/LOGLSMA/App/Src/ltp.c — без изменений.
 */
#include "ltp.h"
#include <string.h>

/* ───────────────────────── CRC16-CCITT ─────────────────────────
 * poly=0x1021, init=0xFFFF, no reflect, xor=0 (спека §7) */
uint16_t ltp_crc16(const uint8_t *buf, size_t len)
{
  uint16_t crc = 0xFFFF;
  for(size_t i = 0; i < len; i++)
  {
    crc ^= (uint16_t)buf[i] << 8;
    for(int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ───────────────────────── Сборка пакета ───────────────────────── */

static int put_stuffed(uint8_t *out, size_t out_size, size_t *pos, uint8_t b)
{
  if(b == LTP_FEND || b == LTP_FESC)
  {
    if(*pos + 2 > out_size) return -1;
    out[(*pos)++] = LTP_FESC;
    out[(*pos)++] = (b == LTP_FEND) ? LTP_TFEND : LTP_TFESC;
  }
  else
  {
    if(*pos + 1 > out_size) return -1;
    out[(*pos)++] = b;
  }
  return 0;
}

int ltp_build(uint8_t *out, size_t out_size,
              uint8_t addr, uint8_t cmd, uint8_t flags, uint16_t seq,
              const uint8_t *payload, uint16_t plen)
{
  if(out == NULL || out_size < 1)        return -1;
  if(plen > 0 && payload == NULL)        return -1;
  if(plen > LTP_MAX_PAYLOAD)             return -1;

  uint8_t hdr[LTP_HDR_SIZE] = {
    addr, cmd, flags,
    (uint8_t)(seq  & 0xFF), (uint8_t)(seq  >> 8),   /* SEQ little-endian */
    (uint8_t)(plen & 0xFF), (uint8_t)(plen >> 8)    /* LEN little-endian */
  };

  /* CRC по сырым байтам ADDR..PAYLOAD */
  uint16_t crc = 0xFFFF;
  {
    /* инкрементально, чтобы не клеить header+payload в один буфер */
    for(size_t i = 0; i < LTP_HDR_SIZE; i++)
    {
      crc ^= (uint16_t)hdr[i] << 8;
      for(int b = 0; b < 8; b++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    for(size_t i = 0; i < plen; i++)
    {
      crc ^= (uint16_t)payload[i] << 8;
      for(int b = 0; b < 8; b++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }

  size_t pos = 0;
  out[pos++] = LTP_FEND;                  /* стартовый FEND не экранируется */

  for(size_t i = 0; i < LTP_HDR_SIZE; i++)
    if(put_stuffed(out, out_size, &pos, hdr[i]) < 0) return -1;

  for(size_t i = 0; i < plen; i++)
    if(put_stuffed(out, out_size, &pos, payload[i]) < 0) return -1;

  /* CRC big-endian: старший байт первым */
  if(put_stuffed(out, out_size, &pos, (uint8_t)(crc >> 8))   < 0) return -1;
  if(put_stuffed(out, out_size, &pos, (uint8_t)(crc & 0xFF)) < 0) return -1;

  return (int)pos;
}

/* ───────────────────────── FSM-парсер ───────────────────────── */

void ltp_parser_init(LtpParser *p)
{
  memset(p, 0, sizeof(*p));
  p->state = LTP_WAIT_FEND;
}

static void parser_resync(LtpParser *p)
{
  p->state    = LTP_WAIT_FEND;
  p->stuffing = 0;
  p->idx      = 0;
}

void ltp_parser_feed(LtpParser *p, uint8_t byte)
{
  /* FEND — безусловная точка синхронизации: всегда начинает новый пакет */
  if(byte == LTP_FEND)
  {
    p->state    = LTP_READ_HEADER;
    p->stuffing = 0;
    p->idx      = 0;
    return;
  }

  if(p->state == LTP_WAIT_FEND)
    return;

  /* Destuffing до FSM */
  if(p->stuffing)
  {
    p->stuffing = 0;
    if(byte == LTP_TFEND)      byte = LTP_FEND;
    else if(byte == LTP_TFESC) byte = LTP_FESC;
    else { parser_resync(p); return; }   /* некорректный escape */
  }
  else if(byte == LTP_FESC)
  {
    p->stuffing = 1;
    return;
  }

  switch(p->state)
  {
  case LTP_READ_HEADER:
    p->header[p->idx++] = byte;
    if(p->idx == LTP_HDR_SIZE)
    {
      p->pkt.addr  = p->header[0];
      p->pkt.cmd   = p->header[1];
      p->pkt.flags = p->header[2];
      p->pkt.seq   = (uint16_t)(p->header[3] | (p->header[4] << 8));  /* LE */
      p->pkt.n     = (uint16_t)(p->header[5] | (p->header[6] << 8));  /* LE */

      if(p->pkt.n > LTP_MAX_PAYLOAD) { parser_resync(p); return; }

      p->idx   = 0;
      p->state = (p->pkt.n > 0) ? LTP_READ_PAYLOAD : LTP_READ_CRC;
    }
    break;

  case LTP_READ_PAYLOAD:
    p->pkt.data[p->idx++] = byte;
    if(p->idx == p->pkt.n)
    {
      p->idx   = 0;
      p->state = LTP_READ_CRC;
    }
    break;

  case LTP_READ_CRC:
    p->crc_buf[p->idx++] = byte;
    if(p->idx == LTP_CRC_SIZE)
    {
      uint16_t rx_crc = (uint16_t)((p->crc_buf[0] << 8) | p->crc_buf[1]); /* BE */

      uint16_t crc = ltp_crc16(p->header, LTP_HDR_SIZE);
      /* продолжить CRC по payload */
      for(size_t i = 0; i < p->pkt.n; i++)
      {
        crc ^= (uint16_t)p->pkt.data[i] << 8;
        for(int b = 0; b < 8; b++)
          crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
      }

      if(crc == rx_crc)
        p->packet_recognized = 1;
      else
        p->crc_errors++;

      parser_resync(p);
    }
    break;

  default:
    parser_resync(p);
    break;
  }
}
