#ifndef __COM_H
#define __COM_H

#include "stm32l4xx_hal.h"
#include "lsm6dso.h"
#include "Data.h"

#define LTP_DEV_ADDRESS                 0x8D  /* адрес устройства (бит 7 = 1), экс-WAKE_DEV_ADDRESS */

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
void Service(RegistratorData *r);

/* CMD 0x20: unsolicited-пуш записи цикла после завершения вращения.
 * Вызывается изнутри RotationStateStep() (com.c) при каждом завершении
 * цикла ROTATING, безусловно — сама функция проверяет текущий уровень
 * WKUP1 и молча возвращается, если кабеля нет. Payload 16 байт — см.
 * реализацию в com.c. */
void PushCycleRecord(RegistratorData *r);

/* Общий шаг конечного автомата SLEEP->CONFIRM->ROTATING (02.07.2026).
 * Вызывается ТОЛЬКО из main.c (WORK/PASSIVE-цикл, настоящий Stop2) — вне
 * зависимости от того, подключён кабель или нет: сон один и тот же
 * всегда, это не вопрос экономии энергии. Service()/BENCH (com.c)
 * распознаванием вращения не занимается вообще, эту функцию не зовёт —
 * только обслуживает LTP-команды.
 * Сам автомат запускается ТОЛЬКО командой CMD_START_REGISTER (0x1D,
 * cmdStartRegister()) — main.c вызывает эту функцию, только если
 * r->monitoringActive=1 (иначе состояние всегда REG_STATE_SLEEP и вызов
 * не нужен). После запуска работает независимо от WKUP1 — обработка
 * фронта WKUP1 (WUF1) — это отдельный, самостоятельный вектор входа в
 * Service() (верхняя проверка в while(1), main.c), она не пересекается с
 * этим автоматом и не может его прервать. См. подробный комментарий у
 * реализации в com.c. */
uint8_t RotationStateStep(RegistratorData *r);

//DBG
void printff(const char *format, ...);
void printHex(uint8_t *src, uint32_t size, uint32_t width);
void print(char *str);
uint32_t send(uint8_t *buff, uint32_t size);

#endif //__COM_H
