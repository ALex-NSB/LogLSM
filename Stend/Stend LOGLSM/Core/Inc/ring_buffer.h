#ifndef __RINGBUFFER_H
#define __RINGBUFFER_H

#include "stm32l4xx_hal.h"

#define RING_BUFFER_SIZE 1024

typedef struct
{
  uint8_t rb[RING_BUFFER_SIZE];
  uint32_t readCount;
  uint32_t writeCount;
  uint32_t mask;
} RingBuffer;

void RingInit(RingBuffer *rb);
uint8_t RingIsEmpty(RingBuffer *rb);
uint8_t RingIsFull(RingBuffer *rb);
void RingClear(RingBuffer *rb);
uint32_t RingCount(RingBuffer *rb);
uint8_t RingPut(RingBuffer *rb, uint8_t ch);
uint8_t RingRead(RingBuffer *rb, uint8_t *ch);
uint32_t RingReadN(RingBuffer *rb, uint32_t n, uint8_t *dst);

#endif // __RINGBUFFER_H
