/**
 * @file    log_ringbuf.c
 * @brief   Ring buffer of IMU samples for deferred UART transmission.
 */
#include "log_ringbuf.h"
#include <stdio.h>
#include <string.h>

void LogRingBuf_Init(LogRingBuf_t *rb)
{
  rb->head = 0U;
  rb->tail = 0U;
  rb->count = 0U;
}

bool LogRingBuf_IsEmpty(const LogRingBuf_t *rb)
{
  return (rb->count == 0U);
}

bool LogRingBuf_IsFull(const LogRingBuf_t *rb)
{
  return (rb->count >= LOG_RINGBUF_CAPACITY);
}

uint16_t LogRingBuf_Count(const LogRingBuf_t *rb)
{
  return rb->count;
}

bool LogRingBuf_Push(LogRingBuf_t *rb, const LogImuRecord_t *record)
{
  if (LogRingBuf_IsFull(rb))
  {
    return false;
  }

  rb->records[rb->head] = *record;
  rb->head = (uint16_t)((rb->head + 1U) % LOG_RINGBUF_CAPACITY);
  rb->count++;

  return true;
}

bool LogRingBuf_Pop(LogRingBuf_t *rb, LogImuRecord_t *record)
{
  if (LogRingBuf_IsEmpty(rb))
  {
    return false;
  }

  *record = rb->records[rb->tail];
  rb->tail = (uint16_t)((rb->tail + 1U) % LOG_RINGBUF_CAPACITY);
  rb->count--;

  return true;
}

bool LogRingBuf_PushSample(LogRingBuf_t *rb,
                           const LSM6DSV_ImuData_t *sample,
                           uint32_t timestamp_ms)
{
  LogImuRecord_t record;

  record.timestamp_ms = timestamp_ms;
  record.sample = *sample;

  return LogRingBuf_Push(rb, &record);
}

uint16_t LogRingBuf_TrySendUart(LogRingBuf_t *rb, UART_HandleTypeDef *huart)
{
  char line[160];
  uint16_t sent = 0U;

  while (!LogRingBuf_IsEmpty(rb))
  {
    const LogImuRecord_t *record = &rb->records[rb->tail];
    const LSM6DSV_ImuData_t *s = &record->sample;
    int len = snprintf(line, sizeof(line),
                       "%lu,%ld,%ld,%ld,%ld,%ld,%ld\r\n",
                       (unsigned long)record->timestamp_ms,
                       (long)(s->accel_g.x * 1000.0f),
                       (long)(s->accel_g.y * 1000.0f),
                       (long)(s->accel_g.z * 1000.0f),
                       (long)(s->gyro_dps.x * 1000.0f),
                       (long)(s->gyro_dps.y * 1000.0f),
                       (long)(s->gyro_dps.z * 1000.0f));

    if ((len <= 0) || ((size_t)len >= sizeof(line)))
    {
      rb->tail = (uint16_t)((rb->tail + 1U) % LOG_RINGBUF_CAPACITY);
      rb->count--;
      continue;
    }

    if (HAL_UART_Transmit(huart, (uint8_t *)line, (uint16_t)len, 50U) != HAL_OK)
    {
      break;
    }

    rb->tail = (uint16_t)((rb->tail + 1U) % LOG_RINGBUF_CAPACITY);
    rb->count--;
    sent++;
  }

  return sent;
}
