#include "StepDriver.h"
#include "stdlib.h"                     // abs

extern TIM_HandleTypeDef htim1;

uint16_t currSpeed;

void StepDriverInit()
{
  currSpeed = 0;
}

uint32_t GetTimFreq(TIM_HandleTypeDef *htim1)
{
  return HAL_RCC_GetPCLK2Freq();        // htim1
}

void SetTimPeriod(uint16_t speed, uint8_t coef)
{
    uint32_t f = speed * 200 * coef / 60;
    uint32_t period = GetTimFreq(&htim1) / htim1.Init.Prescaler / f;
    __HAL_TIM_SET_AUTORELOAD(&htim1, period);
    htim1.Instance->CCR1 = period / 2;
    if((htim1.Instance->CR1 & TIM_CR1_CEN) == 0)
      HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

void StepDriverSetSpeed(uint16_t newSpeed, uint8_t coef)
{
  if(newSpeed == 0)
  {
    HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
  }
  else
  {
    while((newSpeed - currSpeed) > 200)
    {
      currSpeed += 200;
      SetTimPeriod(currSpeed, coef);
      HAL_Delay(50);
    }
    SetTimPeriod(newSpeed, coef);
  }
  currSpeed = newSpeed;
}
