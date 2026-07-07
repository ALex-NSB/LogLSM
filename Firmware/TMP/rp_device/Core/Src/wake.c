#include "wake.h"
#include "stdio.h"

//#include "stm32l4xx_hal.h"
//#include "usart.h"

#define FEND            0xC0
#define FESC            0xDB
#define TFEND           0xDC
#define TFESC           0xDD
#define INIT_CRC        0xDE

enum {
  WAIT_FEND,
  WAIT_ADDR,
  WAIT_CMD,
  WAIT_N1,
  WAIT_N2,
  WAIT_DATA,
  WAIT_CRC
};

//static uint8_t TxBuf[256];
//static uint32_t TxSize;

uint32_t WakeStuffing(uint8_t *src, uint8_t *dst, uint32_t size)
{
  uint32_t n = 0;
  for(uint32_t i=0; i<size; i++)
  {
    if(src[i] == FEND)
    {
      *dst = FESC; dst++;
      *dst = TFEND;
      n += 2;
    }
    else if(src[i] == FESC)
    {
      *dst = FESC; dst++;
      *dst = TFESC;
      n += 2;
    }
    else
    {
      *dst = src[i];
      n++;
    }
  }
  return n;
}

uint8_t doStuffing(uint8_t ch, uint8_t *dst)
{
  uint8_t n;
  if(ch == FEND)
  {
    *dst = FESC; dst++;
    *dst = TFEND;
    n = 2;
  }
  else if(ch == FESC)
  {
    *dst = FESC; dst++;
    *dst = TFESC;
    n = 2;
  }
  else
  {
    *dst = ch;
    n = 1;
  }
  return n;
}

void WakeProtocolInit(WakeContext *wc)
{
  wc->state = WAIT_FEND;
  wc->packet_recognized = 0;
}

uint8_t WakeCRC8(uint8_t crc, uint8_t dat)
{
    uint8_t g;
    for(g=0;g<8;g++,dat>>=1)
    {
        if((dat^crc)&1) crc=((crc^0x18)>>1)|0x80;
        else crc=(crc>>1)&(~0x80);
    }
    return crc;
}

uint8_t WakeCRC(uint8_t *data, uint16_t n)
{
  uint8_t crc = INIT_CRC;
  while(n--)
  {
    crc = WakeCRC8(crc, *data);
    data++;
  }
  return crc;
}

uint16_t WakeProtocolBuildPacket(uint8_t addr, uint8_t cmd, uint8_t *data, uint32_t n, uint8_t *txbuf)
{
  uint16_t i = 0;
  uint8_t crc = INIT_CRC;
  txbuf[i++] = FEND;
  if(addr & 0x80)
  {
    i += doStuffing(addr, txbuf+i);
  }
  txbuf[i++] = cmd;
  i += doStuffing((n>>8), txbuf+i);
  i += doStuffing((n & 0xFF), txbuf+i);
  
#ifdef WAKE_CRC_USE
  crc = WakeCRC8(crc, FEND);
  crc = WakeCRC8(crc, addr);
  crc = WakeCRC8(crc, cmd);
  crc = WakeCRC8(crc, n);
#endif
  while(n--)
  {
    i += doStuffing(*data, txbuf+i);
#ifdef WAKE_CRC_USE    
    crc = WakeCRC8(crc, *data);
#endif
    data++;
  }
#ifdef WAKE_CRC_USE
  i += doStuffing(crc, txbuf+i);
#endif
  return i;
}

/*void print(char *s)
{
  HAL_UART_Transmit(&huart2, (uint8_t*)s, size, 1000);
}*/

void WakeProtocolParse(WakeContext *wc, uint8_t b)
{
    if(b == FEND)
    {
        wc->crc = WakeCRC8(INIT_CRC, FEND);
        wc->stuffing = 0;
        wc->state = WAIT_ADDR;
        return;
    }
    if(wc->stuffing == 1)
    {
      wc->stuffing = 0;
      if(b == TFEND) b = FEND;
      else if(b == TFESC) b = FESC;
      else
      {
        wc->state = WAIT_FEND;
        return;
      }
    }
    else if(b == FESC)
    {
      wc->stuffing = 1;
      return;
    }
    switch(wc->state)
    {
    case WAIT_FEND:
      break;
    case WAIT_ADDR:
      if(b & 0x80)
      {
        wc->addr = b;
        wc->crc = WakeCRC8(wc->crc, b);
        wc->state = WAIT_CMD;
        break;
      }
      wc->addr = 0;
    case WAIT_CMD:
      wc->cmd = b;
      wc->crc = WakeCRC8(wc->crc, wc->cmd);
      wc->state = WAIT_N1;
      break;
    case WAIT_N1:
      wc->n = (b << 8);
      wc->crc = WakeCRC8(wc->crc, b);
      wc->state = WAIT_N2;
      break;
    case WAIT_N2:
      wc->n |= b;
      wc->crc = WakeCRC8(wc->crc, b);
      wc->data_index = 0;
      if (wc->n > WAKE_RX_BUFFER_SIZE)
        wc->state = WAIT_FEND;                  // skip the packet if its size is too large
      if(wc->n == 0)
      {
#ifdef WAKE_CRC_USE
        wc->state = WAIT_CRC;
#else
        wc->packet_recognized = 1;
        wc->state = WAIT_FEND;
#endif
      }
      else
      {
        wc->state = WAIT_DATA;
      }
      break;
    case WAIT_DATA:
      wc->data[wc->data_index] = b;
      wc->data_index++;
      wc->crc = WakeCRC8(wc->crc, b);
      if(wc->data_index == wc->n)
      {
#ifdef WAKE_CRC_USE
        wc->state = WAIT_CRC;
#else
        wc->packet_recognized = 1;
        wc->state = WAIT_FEND;
#endif
      }
      break;
    case WAIT_CRC:
      if(b == wc->crc)
      {
        wc->packet_recognized = 1;
      }
      wc->state = WAIT_FEND;
      break;
    default:
      break;
    }
}
