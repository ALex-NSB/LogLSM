#include "reset.h"
#include "FlashQ.h"

extern RTC_HandleTypeDef hrtc;

/*void WriteFlash(uint32_t addr, uint8_t *dataPtr, uint32_t size)
{
	__disable_irq();
	HAL_FLASH_Unlock();
	for(uint32_t i=0; i<size; i++)
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr+i, dataPtr[i]);
	HAL_FLASH_Lock();
	__enable_irq();
}*/

void SetPowerResetFlag(void)
{
        hrtc.Instance = RTC;
	HAL_PWR_EnableBkUpAccess();
	HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0xBEBE);
	HAL_PWR_DisableBkUpAccess();
}

uint8_t GetPowerResetFlag(void)
{
        hrtc.Instance = RTC;
	return (uint8_t)HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0);
}

int32_t FindEndOfData(uint8_t *buf, uint32_t start_index, uint32_t end_index)
{
	uint32_t mid_index;
	if(start_index > end_index)
		return -1;
	if(start_index == end_index)
	{
		if(buf[start_index] == 0xff)
			return start_index;
		else
			return -1;
	}
	mid_index = (end_index + start_index) / 2;
	if(buf[mid_index] == 0xff)
	{
		return FindEndOfData(buf, start_index, mid_index);
	}
	else
	{
		return FindEndOfData(buf, mid_index+1, end_index);
	}
}

int32_t FlashFindFreeAddr(void)
{
	uint8_t data[256];
	ReadFlash(0, data, sizeof(data));
	return FindEndOfData(data, 0, sizeof(data)-1);
}

void UpdateFlags(void)
{
	SetPowerResetFlag();
	RCC->CSR |= RCC_CSR_RMVF;
}

/*void SaveResetStatus(void)
{
	if(GetPowerResetFlag() == 0)
	{
		uint8_t flag = 0;
		int32_t addr = FlashFindFreeAddr();
		if(addr >= 0)
			WriteFlash(&flag, addr, 1);
		SetPowerResetFlag();
	}
}*/

void SaveResetStatus(void)
{
	int32_t addr = FlashFindFreeAddr();
	if(addr >= 0)
	{
		uint8_t flags;
		if(GetPowerResetFlag() == 0)
			flags = 0;
		else
			flags = ((uint32_t)RCC->CSR >> 24);
		WriteFlash(&flags, addr, 1);
	}
	UpdateFlags();
}