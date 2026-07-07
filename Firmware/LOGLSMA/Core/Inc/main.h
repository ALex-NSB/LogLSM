/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* Текущий уровень WKUP1/PA0 (1 = внешнее питание/кабель присутствует).
 * 02.07.2026: раньше static в main.c — потерян доступ извне при переносе
 * общей логики (RotationStateStep(), PushCycleRecord()) в com.c, поэтому
 * вынесено сюда и сделано доступным для App/. */
uint8_t wkup1_pin_set(void);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define BKEY_Pin GPIO_PIN_5
#define BKEY_GPIO_Port GPIOA
#define QSPI3_Pin GPIO_PIN_6
#define QSPI3_GPIO_Port GPIOA
#define QSPI2_Pin GPIO_PIN_7
#define QSPI2_GPIO_Port GPIOA
#define QSPI1_Pin GPIO_PIN_0
#define QSPI1_GPIO_Port GPIOB
#define QSPI0_Pin GPIO_PIN_1
#define QSPI0_GPIO_Port GPIOB
#define QSPICLK_Pin GPIO_PIN_10
#define QSPICLK_GPIO_Port GPIOB
#define QSPICS_Pin GPIO_PIN_11
#define QSPICS_GPIO_Port GPIOB
#define BLINK_Pin GPIO_PIN_12
#define BLINK_GPIO_Port GPIOB
#define FLASH_22_Pin GPIO_PIN_15
#define FLASH_22_GPIO_Port GPIOB
#define FLASH_0_Pin GPIO_PIN_8
#define FLASH_0_GPIO_Port GPIOA
#define SPI1_CS_Pin GPIO_PIN_15
#define SPI1_CS_GPIO_Port GPIOA
#define TPWR_Pin GPIO_PIN_4
#define TPWR_GPIO_Port GPIOB
#define FPWR_Pin GPIO_PIN_5
#define FPWR_GPIO_Port GPIOB
#define nBPWR_Pin GPIO_PIN_6
#define nBPWR_GPIO_Port GPIOB
#define BRES_Pin GPIO_PIN_7
#define BRES_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* WKUP1 = PA0 (в Pinout сконфигурирован как SYS_WKUP1, системный pin
 * пробуждения PWR, не обычный GPIO_Input с User Label). Для SYS_WKUPx
 * CubeMX НЕ генерирует пару _Pin/_GPIO_Port выше по этому файлу (в отличие
 * от обычных GPIO вроде BKEY/QSPI-пинов). Два места в App/Core
 * (main.c::wkup1_pin_set(), com.c — код проверки выхода из Service(),
 * сейчас внутри if(0)) читают
 * ТЕКУЩИЙ уровень пина через HAL_GPIO_ReadPin(WKUP1_GPIO_Port, WKUP1_Pin) —
 * это не то же самое, что __HAL_PWR_GET_FLAG(PWR_FLAG_WUF1) (защёлкнутый
 * флаг ФРОНТА, не текущий уровень). Обнаружено 02.07.2026: сборка не
 * проходила (undeclared WKUP1_GPIO_Port/WKUP1_Pin) в com.c — макросы,
 * судя по всему, никогда не генерировались для этого пина, а не
 * потерялись при недавней регенерации под WKUP2/PC13. Добавлено вручную,
 * в USER CODE секции (переживёт регенерацию CubeMX). */
#define WKUP1_Pin        GPIO_PIN_0
#define WKUP1_GPIO_Port  GPIOA

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
