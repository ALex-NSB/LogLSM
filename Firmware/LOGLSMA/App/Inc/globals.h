#ifndef GLOBALS_H
#define GLOBALS_H

#include "lsm6dso.h"
#include "p25q128.h"
#include "Data.h"

/* Глобальные объекты, определённые в main.c */
extern LSM6DSO_Object_t lsm;
extern P25Qx_HandleTypeDef flash;
extern uint8_t flash_powered;
extern RegistratorData regist;
extern uint8_t ble_flag;

/* IWDG-сторож (main.c): рефреш живого кода. Зовём в Service() и долгих
 * операциях (стирание), чтобы не сработал во время легитимной работы. */
extern volatile uint8_t g_iwdg_on;
void iwdg_kick(void);

#endif /* GLOBALS_H */
