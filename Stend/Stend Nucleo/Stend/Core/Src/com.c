#include "com.h"
#include "wake.h"
#include "StepDriver.h"
#include "ring_buffer.h"
                                        // args (little-endian)
#define CMD_START       0x04            // NULL
#define CMD_STOP        0x06            // NULL
#define CMD_SPEED       0x05            // speed(uint16) coefficient(uint8)

void cmdStartHandler();
void cmdStopHandler();
void cmdSpeedHandler();

static WakeContext wc;
static uint8_t RX;
static uint8_t regRX;
//static uint8_t cmdReceived;
static uint8_t txBuffer[512];
uint32_t TxLen;
//static uint32_t txBytes;

static uint8_t start;

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;

static UART_HandleTypeDef *comUart = &huart2;
static UART_HandleTypeDef *regUart = &huart3;

uint8_t RxBuffer[512];
uint32_t RxLen;

void ComInit()
{
  start = 0;
  TxLen = RxLen = 0;
  WakeProtocolInit(&wc);
  HAL_UART_Receive_IT(comUart, &RX, 1);
  HAL_UARTEx_ReceiveToIdle_IT(regUart, txBuffer, sizeof(txBuffer));
}

void ComPoll()
{
  if(wc.packet_recognized)
  {
    wc.packet_recognized = 0;
    //HAL_UART_Transmit(comUart, RxBuffer, RxLen, 1000);
    
    if(wc.addr != WAKE_OWN_ADDR)
    {
      HAL_UART_Transmit(regUart, RxBuffer, RxLen, 1000);
      RxLen = 0;
      return;
    }
    switch(wc.cmd)
    {
    case CMD_START:
      cmdStartHandler();
      break;
    case CMD_STOP:
      cmdStopHandler();
      break;
    case CMD_SPEED:
      cmdSpeedHandler();
      break;
    default:
      ;
    };
    RxLen = 0;
    /*uint32_t c = WakeProtocolBuildPacket(WAKE_OWN_ADDR, wc.cmd, wc.data, wc.n, txBuffer);
    HAL_UART_Transmit(&huart2, txBuffer, c, 100);*/
  }
}

void cmdStartHandler()
{
  start = 1;
}

void cmdStopHandler()
{
  start = 0;
  StepDriverSetSpeed(0, 0);
}

void cmdSpeedHandler()
{
  uint16_t *aSpeed;
  uint8_t *aCoef;
  
  if(wc.cmd == CMD_SPEED)       // Проверяем что команда соответствует обработчику
  {
      // Проверяем что была команда СТАРТ
      // Если нет, то игнорируем команду
      if(!start)
        return;
      
      // Извлекаем аргументы
      aSpeed = (uint16_t*)&wc.data[0];
      aCoef = (uint8_t*)&wc.data[2];
      
      // Проверяем корректность аргументов
      //if(*aSpeed > MAX_SPEED_VALUE)
      //{
      //  sendErr(errInvalArgs);
      //  return;
      //}
      
      // Выполняем команду
      StepDriverSetSpeed(*aSpeed, (1 << *aCoef));
  }
}

UART_HandleTypeDef* getUartBySlaveAddr(uint8_t slaveAddr)
{
  
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *UartHandle)
{
  if(UartHandle == comUart)
  {
    RxBuffer[RxLen++] = RX;
    WakeProtocolParse(&wc, RX);
    HAL_UART_Receive_IT(comUart, &RX, 1);
  }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if(huart == regUart)
  {
    HAL_UART_Transmit_IT(comUart, txBuffer, Size);
    HAL_UARTEx_ReceiveToIdle_IT(regUart, txBuffer, sizeof(txBuffer));
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
 
}
