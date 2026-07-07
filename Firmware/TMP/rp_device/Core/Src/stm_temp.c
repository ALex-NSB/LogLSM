#include "stm_temp.h"
#include "main.h"

#define TS_CAL1 *((uint16_t*)0x1FFF75A8)
#define TS_CAL2 *((uint16_t*)0x1FFF75CA)
#define VREFINT_CALIB *((uint16_t*)0x1FFF75AA)
#define TS_1 30.0f
#define TS_2 130.0f

float TS_CALIB1_NEW = 0.0f;
float TS_CALIB2_NEW = 0.0f;

extern ADC_HandleTypeDef hadc1;

void calc_vref(void)
{
    if (HAL_ADC_DeInit(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }
    ADC_ChannelConfTypeDef sConfig = {0};
    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV8;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
    hadc1.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }
    sConfig.Channel = ADC_CHANNEL_VREFINT;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
    {
        Error_Handler();
    }
    HAL_ADC_Start(&hadc1); //?????? ???.
    if( HAL_ADC_PollForConversion( &hadc1, 10 ) == HAL_OK ) //???????? ??????????????
    {
        float res = (float)HAL_ADC_GetValue(&hadc1);       //?????? ?? ???
        float vdda_val = (3.0f*VREFINT_CALIB)/(res);         //??????? ???????? Vdda
        float v_calib = 3.0f/vdda_val;                     //??????? ???????????
        TS_CALIB1_NEW  = TS_CAL1*v_calib;
        TS_CALIB2_NEW  = TS_CAL2*v_calib;
    }
    HAL_ADC_Stop(&hadc1); //???????? ???
    if (HAL_ADC_DeInit(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }
}

void get_stm_temp(float *temp)
{
    HAL_ADC_Start(&hadc1); // ?????? ???.
    if( HAL_ADC_PollForConversion( &hadc1, 20 ) == HAL_OK ) // ???????? ??????????????
    {
        float res = (float)HAL_ADC_GetValue(&hadc1);
        *temp = ((res - TS_CALIB1_NEW)*(TS_2 - TS_1))/(TS_CALIB2_NEW - TS_CALIB1_NEW) + TS_1;
    }
    HAL_ADC_Stop(&hadc1);
}

void ADC1_Temp_Channel_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV8;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  
  /*float v_calib = 3.0f/3.3;
  TS_CALIB1_NEW  = TS_CAL1 * v_calib;
  TS_CALIB2_NEW  = TS_CAL2 * v_calib;*/
}
