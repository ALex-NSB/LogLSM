//Library for TMP117-High-Precision-Digital-Temperature-Sensor
//I have written it using HAL Api.
//I have not added exception handling in this library because it makes code bulky.
//you can simply use exception handling in your main function.
//Hope it helps!


#ifndef      _tmp117_H_
#define      _tmp117_H_	

#include "stm32l4xx_hal.h"

#ifndef     __STM32F4xx_HAL_I2C_H
#define     __STM32F4xx_HAL_I2C_H
#endif

//######## Error codes ##############
#define TMP117_OK       0
#define TMP117_ERR      1
//###################################

#define         tmp117_GND                            0x48<<1    //	GND
#define         tmp117_VCC                            0x49<<1    //	VCC
#define         tmp117_SDA                            0x4A<<1    //	SDA
#define         tmp117_SCL                            0x4B<<1    //	SCL

/*------------Pointer Registers-----------------------*/

#define         tmp117_TempReg    			0x00 
#define         tmp117_ConfigReg  			0x01
#define         tmp117_TempHighLimit  	        0x02
#define         tmp117_TempLowLimit    	        0x03
#define         tmp117_EEPROM_Unlock                  0x04
#define         tmp117_EEPROM1                        0x05
#define         tmp117_EEPROM2        	        0x06
#define         tmp117_Temp_Offset   	                0x07
#define         tmp117_EEPROM3                        0x08
#define         tmp117_ID_Reg		                0x0F

#define         TMP117_CONFIG_DRDY                      0x2000


uint8_t tmp117_Init(I2C_HandleTypeDef *i2c); //Initialization by giving the parameters

enum TMP117_AVE{NOAVE = 0, AVE8, AVE32, AVE64};    //Averaging mode No Average, Average 8,32,64

void tmp117_set_Averaging(I2C_HandleTypeDef *i2c, enum TMP117_AVE ave);    //set averaging

void tmp117_set_Alert_Temp(I2C_HandleTypeDef *i2c,uint8_t Lowfirst,uint8_t LowSecond,uint8_t Highfirst,uint8_t HighSecond); //set the high and low limit register for alert

float tmp117_get_Temp(I2C_HandleTypeDef *i2c); //Return the temprature in float.

uint16_t tmp117_get_Config(I2C_HandleTypeDef *i2c); //Return TMP117 configuration register value

uint8_t tmp117_set_Config(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); // Set the configuration Reg for features.

uint8_t tmp117_set_HighLimit(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set high limit for alert

uint16_t tmp117_get_HighLimit(I2C_HandleTypeDef *i2c); //Get the highlimit register value

uint8_t tmp117_set_LowLimit(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set low limit for alert

uint16_t tmp117_get_LowLimit(I2C_HandleTypeDef *i2c); //Get the low limit register value

uint16_t tmp117_get_EEPROM_Unlock(I2C_HandleTypeDef *i2c); //Get EEPROM Unlock Register Value

void tmp117_set_EEPROM_Unlock(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set EEPROM Unlock Register value

void tmp117_set_EEPROM1(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set EEPROM1

uint16_t tmp117_get_EEPROM1(I2C_HandleTypeDef *i2c); //Get the Value from EEPROM1 

void tmp117_set_EEPROM2(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set EEPROM2

uint16_t tmp117_get_EEPROM2(I2C_HandleTypeDef *i2c); //Get the Value from EEPROM2

void tmp117_set_EEPROM3(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set EEPROM3

uint16_t tmp117_get_EEPROM3(I2C_HandleTypeDef *i2c); //Get the Value from EEPROM3

uint8_t tmp117_set_Temp_Offset(I2C_HandleTypeDef *i2c,uint8_t first,uint8_t second); //Set temperature offset value

uint16_t tmp117_get_Temp_Offset(I2C_HandleTypeDef *i2c); //Get temperature offset value

uint16_t tmp117_get_ID_Reg(I2C_HandleTypeDef *i2c); //Get ID Register


#endif
