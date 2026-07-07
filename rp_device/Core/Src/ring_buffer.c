#include "ring_buffer.h"

void RingInit(RingBuffer *rb)
{
  rb->readCount = 0;
  rb->writeCount = 0;
  rb->mask = RING_BUFFER_SIZE - 1;
}

uint8_t RingIsEmpty(RingBuffer *rb)
{
  return rb->writeCount == rb->readCount;
}

uint8_t RingIsFull(RingBuffer *rb)
{
  return ((rb->writeCount - rb->readCount) & ~(rb->mask)) != 0;
}

void RingClear(RingBuffer *rb)
{
  rb->readCount = rb->writeCount = 0;
}

uint32_t RingCount(RingBuffer *rb)
{
  return (rb->writeCount - rb->readCount);// & mask;
}

uint8_t RingPut(RingBuffer *rb, uint8_t ch)
{
  if(RingIsFull(rb))
    return 0;
  rb->rb[rb->writeCount++ & rb->mask] = ch;
  return 1;
}

uint8_t RingRead(RingBuffer *rb, uint8_t *ch)
{
  if(RingIsEmpty(rb))
    return 0;
  *ch = rb->rb[rb->readCount++ & rb->mask];
  return 1;
}

uint32_t RingReadN(RingBuffer *rb, uint32_t n, uint8_t *dst)
{
  uint16_t c = 0;
  while(!RingIsEmpty(rb) && (c <= n))
  {
    *dst = rb->rb[rb->readCount++ & rb->mask];
    dst++;
    c++;
  }
  return c;
}
