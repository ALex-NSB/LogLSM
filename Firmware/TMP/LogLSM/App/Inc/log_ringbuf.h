/**
 * @file    log_ringbuf.h
 * @brief   Ring buffer of IMU samples for deferred UART transmission.
 */
#ifndef LOG_RINGBUF_H
#define LOG_RINGBUF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "stm32l4xx_hal.h"
#include "lsm6dsv_data.h"

#ifndef LOG_RINGBUF_CAPACITY
#define LOG_RINGBUF_CAPACITY  (64U)
#endif

typedef struct {
  uint32_t timestamp_ms;
  LSM6DSV_ImuData_t sample;
} LogImuRecord_t;

typedef struct {
  LogImuRecord_t records[LOG_RINGBUF_CAPACITY];
  volatile uint16_t head;
  volatile uint16_t tail;
  volatile uint16_t count;
} LogRingBuf_t;

void LogRingBuf_Init(LogRingBuf_t *rb);
bool LogRingBuf_Push(LogRingBuf_t *rb, const LogImuRecord_t *record);
bool LogRingBuf_Pop(LogRingBuf_t *rb, LogImuRecord_t *record);
bool LogRingBuf_IsEmpty(const LogRingBuf_t *rb);
bool LogRingBuf_IsFull(const LogRingBuf_t *rb);
uint16_t LogRingBuf_Count(const LogRingBuf_t *rb);

bool LogRingBuf_PushSample(LogRingBuf_t *rb,
                           const LSM6DSV_ImuData_t *sample,
                           uint32_t timestamp_ms);

uint16_t LogRingBuf_TrySendUart(LogRingBuf_t *rb, UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* LOG_RINGBUF_H */
