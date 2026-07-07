#ifndef __ble_h
#define __ble_h

#include "main.h"

#define KEY_AT_DELAY                    2000
#define KEY_RESTORE_DELAY               14000 //20000
#define RESET_DELAY                     1000
#define BLE_BAND_DEFAULT                115200

void BLE_PushKey(uint32_t ms);
void BLE_Reset(void);
uint8_t BLE_AT(char *AT);
uint8_t BLE_Init(void);

#endif //__ble_h