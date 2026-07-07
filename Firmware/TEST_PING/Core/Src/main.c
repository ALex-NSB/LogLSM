/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "rtc.h"
#include "gpio.h"
#include "dma.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "string.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
#include "quadspi.h"
#include "adc.h"
#include "dma.h"
#include "p25q128.h"
#include "Data.h"
#include "com.h"
#include "tmp117.h"
#include "fm25xx.h"
#include "lsm6dso_bus.h"
#include "stm_temp.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
LSM6DSO_Object_t lsm;
P25Qx_HandleTypeDef flash;
uint8_t flash_powered;
RegistratorData regist;
uint8_t ble_flag;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void lsm6dso_init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint8_t wkup1_pin_set(void)
{
  return (GPIO_PIN_SET == HAL_GPIO_ReadPin(WKUP1_GPIO_Port, WKUP1_Pin));
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */

  /* TEST_PING: DMA init перед UART */
  MX_DMA_Init();
  MX_USART2_UART_Init();

  /* Настройка пробуждения WKUP1 (PA0) — детект внешнего питания */
  HAL_PWREx_EnablePullUpPullDownConfig();
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, PWR_GPIO_BIT_0);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);

  /* TEST_PING: lsm6dso_init убран — тест только UART/PING */
  /* lsm6dso_init(); */

  regist.lsm = &lsm;
  regist.state = 0;
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* TEST_PING: всегда входим в Service() для теста PING */
    if (1 || 0 != __HAL_PWR_GET_FLAG(PWR_FLAG_WUF1) || wkup1_pin_set())
    {
      Service();
      __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
    }

    Poll_Sensor(&regist);

    if ((regist.state == 0) && RotationDetected(&regist))
    {
      RTC_GetTimeDate(&regist.rot.startTimeStamp);
      regist.rot.maxRate = regist.rot.maxVibr = 0;
      HandleSensorData(&regist);
      regist.state = 1;
    }
    else if (regist.state == 1)
    {
      if (RotationDetected(&regist))
      {
        HandleSensorData(&regist);
      }
      else
      {
        RTC_GetTimeDate(&regist.rot.stopTimeStamp);
        regist.totalSec += RTC_SubTimeDateSec(&regist.rot.stopTimeStamp, &regist.rot.startTimeStamp);
        HandleSensorData(&regist);
        SaveParamOnEEPROM(&regist);
        regist.state = 0;

        if (wkup1_pin_set())
        {
          uint8_t head = 0xC0;
          send(&head, 1);
          send((uint8_t *)&regist.rot, sizeof(RotationData));
        }
      }
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV4;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static void lsm6dso_init(void)
{
  LSM6DSO_IO_t pIO;
  pIO.Init     = I2C_LSMDSO_Init;
  pIO.DeInit   = I2C_LSMDSO_DeInit;
  pIO.BusType  = LSM6DSO_I2C_BUS;
  pIO.Address  = I2C_LSMDSO_ADDRESS;
  pIO.WriteReg = I2C_LSMDSO_WriteReg;
  pIO.ReadReg  = I2C_LSMDSO_ReadReg;
  pIO.GetTick  = LSMDSO_GetTick;
  pIO.Delay    = HAL_Delay;
  LSM6DSO_RegisterBusIO(&lsm, &pIO);
  LSM6DSO_Init(&lsm);

  LSM6DSO_ACC_SetFullScale(&lsm, 2);
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_ACC_ULTRA_LOW_POWER_MODE);
  LSM6DSO_GYRO_SetFullScale(&lsm, 2000);
  LSM6DSO_GYRO_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_GYRO_LOW_POWER_NORMAL_MODE);

  LSM6DSO_ACC_Enable(&lsm);
  LSM6DSO_GYRO_Enable(&lsm);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
