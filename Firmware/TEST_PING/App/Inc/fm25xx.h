#ifndef __fm25xx_h_
#define __fm25xx_h_

#include "stm32l4xx_hal.h"

/* opcodes */
#define WREN			0x06
#define WRDI			0x04
#define RDSR			0x05
#define WRSR			0x01
#define READ			0x03
#define WRITE			0x02
#define PROTECT 		0x0c
#define UNPROTECT	        0x00
#define READID                  0x9f

/* Status Register bit position */
#define FM25XX_SR_WPEN        0x80            /*      Write Protect Enable bit     */
#define FM25XX_SR_BP1         0x08            /*      Block proteckt bit 1         */
#define FM25XX_SR_BP0         0x04            /*      Block proteckt bit 0         */
#define FM25XX_SR_WEL         0x02            /*      Write enable latch bit       */

void fm25xx_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *nss_port, uint32_t nss_pin, uint32_t density);
uint32_t fm25xx_ReadID(SPI_HandleTypeDef *hspi, uint8_t *id);
uint8_t fm25xx_SPI_ReadWriteByte(SPI_HandleTypeDef *hspi, uint8_t writeByte);
uint32_t fm25xx_getDensity(void);
uint8_t fm25xx_readStatus(SPI_HandleTypeDef *hspi);
void fm25xx_writeStatus(SPI_HandleTypeDef *hspi, uint8_t val);
void fm25xx_writeEnable(SPI_HandleTypeDef *hspi);
void fm25xx_writeDisable(SPI_HandleTypeDef *hspi);
uint8_t fm25xx_readSingle(SPI_HandleTypeDef *hspi, uint32_t addr);
void fm25xx_readMultiple(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t *buffer, uint32_t size);
void fm25xx_writeSingle(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t value);
void fm25xx_writeMultiple(SPI_HandleTypeDef *hspi, uint32_t addr, uint8_t *buffer, uint32_t size);

void fm25xx_beginWrite(SPI_HandleTypeDef *hspi, uint32_t addr);
void fm25xx_beginRead(SPI_HandleTypeDef *hspi, uint32_t addr);
void fm25xx_writeByte(SPI_HandleTypeDef *hspi, uint8_t byte);
uint8_t fm25xx_readByte(SPI_HandleTypeDef *hspi);
void fm25xx_close(SPI_HandleTypeDef *hspi);

uint8_t fm25xx_SPI_ReadWriteByte(SPI_HandleTypeDef *hspi, uint8_t writeByte);

#endif //__fm25xx_h_
