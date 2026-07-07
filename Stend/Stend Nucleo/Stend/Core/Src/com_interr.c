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
static uint8_t RX_Relay;
//static uint8_t cmdReceived;
static uint8_t txBuffer[256];
//static uint32_t txBytes;

static uint8_t start;
static uint32_t lenPacket = 0;
static UART_HandleTypeDef *uartRelay = NULL;
static uint8_t slaveAddr;

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;

RingBuffer u2TxBuff;
RingBuffer u3TxBuff;

void ComInit()
{
  start = 0;
  WakeProtocolInit(&wc);
  HAL_UART_Receive_IT(&huart2, &RX, 1);
  RingInit(&u2TxBuff);
  RingInit(&u3TxBuff);
}

void ComPoll()
{
  if(wc.packet_recognized)
  {
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
    /*uint32_t c = WakeProtocolBuildPacket(WAKE_OWN_ADDR, wc.cmd, wc.data, wc.n, txBuffer);
    HAL_UART_Transmit(&huart2, txBuffer, c, 100);*/
    wc.packet_recognized = 0;
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
  switch(slaveAddr)
  {
  default:
    return &huart3;     // RVZD
  };
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if(huart == &huart2)
  {
    if(!RingIsEmpty(u2TxBuff))
    {
      HAL_UART_Transmit_IT();
    }
  }
  else if(huart == &huart3)
  {
    
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *UartHandle)
{
  if(UartHandle == &huart2)
  {
    if(RX == 0xC0)
    {
      lenPacket = 1;
    }
    else if(lenPacket > 0)
    {
      lenPacket++;
      
      if(lenPacket == 2)
      {
        slaveAddr = RX;
        uint8_t fend = 0xC0;
        if(slaveAddr != WAKE_OWN_ADDR)
        {
          UART_HandleTypeDef *uart = getUartBySlaveAddr(slaveAddr);
          if(uart != uartRelay)
          {
            if(uartRelay != NULL)
              HAL_UART_AbortReceive_IT(uartRelay);
            HAL_UART_Receive_IT(uart, &RX_Relay, 1);
            uartRelay = uart;
          }
          //HAL_UART_Transmit(uartRelay, &fend, 1, 1);
          //HAL_UART_Transmit_IT(uartRelay, &fend, 1);
          //RingPut(u3TxBuff, fend);
          SendUart(uartRelay, fend);
        }
        else
        {
          if(uartRelay != NULL)
          {
            HAL_UART_AbortReceive_IT(uartRelay);
            uartRelay = NULL;
          }
          WakeProtocolParse(&wc, fend);
        }
      }
      if(uartRelay != NULL)
      {
        //HAL_UART_Transmit(uartRelay, &RX, 1, 1);
        //HAL_UART_Transmit_IT(uartRelay, &RX, 1);
        RingPut(u3TxBuff, RX);
        
      }
      else
      {
        WakeProtocolParse(&wc, RX);
      }
    }
    HAL_UART_Receive_IT(&huart2, &RX, 1);
  }
  else
  {
    //HAL_UART_Transmit(&huart2, &RX_Relay, 1, 1);
    HAL_UART_Transmit_IT(&huart2, &RX_Relay, 1);
    HAL_UART_Receive_IT(UartHandle, &RX_Relay, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if(huart == uartRelay)
  {
    HAL_UART_Receive_IT(huart, &RX_Relay, 1);
  }
  else if(huart == &huart2)
  {
    HAL_UART_Receive_IT(huart, &RX, 1);
  }
}
