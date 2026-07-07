#ifndef __COM_H
#define __COM_H

#include "stm32l4xx_hal.h"
#include "lsm6dso.h"
#include "Data.h"

#define WAKE_DEV_ADDRESS                0x8D

#pragma pack(push,1)
struct AxesRaw
{
  uint8_t code;
  LSM6DSO_AxesRaw_t gyro;
  LSM6DSO_AxesRaw_t acc;
};
#pragma pack(pop)

#pragma pack(push,1)
struct FullScale
{
  uint8_t code;
  int32_t fullscale;
};
#pragma pack(pop)

#pragma pack(push,1)
typedef struct
{
  uint8_t cod;
  RTC_DateTime datetime;
} DateTimeResponse;
#pragma pack(pop)

void ComInit();
void ComPoll();
void Service();

//DBG
void printff(const char *format, ...);
void printHex(uint8_t *src, uint32_t size, uint32_t width);
void print(char *str);
uint32_t send(uint8_t *buff, uint32_t size);

#endif //__COM_H
