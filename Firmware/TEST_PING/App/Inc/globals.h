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

#endif /* GLOBALS_H */
