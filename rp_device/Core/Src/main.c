/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "string.h"
//#include "FlashQ.h"
#include "com.h"
#include "tmp117.h"
#include "fm25xx.h"
#include "lsm6dso_bus.h"
#include "stm_temp.h"
#include "usart.h"
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
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void FlashOn();
void lsm6dso_init();
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
uint8_t wkup1_pin_set()
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
  
  MX_USART2_UART_Init();
  //while(1)
  {
  //uint8_t *info = "program start\r\n";
  //HAL_UART_Transmit(&huart2, info, strlen(info), 1000);
  
  }
  //HAL_UART_DeInit(&huart2);
  //Настройка пробуждающего WKUP1
  HAL_PWREx_EnablePullUpPullDownConfig();
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, PWR_GPIO_BIT_0);
  //__HAL_RCC_PWR_FORCE_RESET();
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);
  //
  
  ///если датчик не настроен, то настраиваем
  ///варианты проверок:
  ///1 сброс по питанию (анализ флагов или bakup domain)
  ///2 проверка значения регистра датчика (контроллер не сбросился, датчик да, возможен ли вариант?)

  lsm6dso_init();
  //
  regist.lsm = &lsm;
  regist.state = 0;
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  
  while (1)
  {
    if(0 != __HAL_PWR_GET_FLAG(PWR_FLAG_WUF1))
    {
      //HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
      
      Service();
      
      //__HAL_GPIO_EXTI_CLEAR_IT(INT1_Pin);
      //HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
      
      __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
    }
    
    Poll_Sensor(&regist);
    
    if((regist.state == 0) && RotationDetected(&regist))
    {
      RTC_GetTimeDate(&regist.rot.startTimeStamp);
      regist.rot.maxRate = regist.rot.maxVibr = 0;
      HandleSensorData(&regist);
      regist.state = 1;
    }
    else if (regist.state == 1)
    {
      if(RotationDetected(&regist))
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
        
        if(wkup1_pin_set())
        {
          char head = 0xC0;
          send(&head, 1);
          send((uint8_t*)&regist.rot, sizeof(RotationData));
        }
      }
    }
    
    //if(wkup1_pin_set())
    /*{
      RTC_GetTimeDate(&rot.startTimeStamp);
      send(&head, 1);
      send((uint8_t*)&rot, sizeof(rot));
      HAL_Delay(1000);
    }*/
    
    ///если не сервис, настройка I2C датчика

    ///проверка вращения 

    ///если вращение, то настраиваем флешку
    //flash_powered = 0;
    //FlashOn();
    //P25Qx_SetQuadSpi(&flash);
    //FlashOff();
    //fm25xx_Init(&hspi1, SPI1_CS_GPIO_Port, SPI1_CS_Pin, 128000);
    //Work()
    
    //Включаем термометр когда нужно скорректировать генератор часов
    //tmp117_Init(&hi2c2);

    //calc_vref();
    //ADC1_Temp_Channel_Init();
   
    
    ///Уходим в режим пониженного ЭП
    //__HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
    //HAL_PWREx_EnterSHUTDOWNMode();
    
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

void lsm6dso_init()
{
  LSM6DSO_IO_t pIO;
  pIO.Init = I2C_LSMDSO_Init;
  pIO.DeInit = I2C_LSMDSO_DeInit;
  pIO.BusType = LSM6DSO_I2C_BUS;
  pIO.Address = I2C_LSMDSO_ADDRESS;
  pIO.WriteReg = I2C_LSMDSO_WriteReg;
  pIO.ReadReg = I2C_LSMDSO_ReadReg;
  pIO.GetTick = LSMDSO_GetTick;
  pIO.Delay = HAL_Delay;
  LSM6DSO_RegisterBusIO(&lsm, &pIO);
  LSM6DSO_Init(&lsm);
  
  LSM6DSO_ACC_SetFullScale(&lsm, 2);
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_ACC_ULTRA_LOW_POWER_MODE);
  LSM6DSO_GYRO_SetFullScale(&lsm, 2000);
  LSM6DSO_GYRO_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_GYRO_LOW_POWER_NORMAL_MODE);
  
  LSM6DSO_ACC_Enable(&lsm);
  LSM6DSO_GYRO_Enable(&lsm);
}

#if 0
static uint8_t LSM_FIFO_DATA[512*7];

void lsm6dso_init()
{
  LSM6DSO_IO_t pIO;
  pIO.Init = I2C_LSMDSO_Init;
  pIO.DeInit = I2C_LSMDSO_DeInit;
  pIO.BusType = LSM6DSO_I2C_BUS;
  pIO.Address = I2C_LSMDSO_ADDRESS;
  pIO.WriteReg = I2C_LSMDSO_WriteReg;
  pIO.ReadReg = I2C_LSMDSO_ReadReg;
  pIO.GetTick = LSMDSO_GetTick;
  pIO.Delay = HAL_Delay;
  LSM6DSO_RegisterBusIO(&lsm, &pIO);
  LSM6DSO_Init(&lsm);
  
  //LSM6DSO_Write_Reg(&lsm, LSM6DSO_CTRL1_XL, 0x64);      // ODR_XL = 208 Hz, FS_XL = ±16 g
  //LSM6DSO_Write_Reg(&lsm, LSM6DSO_CTRL1_XL, 0x6C);      // ODR_XL = 416 Hz, FS_XL = ±8 g
  //LSM6DSO_Write_Reg(&lsm, LSM6DSO_CTRL1_XL, 0x1C);      // ODR_XL = 12,5 Hz, FS_XL = ±8 g
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_CTRL1_XL, 0x10);      // ODR_XL = 12,5 Hz, FS_XL = ±2 g
  
  //LSM6DSO_ACC_SetOutputDataRate(&lsm, 208);
  //LSM6DSO_ACC_SetFullScale(&lsm, 16);
  //LSM6DSO_ACC_Enable(LSM6DSO_Object_t *pObj);
  
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_CTRL2_G, 0x40);       // ODR_G = 104 Hz, FS_G = ±250 dps
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_WAKE_UP_DUR, 0x60);
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_WAKE_UP_THS, 0x02);   //62 mg (1 THS = FS_XL/2^6 = 31 mg)‬
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_TAP_CFG0, 0x00);
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_TAP_CFG2, 0xE0);
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_MD1_CFG, 0x80);
  
  //FIFO Bypass-FIFO mode
  //LSM6DSO_Write_Reg(&lsm, LSM6DSO_MD2_CFG, 0x20);       //INT2_WU=1
  
  LSM6DSO_FIFO_Set_Watermark_Level(&lsm, 500);
  LSM6DSO_FIFO_Set_Stop_On_Fth(&lsm, 1);
  LSM6DSO_FIFO_ACC_Set_BDR(&lsm, 12.5);
  LSM6DSO_FIFO_GYRO_Set_BDR(&lsm, 104);
  //LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_TO_FIFO_MODE);
  
  LSM6DSO_Write_Reg(&lsm, LSM6DSO_INT1_CTRL, 0x08);
  
    ///Прерывание
  /*GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = INT1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(INT1_GPIO_Port, &GPIO_InitStruct);
  
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 15, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);*/
  
  
  /*LSM6DSO_ACC_SetFullScale(&lsm, 2);
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_ACC_ULTRA_LOW_POWER_MODE);
  LSM6DSO_GYRO_SetFullScale(&lsm, 2000);
  LSM6DSO_GYRO_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_GYRO_LOW_POWER_NORMAL_MODE);
  
  LSM6DSO_ACC_Enable(&lsm);
  LSM6DSO_GYRO_Enable(&lsm);*/
}


uint32_t GetFIFOBuffer(uint8_t *dst)
{
  /*uint8_t fifo_status1, fifo_status2;
  uint16_t fifo_samples, fifo_bytes;
  
  LSM6DSO_Read_Reg(&lsm, LSM6DSO_FIFO_STATUS1, &fifo_status1);
  LSM6DSO_Read_Reg(&lsm, LSM6DSO_FIFO_STATUS2, &fifo_status2);
  fifo_samples = (((uint16_t)(fifo_status2 & 1) << 8) | fifo_status1);
  fifo_bytes = fifo_samples * 7;*/
  
  uint16_t fifo_diff;
  uint32_t size;
  lsm6dso_fifo_data_level_get(&(lsm.Ctx), &fifo_diff);
  size = fifo_diff * 7;
  lsm6dso_read_reg(&(lsm.Ctx), LSM6DSO_FIFO_DATA_OUT_TAG, dst, size);
  return size;
}

void Process_LSM_Data(uint8_t *d, uint32_t size)
{
  printff("%d\r\n", size);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == INT1_Pin)
  {
    uint8_t wkup_src, allint_src;
    LSM6DSO_Read_Reg(&lsm, LSM6DSO_WAKE_UP_SRC, &wkup_src);
    //LSM6DSO_Read_Reg(&lsm, LSM6DSO_ALL_INT_SRC, &allint_src);
    
    printff("[%02X]", wkup_src);
    
    if(wkup_src & 0x40)         //SLEEP_CHANGE_IA=1
    {
      //LSM6DSO_Read_Reg(&lsm, LSM6DSO_WAKE_UP_SRC, &wkup_src);
      //printff("[%02X]", wkup_src);
      
      if(wkup_src & 0x10)       //SLEEP_STATE=1 (Inactivity)
      {
        LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_MODE);
        print("FINISH\r\n");
      }
      else
      {
        LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_STREAM_MODE);
        //LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_FIFO_MODE);
        print("START\r\n");
      }
    }
    else
    {
      uint8_t fifo_status2;
      LSM6DSO_Read_Reg(&lsm, LSM6DSO_FIFO_STATUS2, &fifo_status2);
      if(fifo_status2 & 0x80)   //FIFO_WTM_IA=1
      {
        uint32_t size = GetFIFOBuffer(LSM_FIFO_DATA);
        //Process_LSM_Data(LSM_FIFO_DATA, size);
        //LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_MODE);
        //LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_FIFO_MODE);
        print("B\r\n");
        
        //обрабатываем если сигнал неактив пришел во время чтения буффера
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_Pin);
        LSM6DSO_Read_Reg(&lsm, LSM6DSO_WAKE_UP_SRC, &wkup_src);
        if(wkup_src & 0x10)       //SLEEP_STATE=1 (Inactivity)
        {
          LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_MODE);
          print("FINISH\r\n");
        }
        
      }
      else
      {
        print("ERROR\r\n");
      }
      
    }
  }
}

void HAL_GPIO_EXTI_Callback1(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == INT1_Pin)
  {
    static uint8_t fifo_status1, fifo_status2;
    static uint16_t fifo_samples, fifo_bytes;
    static uint8_t tag;
    static LSM6DSO_AxesRaw_t data;
    
    LSM6DSO_Read_Reg(&lsm, LSM6DSO_FIFO_STATUS1, &fifo_status1);
    LSM6DSO_Read_Reg(&lsm, LSM6DSO_FIFO_STATUS2, &fifo_status2);
    fifo_samples = (((uint16_t)(fifo_status2 & 1) << 8) | fifo_status1);
    
    //printff("<%d>\r\n", fifo_bytes);
    /*if(fifo_bytes != 0)
    {
      lsm6dso_read_reg(&(lsm.Ctx), LSM6DSO_FIFO_DATA_OUT_TAG, LSM_FIFO_DATA, fifo_bytes);
      printHex(LSM_FIFO_DATA, fifo_bytes, 7);
    }*/
    while(fifo_samples--)
    {
      LSM6DSO_FIFO_Get_Tag(&lsm, &tag);
      LSM6DSO_FIFO_Get_Data(&lsm, (uint8_t*)&data);
      printff("%02X %d  %d  %d\r\n", tag, data.x, data.y, data.z);
    }
    print("\r\n");
    
    LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_MODE);
    //LSM6DSO_FIFO_ACC_Set_BDR(&lsm, 208);
    //LSM6DSO_FIFO_GYRO_Set_BDR(&lsm, 104);
    LSM6DSO_FIFO_Set_Mode(&lsm, LSM6DSO_BYPASS_TO_FIFO_MODE);
    //LSM6DSO_Write_Reg(&lsm, LSM6DSO_INT1_CTRL, 0x08);
  }
}
#endif // 0

void FlashOn()
{
  if(flash_powered)
    return;
  
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  
  HAL_GPIO_WritePin(FLASH_0_GPIO_Port, FLASH_0_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(FLASH_22_GPIO_Port, FLASH_22_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  
  GPIO_InitStruct.Pin = FLASH_22_Pin;
  HAL_GPIO_Init(FLASH_22_GPIO_Port, &GPIO_InitStruct);  

  GPIO_InitStruct.Pin = FLASH_0_Pin;
  HAL_GPIO_Init(FLASH_0_GPIO_Port, &GPIO_InitStruct);
  HAL_Delay(60);
  
  MX_QUADSPI_Init();
  
  flash.hqspi = &hqspi;
  P25Qx_Init(&flash);

  flash_powered = 1;
}

void FlashOff()
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  
  HAL_QSPI_DeInit(&hqspi);
  
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  GPIO_InitStruct.Pin = FLASH_0_Pin;
  HAL_GPIO_Init(FLASH_0_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FLASH_22_Pin;
  HAL_GPIO_Init(FLASH_22_GPIO_Port, &GPIO_InitStruct);
  
  P25Qx_DeInit(&flash);
  
  flash_powered = 0;
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
