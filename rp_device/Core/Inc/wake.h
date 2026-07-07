#ifndef __wake_h
#define __wake_h

#include "stdint.h"

//#define WAKE_CRC_USE

#define WAKE_RX_BUFFER_SIZE     1024

typedef struct
{
  uint8_t addr;
  uint8_t cmd;
  uint16_t n;
  uint8_t data[WAKE_RX_BUFFER_SIZE];
  uint32_t data_index;
  uint8_t state;
  uint8_t stuffing;
  uint8_t packet_recognized;
  uint8_t crc;
} WakeContext;

void WakeProtocolInit(WakeContext *wc);
uint16_t WakeProtocolBuildPacket(uint8_t addr, uint8_t cmd, uint8_t *data, uint32_t n, uint8_t *txbuf);
void WakeProtocolParse(WakeContext *wc, uint8_t b);
uint32_t WakeStuffing(uint8_t *src, uint8_t *dst, uint32_t size);
uint8_t WakeCRC(uint8_t *data, uint16_t n);

#endif //__wake_h
