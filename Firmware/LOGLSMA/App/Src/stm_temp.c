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

/* ─────────────────────────────────────────────────────────────────────────
 * stm_adc_read — единое безопасное чтение VREFINT + TEMPSENSOR (12.06.2026).
 *
 * Полный bring-up ADC по запросу команды (НЕ в Service-init): при сбое
 * возвращает 0 и не вешает прибор. Все ожидания HAL — с явным таймаутом,
 * без Error_Handler. ADC выключается на выходе (низкое потребление).
 *
 *   *vdda  — напряжение питания аналоговой части, В (из VREFINT);
 *   *temp  — температура кристалла, °C (TEMPSENSOR, заводская калибровка,
 *            компенсированная по фактическому VDDA).
 * Возврат: 1 — успех, 0 — ошибка (значения = 0).
 * ───────────────────────────────────────────────────────────────────────── */
static int adc_read_channel(uint32_t channel, uint32_t sampling, uint16_t *out)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = sampling;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
        return 0;
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
        return 0;
    if (HAL_ADC_PollForConversion(&hadc1, 50) != HAL_OK) {
        HAL_ADC_Stop(&hadc1);
        return 0;
    }
    *out = (uint16_t)HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return 1;
}

int stm_adc_read(float *vdda, float *temp)
{
    if (vdda) *vdda = 0.0f;
    if (temp) *temp = 0.0f;

    hadc1.Instance = ADC1;
    HAL_ADC_DeInit(&hadc1);                     /* Instance задан — DeInit безопасен */

    hadc1.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV8;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.Overrun               = ADC_OVR_DATA_PRESERVED;
    hadc1.Init.OversamplingMode      = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)         /* MSP включит клок + ADCSEL=SYSCLK */
        return 0;
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
        HAL_ADC_DeInit(&hadc1);
        return 0;
    }

    /* Внутренние каналы требуют длинной выборки — 640.5 циклов с запасом */
    uint16_t vref_raw = 0, ts_raw = 0;
    const int ok_vref = adc_read_channel(ADC_CHANNEL_VREFINT,
                                         ADC_SAMPLETIME_640CYCLES_5, &vref_raw);
    const int ok_ts   = adc_read_channel(ADC_CHANNEL_TEMPSENSOR,
                                         ADC_SAMPLETIME_640CYCLES_5, &ts_raw);
    HAL_ADC_DeInit(&hadc1);

    if (!ok_vref || vref_raw == 0)
        return 0;

    /* VDDA = 3.0 В * VREFINT_CAL / VREFINT_DATA (cal снят при 3.0 В) */
    const float vdda_val = (3.0f * (float)VREFINT_CALIB) / (float)vref_raw;
    if (vdda) *vdda = vdda_val;

    if (ok_ts && temp) {
        /* TS_DATA приводим к 3.0 В, затем линейно по двум заводским точкам */
        const float ts_at_3v = (float)ts_raw * vdda_val / 3.0f;
        const float c1 = (float)TS_CAL1, c2 = (float)TS_CAL2;
        *temp = ((ts_at_3v - c1) * (TS_2 - TS_1)) / (c2 - c1) + TS_1;
    }
    return 1;
}

void calc_vref(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    /* Instance задаётся ДО DeInit. При холодном старте ADC ещё не
     * инициализирован (MX_ADC1_Init не вызывается), и DeInit с NULL-Instance
     * ранее уходил в Error_Handler() → вечный цикл → SERVICE не стартовал.
     * Некритичный датчик не должен вешать устройство: failure → return. */
    hadc1.Instance = ADC1;
    HAL_ADC_DeInit(&hadc1);
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
        return;
    sConfig.Channel = ADC_CHANNEL_VREFINT;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
        return;
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
        return;
    HAL_ADC_Start(&hadc1);
    if( HAL_ADC_PollForConversion( &hadc1, 10 ) == HAL_OK ) //???????? ??????????????
    {
        float res = (float)HAL_ADC_GetValue(&hadc1);       //?????? ?? ???
        float vdda_val = (3.0f*VREFINT_CALIB)/(res);         //??????? ???????? Vdda
        float v_calib = 3.0f/vdda_val;                     //??????? ???????????
        TS_CALIB1_NEW  = TS_CAL1*v_calib;
        TS_CALIB2_NEW  = TS_CAL2*v_calib;
    }
    HAL_ADC_Stop(&hadc1);
    HAL_ADC_DeInit(&hadc1);
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
    return;
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    return;

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
    return;
  
  /*float v_calib = 3.0f/3.3;
  TS_CALIB1_NEW  = TS_CAL1 * v_calib;
  TS_CALIB2_NEW  = TS_CAL2 * v_calib;*/
}
