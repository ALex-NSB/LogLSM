#include "fm25xx.h"

/* drive nss pin */
#define NSS_EN                  HAL_GPIO_WritePin(fram_nss_port, fram_nss_pin, GPIO_PIN_RESET)
#define NSS_DN                  HAL_GPIO_WritePin(fram_nss_port, fram_nss_pin, GPIO_PIN_SET)

#define SPI_MAX_DELAY           1000

/* local variables */
static uint32_t fram_density;
static uint8_t fram_init = 0;
static GPIO_TypeDef *fram_nss_port;
static uint32_t fram_nss_pin;

uint8_t fm25xx_SPI_ReadWriteByte(SPI_HandleTypeDef *hspi, uint8_t writeByte)
{
  uint8_t recvByte;
  HAL_SPI_TransmitReceive(hspi, &writeByte, &recvByte, 1, SPI_MAX_DELAY);
  return recvByte;
}

void fm25xx_SPI_nss_en(void)
{
  NSS_EN;
}

void fm25xx_SPI_nss_dn(void)
{
  NSS_DN;
}

void writeAddress(SPI_HandleTypeDef *hspi, uint32_t addr)
{
  if(fm25xx_getDensity() > 0xffff)
  {
    fm25xx_SPI_ReadWriteByte(hspi, (uint8_t)(addr >> 16));
  }
    fm25xx_SPI_ReadWriteByte(hspi, (uint8_t)(addr >> 8));
    fm25xx_SPI_ReadWriteByte(hspi, (uint8_t)addr);
}

void fm25xx_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *nss_port, uint32_t nss_pin, uint32_t density)
{
  fram_nss_port = nss_port;
  fram_nss_pin = nss_pin;
  fram_density = density;
  fram_init = 1;
}

uint32_t fm25xx_ReadID(SPI_HandleTypeDef *hspi, uint8_t *id)
{
  if(!fram_init)
    return 0;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, READID);
  for(uint8_t i=0; i<9; i++)
    id[i] = fm25xx_SPI_ReadWriteByte(hspi, 0);
  NSS_DN;
  return 9;
}

uint8_t fm25xx_readStatus(SPI_HandleTypeDef *hspi)
{
  if(!fram_init)
    return 0;
  uint8_t statusReg;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, RDSR);
  statusReg = fm25xx_SPI_ReadWriteByte(hspi, 0);
  NSS_DN;
  return statusReg;
}


void fm25xx_writeStatus(SPI_HandleTypeDef *hspi, uint8_t val)
{
  if(!fram_init)
    return;
  fm25xx_writeEnable(hspi);
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WRSR);
  fm25xx_SPI_ReadWriteByte(hspi, val);
  NSS_DN;
}


void fm25xx_writeEnable(SPI_HandleTypeDef *hspi)
{
  if(!fram_init)
    return;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WREN);
  NSS_DN;
}


void fm25xx_writeDisable(SPI_HandleTypeDef *hspi)
{
  if(!fram_init)
    return;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WRDI);
  NSS_DN;
}


uint8_t fm25xx_readSingle(SPI_HandleTypeDef *hspi, uint32_t addr)
{
  if(!fram_init)
    return 0;
  uint8_t recvByte;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, READ);
  writeAddress(hspi, addr);
  recvByte = fm25xx_SPI_ReadWriteByte(hspi, 0);
  NSS_DN;
  return recvByte;
}



void fm25xx_readMultiple(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t *buffer, uint32_t size)
{
  if(!fram_init)
    return;
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, READ);
  writeAddress(hspi, addr);
  while(size--)
  {
    *buffer++ = fm25xx_SPI_ReadWriteByte(hspi, 0);
  }
  NSS_DN;
}


void fm25xx_writeSingle(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t value)
{
  if(!fram_init)
    return;
  fm25xx_writeEnable(hspi);
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WRITE);
  writeAddress(hspi, addr);
  fm25xx_SPI_ReadWriteByte(hspi, value);
  NSS_DN;
}


void fm25xx_writeMultiple(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t *buffer, uint32_t size)
{
  if(!fram_init)
    return;
  fm25xx_writeEnable(hspi);
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WRITE);
  writeAddress(hspi, addr);
  while(size--)
  {
    fm25xx_SPI_ReadWriteByte(hspi, *buffer++);
  }
  NSS_DN;
}

uint32_t fm25xx_getDensity(void)
{
  return fram_density;
}

void fm25xx_beginWrite(SPI_HandleTypeDef *hspi, uint32_t addr)
{
  if(!fram_init)
    return;
  NSS_DN;
  HAL_Delay(1);
  fm25xx_writeEnable(hspi);
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, WRITE);
  fm25xx_SPI_ReadWriteByte(hspi, addr);
}

void fm25xx_beginRead(SPI_HandleTypeDef *hspi, uint32_t addr)
{
  if(!fram_init)
    return;
  NSS_DN;
  HAL_Delay(1);
  NSS_EN;
  fm25xx_SPI_ReadWriteByte(hspi, READ);
  fm25xx_SPI_ReadWriteByte(hspi, addr);
}

void fm25xx_writeByte(SPI_HandleTypeDef *hspi, uint8_t byte)
{
  fm25xx_SPI_ReadWriteByte(hspi, byte);
}

uint8_t fm25xx_readByte(SPI_HandleTypeDef *hspi)
{
  return fm25xx_SPI_ReadWriteByte(hspi, 0);
}

void fm25xx_close(SPI_HandleTypeDef *hspi)
{
  NSS_DN;
}
