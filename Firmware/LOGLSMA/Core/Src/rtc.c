/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   This file provides code for the configuration
  *          of the RTC instances.
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
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */
/* Флаг «часы обнулились на этом boot» (маркёр 0xBEBE отсутствовал). Ставится
 * ниже в ветке обнуления, читается в main.c::ServiceStorageBootLog() → там
 * пишется событие EVT_CLOCKZERO в журнал внутренней Flash (счётчик «поколения
 * часов»). Определён в main.c рядом с g_wakeCause. (02.08.2026) */
extern volatile uint8_t g_clockZeroed;
/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */
  /* RTC SMOOTH-CALIBRATION. Базовый член (19.07.2026): в ПОКОЕ часы спешат
   * ≈ +305 ppm → замедляем (CALM=320, окно 32 c). 03.08.2026: значение больше
   * НЕ жёсткое — берётся из стр.123 (поэкземплярная поправка, «Калибровка» →
   * Часы RTC), дефолт = те же −305 ppm если пользователь не задал. Применяем на
   * КАЖДОМ boot, ДО проверки маркера (и при живых часах). После потери VBAT
   * RTC_CALR обнуляется — поэтому переприменяем из внутр. Flash здесь. */
  extern void rtc_calib_apply_from_flash(void);   /* Data.c */
  rtc_calib_apply_from_flash();
  /* Достоверность часов (17.07.2026). Backup-домен (RTC_BKP_DR0) гаснет ТОЛЬКО
   * с питанием. Маркер на месте → питание НЕ терялось (перешивка/watchdog/Stop2-
   * ресет), часы достоверны — НЕ сбрасываем, выходим. Маркера нет → питание
   * терялось (ОЗУ+часы потеряны) → даём установить НУЛЕВОЕ время ниже и помечаем
   * домен живым в RTC_Init 2. Так после полевого разрыва питания часы идут с 0,
   * а запись продолжается с первой чистой страницы (flashFindWritePos). */
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == 0xBEBE0001u)
    return;   /* часы достоверны — переинициализация не нужна */
  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0;
  sTime.Minutes = 0;
  sTime.Seconds = 0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 1;
  sDate.Year = 0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */
  /* Часы только что установлены в 0 (маркера не было = питание терялось).
   * Помечаем backup-домен живым — на СЛЕДУЮЩИХ сбросах без потери питания
   * часы сохранятся (проверка в Check_RTC_BKUP выше). (17.07.2026) */
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0xBEBE0001u);
  /* Взводим флаг «часы обнулились» — ServiceStorageBootLog() ниже по main.c
   * запишет EVT_CLOCKZERO в журнал iflash (поколение часов +1). Пишем не
   * здесь: журнал внутренней Flash на этом раннем этапе ещё не гарантирован
   * к записи (iflash_journal_ensure_ready вызывается позже). (02.08.2026) */
  g_clockZeroed = 1u;
  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */

  /* USER CODE END RTC_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();
  /* USER CODE BEGIN RTC_MspInit 1 */

  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */

  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();
  /* USER CODE BEGIN RTC_MspDeInit 1 */

  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

