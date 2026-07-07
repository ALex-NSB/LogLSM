#include "command.h"
#include "wake.h"
#include "stdio.h"
#include "string.h"

#define CMD_NONE                        (int16_t)-1
#define CMD_ECHO                        0x01
#define CMD_TST_FLASH                   0x02
#define CMD_TST_MEMS                    0x0B
#define CMD_TST_TMP                     0x13
#define CMD_TST_FRAM                    0x17
#define CMD_TST_BLE                     0x0F
#define CMD_TST_STOP                    0x55
#define CMD_TST_FRAM2                   0x66

static int16_t cmd;
static WakeContext wc;
static uint8_t ComLink;

void print(char *str);
void send(uint8_t *data, uint32_t size);
void Echo(uint8_t *data, uint8_t size);
void TestFlash(void);
void TestFram(void);
void TestFram2(void);
void TestTemp(void);
void TestBLE(void);
void TestStop(void);

uint8_t InService(void)
{
  return (uint8_t)(HAL_GPIO_ReadPin(WKUP1_GPIO_Port, WKUP1_Pin) == GPIO_PIN_SET);
}

void EnableRxIT(void)
{
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  //__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE | UART_IT_ORE);
}

void DisableRxIT(void)
{
  HAL_NVIC_DisableIRQ(USART2_IRQn);
  //__HAL_UART_DISABLE_IT(&huart2, UART_IT_RXNE | UART_IT_ORE);
}

void print(char *str)
{
  if(ComLink == 1)
    HAL_UART_Transmit(&huart1, (uint8_t*)str, strlen(str), 1000);
  else
    HAL_UART_Transmit(&huart2, (uint8_t*)str, strlen(str), 1000);
}

void send(uint8_t *data, uint32_t size)
{
  if(ComLink == 1)
    HAL_UART_Transmit(&huart1, data, size, 1000);
  else
    HAL_UART_Transmit(&huart2, data, size, 1000);
}

/*void HAL_UART_RxCpltCallback(UART_HandleTypeDef *UartHandle)
{

}*/

void Service_IRQHandler(UART_HandleTypeDef *UartHandle)
{
  HAL_GPIO_WritePin(TEST_GPIO_Port, TEST_Pin, GPIO_PIN_SET);
  uint32_t isrflags = UartHandle->Instance->ISR;
  if(isrflags | USART_ISR_ORE)
  {
    SET_BIT(UartHandle->Instance->ICR, USART_ICR_ORECF);
  }    
  if(isrflags | USART_ISR_RXNE)
  {
    uint8_t rdrreg = UartHandle->Instance->RDR;
    WakeProtocolParse(&wc, rdrreg);
    if(wc.packet_recognized)
    {
      wc.packet_recognized = 0;
      cmd = wc.cmd;
      DisableRxIT();
    }
  }
  HAL_GPIO_WritePin(TEST_GPIO_Port, TEST_Pin, GPIO_PIN_RESET);
}

void ServiceInitPeriph(void)
{
  MX_USART2_UART_Init();
  __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE | UART_IT_ORE);

  framActiv();
  tmp117Activ();
  flashActiv();
}

void ServiceDeInitPeriph(void)
{
  //HAL_UART_DeInit(&huart2);
}

void Service(void)
{
  cmd = CMD_NONE;
  ComLink = 2;
  WakeProtocolInit(&wc);
  ServiceInitPeriph();
  EnableRxIT();
  while(InService())
  {
    switch(cmd)
    {
    case CMD_ECHO:
      Echo(wc.data, wc.n);
      break;
    case CMD_TST_FLASH:
      TestFlash();
      break;
    case CMD_TST_MEMS:
      
      break;
    case CMD_TST_TMP:
      TestTemp();
      break;
    case CMD_TST_FRAM:
      TestFram();
      break;
    case CMD_TST_FRAM2:
      TestFram2();
      break;
    case CMD_TST_BLE:
      TestBLE();
    case CMD_TST_STOP:
      TestStop();
    default:
      ;
    }
  }
}

void Echo(uint8_t *data, uint8_t size)
{
  send(data, size);
  cmd = CMD_NONE;
  EnableRxIT();
}

void TestStop(void)
{
  cmd = CMD_NONE;
  EnableRxIT();
}

void TestFlash(void)
{
  char str[64];
  uint32_t addr = 0;
  uint8_t page[P25Q_PAGE_SIZE];
  uint8_t flag_0 = 1, flag_ff = 1;
  EnableRxIT();
  while((cmd == CMD_TST_FLASH) && (InService()))
  {
    snprintf(str, sizeof(str), "Page(%d bytes) #%d\n", sizeof(page), addr/sizeof(page));
    print(str);
    for(uint32_t k=0; k<sizeof(page); k++)
      page[k] = 0;
    P25Qx_QPI_ProgramPage(&flash, addr, page, sizeof(page));
    P25Qx_QPI_Read(&flash, addr, sizeof(page), page);
    for(uint32_t k=0; k<sizeof(page); k++)
    {
      if(page[k] != 0)
      {
        flag_0 = 0;
        break;
      }
    }
    P25Qx_QPI_ErasePage(&flash, addr);
    P25Qx_QPI_Read(&flash, addr, sizeof(page), page);
    for(uint32_t k=0; k<sizeof(page); k++)
    {
      if(page[k] != 0xff)
      {
        flag_ff = 0;
        break;
      }
    }
    if(flag_0 == 1)
      print("ok 0\n");
    else
      print("bad 0\n");
    if(flag_ff == 1)
      print("ok ff\n");
    else
      print("bad ff\n");
    addr = addr + sizeof(page);
    addr %= P25Q_FLASH_SIZE;
  }
}

void TestFram2(void)
{
  uint8_t id[9];
  EnableRxIT();
  while((cmd == CMD_TST_FRAM2) && (InService()))
  {
    fm25xx_ReadID(&hspi1, id);
  }
}

void TestFram(void)
{
  uint32_t addr = 0;
  const uint32_t blockSize = 4096;
  uint8_t n[] = {0xff, 0x00};
  uint8_t res[] = {0, 0};
  char str[64];
  EnableRxIT();
  while((cmd == CMD_TST_FRAM) && (InService()))
  {
    snprintf(str, sizeof(str), "Block(%d bytes) #%d\n", blockSize, addr/blockSize);
    print(str);
    for(uint32_t i=0; i<sizeof(n); i++)
    {
      fm25xx_beginWrite(&hspi1, addr);
      for(uint32_t k=0; k<blockSize; k++)
        fm25xx_writeByte(&hspi1, n[i]);
      fm25xx_close(&hspi1);
      fm25xx_beginRead(&hspi1, addr);
      for(uint32_t k=0; k<blockSize; k++)
        if(n[i] != fm25xx_readByte(&hspi1))
        {
          res[i] = 1;
          break;
        }
      fm25xx_close(&hspi1);
    }
    if(res[0] == 0)
      print("ok 1\n");
    else
      print("bad 1\n");
    if(res[1] == 0)
      print("ok 0\n");
    else
      print("bad 0\n");
    addr = (addr + blockSize) % fm25xx_getDensity();
  }
}

void TestTemp(void)
{
  char str[64];
  float temp;
  EnableRxIT();
  while((cmd == CMD_TST_TMP) && (InService()))
  {
    temp = tmp117_get_Temp(&hi2c2);
    snprintf(str, sizeof(str), "(TMP117) %f °C\n", temp);
    print(str);
  }
}

void TestBLE(void)
{
  uint8_t err;
  char str[64];
  USART_TypeDef *uvcp;
  USART_TypeDef *uble;
  uint32_t uvcpISR, ubleISR;
  uint32_t rx;
  uint8_t transparent;
  ble_flag = 0;
  bleActiv();
  uvcp = huart2.Instance;
  uble = huart1.Instance;
  print("BLE Init...\n");
  err = BLE_Init();
  if(err == BLE_OK)
    print("BLE ok\n");
  else
  {
    snprintf(str, sizeof(str), "BLE error (code %d)\n", err);
    print(str);
    goto endBleTest;
  }
  transparent = 1;
  while((cmd == CMD_TST_BLE) && InService())
  {
    if(transparent)
    {
      if(!BLE_Link())
      {
        transparent = 0;
        EnableRxIT();
        continue;
      }
      //uvcp->ICR |= USART_ICR_ORECF;
      //uble->ICR |= USART_ICR_ORECF;
      uvcpISR = uvcp->ISR;
      ubleISR = uble->ISR;
      if((uvcpISR & USART_ISR_RXNE) != 0)
      {
        rx = (uint8_t)uvcp->RDR;
        uble->TDR = rx;
      }
      if((ubleISR & USART_ISR_RXNE) != 0)
      {
        rx = (uint8_t)uble->RDR;
        uvcp->TDR = rx;
      }
    }
    else
    {
      if(BLE_Link())
      {
        transparent = 1;
        DisableRxIT();
      }
    }
  }
endBleTest:
  bleDeactiv();
  EnableRxIT();
}

void FramWriteCmd(uint8_t *data, uint32_t size)
{
  if(size < 3)
    return;
  uint16_t addr = bigEndianToInt(data, 3);
  uint8_t *buffer = data + 3;
  uint8_t writeBytes = size - 3;
  Fram_PWR_ON();
  fm25xx_writeMultiple(&hspi1, addr, buffer, writeBytes);
  char feedback[64];
  snprintf(feedback, sizeof(feedback), "wrote %d bytes at %d address on fram\n", writeBytes, addr);
  print(feedback);
  Fram_PWR_OFF();
}

void FramReadCmd(uint8_t *data, uint32_t size)
{
  if(size < 6)
    return;
  uint16_t addr = bigEndianToInt(data, 3);
  uint32_t amount = bigEndianToInt(data + 3, 3);
  uint8_t buffer[64];
  Fram_PWR_ON();
  for(uint32_t i = 0; i < (amount/sizeof(buffer)); i++)
  {
    fm25xx_readMultiple(&hspi1, addr, buffer, sizeof(buffer));
    send(buffer, sizeof(buffer));
    addr += sizeof(buffer);
  }
  if((amount % sizeof(buffer)) != 0)
  {
    fm25xx_readMultiple(&hspi1, addr, buffer, amount % sizeof(buffer));
    send(buffer, amount % sizeof(buffer));
  }
  Fram_PWR_OFF();
}

void StartTestTemp(void)
{
  TMP_PWR_ON();
  if(TMP117_OK != tmp117_Init(&hi2c2))
    print("TMP117 Init error\r\n");
  test_temp = 1;
}

void StopTestTemp(void)
{
  test_temp = 0;
  TMP_PWR_OFF();
}

void StartTestBLE(void)
{
  uint8_t err = 0;
  test_ble = 1;
  Bluetooth_PWR_ON();
  BLE_PushKey(KEY_AT_DELAY);
  if(BLE_AT("AT+ROLE=1\r\n") == 1)
  {
    BLE_Reset();
  }
  else
  {
    print("Restore BLE settings\n");
    /*

    BLE_PushKey(KEY_RESTORE_DELAY);
    HAL_Delay(?);
    BLE_RESET_ON;
    HAL_UART_DeInit(&huart1);
    huart1.Init.BaudRate = BLE_BAND_DEFAULT;
    HAL_UART_Init(&huart1);
    BLE_RESET_OFF;
    HAL_Delay(?);
    BLE_PushKey(KEY_AT_DELAY);

    */
    BLE_PushKey(KEY_RESTORE_DELAY);
    HAL_UART_DeInit(&huart1);
    huart1.Init.BaudRate = BLE_BAND_DEFAULT;
    HAL_UART_Init(&huart1);
        HAL_Delay(5000); //which delay should be?!
    BLE_PushKey(KEY_AT_DELAY);
    if(BLE_AT("AT+ROLE=1\r\n") == 1)
    {
      BLE_AT("AT+BAND=921600\r\n");
      BLE_Reset();
      HAL_UART_DeInit(&huart1);
      huart1.Init.BaudRate = 921600;
      HAL_UART_Init(&huart1);
      BLE_PushKey(KEY_AT_DELAY);
      if(BLE_AT("AT+ROLE=1\r\n") == 1)
      {
        if(BLE_AT("AT+TS=1\r\n") == 0)
          err = 3;
      }
      else
      {
        err = 2;
      }
    }
    else
    {
      err = 1;
    }  
  }
  if(err == 0)
  {
    print("BLE ok\n");
    ble_link_state = -1;
    USART1->CR1 |= USART_CR1_RXNEIE;
    ComLink = 2;
  }
  else
  {
    snprintf(str, sizeof(str), "BLE error (code %d)\n", err);
    print(str);
  }
}

void StopTestBLE(void)
{
  Bluetooth_PWR_OFF();
  test_ble = 0;
}



// C0 0B 00
void StartTestAccGyro(void)
{
  test_accgyro = 1;
}

// C0 0B 00
void StopTestAccGyro(void)
{
  test_accgyro = 0;
}



// C0 03 00
void StopTestFlash(void)
{
  Flash_PWR_OFF();
  test_flash = 0;
}

// C0 04 00
void EraseFlash(void)
{
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    P25Qx_QPI_EraseChip(&flash);
    print("erase done\n");
    Flash_PWR_OFF();
  }
}

// C0 05 03 00 00 00
void ReadSectorFlash(uint32_t sec)
{
  uint8_t buf[P25Q_PAGE_SIZE];
  uint32_t addr = sec*P25Q_SECTOR_SIZE;
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    for(uint8_t i = 0; i<(P25Q_SECTOR_SIZE/sizeof(buf)); i++)
    {
      P25Qx_QPI_Read(&flash, addr, sizeof(buf), buf);
      send(buf, sizeof(buf));
      addr += sizeof(buf);
    }
    Flash_PWR_OFF();
  }
}

// C0 06 03 00 00 00
void EraseSector(uint32_t sec)
{
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    P25Qx_QPI_EraseSector(&flash, sec*P25Q_SECTOR_SIZE);
    print("erase done\n");
    Flash_PWR_OFF();
  }
}

// C0 07 04 00 00 00 AA
void WriteSector(uint32_t sec, uint8_t val)
{
  uint8_t buf[P25Q_PAGE_SIZE];
  uint32_t addr = sec*P25Q_SECTOR_SIZE;
  if(test_flash == 0)
  {
    for(uint32_t i=0; i<sizeof(buf); i++)
      buf[i] = val;
    
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    for(uint8_t i = 0; i<(P25Q_SECTOR_SIZE/sizeof(buf)); i++)
    {
      P25Qx_QPI_ProgramPage(&flash, addr, buf, sizeof(buf));
      addr += sizeof(buf);
    }
    Flash_PWR_OFF();
  }
}

// example: C0 08 03 00 00 00
void ReadPage(uint32_t page)
{
  uint8_t buf[P25Q_PAGE_SIZE];
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    P25Qx_QPI_Read(&flash, page*P25Q_PAGE_SIZE, sizeof(buf), buf);
    send(buf, sizeof(buf));
    Flash_PWR_OFF();
  }
}

// example: C0 09 04 00 00 00 AA
void WritePage(uint32_t page, uint8_t val)
{
  uint8_t buf[P25Q_PAGE_SIZE];
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    for(uint32_t i=0; i<sizeof(buf); i++)
      buf[i] = val;
    P25Qx_QPI_ProgramPage(&flash, page*P25Q_PAGE_SIZE, buf, sizeof(buf));
    Flash_PWR_OFF();
  }
}

// example: C0 0A 03 00 00 00
void ErasePage(uint32_t page)
{
  if(test_flash == 0)
  {
    Flash_PWR_ON();
    P25Qx_SetQPI(&flash);
    P25Qx_QPI_ErasePage(&flash, page*P25Q_PAGE_SIZE);
    print("erase done\n");
    Flash_PWR_OFF();
  }
}


