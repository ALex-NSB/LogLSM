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

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define INT1_Pin GPIO_PIN_13
#define INT1_GPIO_Port GPIOC
#define WKUP1_Pin GPIO_PIN_0
#define WKUP1_GPIO_Port GPIOA
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

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
