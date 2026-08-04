#include "p25q128.h"

#define ManufactDeviceID_CMD	        0x9f
#define REMS_CMD	                0x90
#define READ_STATU_REGISTER_1           0x05
#define READ_STATU_REGISTER_2           0x35
#define WRITE_STATU_REGISTER_1          0x01
#define WRITE_STATU_REGISTER_2          0x31
//#define READ_SPI_DATA_CMD	        0x03
#define READ_DATA_CMD	                0x0B
#define WRITE_ENABLE_CMD	        0x06
#define WRITE_DISABLE_CMD	        0x04
#define PAGE_ERASE_CMD	                0x81
#define CHIP_ERASE_CMD0	                0xc7
#define CHIP_ERASE_CMD1	                0x60
#define SECTOR_ERASE_CMD		0x20
#define PAGE_PROGRAM_CMD                0x02
#define ENABLE_QPI	                0x38
#define DISABLE_QPI	                0xff

HAL_StatusTypeDef QSPI_Send_Command(P25Qx_HandleTypeDef *handle,
                                    uint32_t instruction, 
                                    uint32_t address, 
                                    uint32_t dummyCycles, 
                                    uint32_t instructionMode, 
                                    uint32_t addressMode, 
                                    uint32_t addressSize, 
                                    uint32_t dataMode)
{
    QSPI_CommandTypeDef cmd;
    cmd.Instruction = instruction;                 	        // инструкция
    cmd.Address = address;                                      //адрес
    cmd.DummyCycles = dummyCycles;                              // Устанавливаем количество пустых командных циклов
    cmd.InstructionMode = instructionMode;			// Командный режим
    cmd.AddressMode = addressMode;   				// Адресный режим
    cmd.AddressSize = addressSize;   				// Длина адреса
    cmd.DataMode = dataMode;             			// Режим данных
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;       	        // Отправлять инструкции каждый раз
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;          // Нет чередующихся байтов
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;           	        // Закройте режим DDR
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    return HAL_QSPI_Command(handle->hqspi, &cmd, 500);
}

HAL_StatusTypeDef QSPI_Receive(P25Qx_HandleTypeDef *handle, uint8_t* recv_buf, uint32_t size)
{
    handle->hqspi->Instance->DLR = size - 1;                                            // Длина данных конфигурации
    return HAL_QSPI_Receive(handle->hqspi, recv_buf, 500);		                // Получение данных
}

uint8_t P25Qx_ReadSR(P25Qx_HandleTypeDef *handle, uint8_t reg)
{
    uint8_t cmd = 0, result = 0;	
    switch(reg)
    {
      case 1:
        cmd = READ_STATU_REGISTER_1;
        break;
      case 2:
        cmd = READ_STATU_REGISTER_2;
        break;
      default:
        cmd = READ_STATU_REGISTER_1;
    }
    if(handle->mode != qpi_mode)
      QSPI_Send_Command(handle, cmd, 0, 0, QSPI_INSTRUCTION_1_LINE, QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS, QSPI_DATA_1_LINE);
    else
      QSPI_Send_Command(handle, cmd, 0, 0, QSPI_INSTRUCTION_4_LINES, QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS, QSPI_DATA_4_LINES);
    QSPI_Receive(handle, &result, 1);
    return result;
}

void Wait_Busy(P25Qx_HandleTypeDef *handle)
{
    /* ЗАЩИТА ОТ ВЕЧНОГО СПИНА (03.08.2026): если флеш вернул мусор в SR (не
     * запитан / не в том режиме / QPI-рассинхрон), WIP читается как 1 всегда и
     * прежний бесконечный while() ВЕШАЛ МК целиком — переставали отвечать даже
     * команды, не трогающие флеш (0x1b и т.п.). Ограничиваем ожидание: реальная
     * запись байта/страницы <1 мс, стирание сектора ~мс — 1 с с запасом; за это
     * время либо WIP снимется, либо флеш недоступен и дальше бессмысленно ждать. */
    uint32_t t0 = HAL_GetTick();
    while((P25Qx_ReadSR(handle, 1) & 0x01) == 0x01)
    {
        if ((HAL_GetTick() - t0) > 1000u)
            break;
    }
}

/* Программный сброс чипа из ЛЮБОГО состояния (SPI или QPI) — самовосстановление
 * без обесточивания (04.07.2026, жёсткое требование пользователя). После
 * SWD-прошивки NRST не сбрасывает Flash-чип: он может остаться в QPI → первый
 * же SPI-опрос (ReadSR ниже) читает мусор / виснет → прошивка не отвечает
 * («команды не проходят», лечилось только питанием). Шлём Reset-Enable(0x66)+
 * Reset(0x99) СНАЧАЛА в QPI (4-line — если чип в QPI), затем в SPI (1-line —
 * если в SPI). Только команды, без чтения данных — не виснет в любом режиме.
 * После сброса чип в заводском дефолте (SPI). */
void P25Qx_Reset(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command(handle, 0x66, 0, 0, QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS, QSPI_DATA_NONE);
  QSPI_Send_Command(handle, 0x99, 0, 0, QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS, QSPI_DATA_NONE);
  HAL_Delay(1);   /* tRST — время выйти из сброса */
  QSPI_Send_Command(handle, 0x66, 0, 0, QSPI_INSTRUCTION_1_LINE,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS, QSPI_DATA_NONE);
  QSPI_Send_Command(handle, 0x99, 0, 0, QSPI_INSTRUCTION_1_LINE,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS, QSPI_DATA_NONE);
  HAL_Delay(1);
  handle->mode = standart_mode;
}

void P25Qx_Init(P25Qx_HandleTypeDef *handle)
{
  /* Самовосстановление без питания (04.07.2026, 2-я, ПРАВИЛЬНАЯ попытка).
   * После SWD-прошивки чип может остаться в QPI (NRST его не сбрасывает) →
   * SPI-опрос SR2 читает мусор, SetQPI не поднимает QPI → «нет ответа Flash».
   * Последовательность:
   *   1) P25Qx_Reset — 0x66/0x99, выйти из QPI в SPI-дефолт;
   *   2) P25Qx_SetQuadSpi — ЗАНОВО включить QE (сброс мог его снять). ЭТОГО
   *      шага не хватало в 1-й попытке → SetQPI (pwr.c) не входил в QPI.
   * Далее pwr.c зовёт P25Qx_SetQPI — и чип корректно в QPI, чтения проходят. */
  P25Qx_Reset(handle);        /* 1) выйти из возможного QPI-залипания */
  P25Qx_SetQuadSpi(handle);   /* 2) вернуть QE (нужно для входа в QPI) */
  handle->state = P25QX_READY;
}

void P25Qx_DeInit(P25Qx_HandleTypeDef *handle)
{
  handle->state = P25QX_DEINIT;
}

void P25Qx_SetQuadSpi(P25Qx_HandleTypeDef *handle)
{
  uint8_t r = P25Qx_ReadSR(handle, 2);
  if((r & 0x02) == 0)
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
            QSPI_INSTRUCTION_1_LINE,
            QSPI_ADDRESS_NONE,
            QSPI_ADDRESS_24_BITS,
            QSPI_DATA_NONE);
    
    QSPI_Send_Command(handle, WRITE_STATU_REGISTER_2, 0, 0,
            QSPI_INSTRUCTION_1_LINE,
            QSPI_ADDRESS_NONE,
            QSPI_ADDRESS_24_BITS,
            QSPI_DATA_1_LINE);
    
    handle->hqspi->Instance->DLR = 0;
    r=2;
    HAL_QSPI_Transmit(handle->hqspi, &r, 500);
    Wait_Busy(handle);
  }
  handle->mode = quadspi_mode;
}

/*void P25Qx_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  QSPI_Send_Command(handle, SECTOR_ERASE_CMD, addr, 0,
                    QSPI_INSTRUCTION_1_LINE,
                    QSPI_ADDRESS_1_LINE, QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_NONE);
  Wait_Busy(handle);
}*/

void P25Qx_SetQPI(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command( handle,
                     ENABLE_QPI,
                     0,                                    //адрес
                     0,                                    //dummyCycles
                     QSPI_INSTRUCTION_1_LINE,              //instructionMode
                     QSPI_ADDRESS_NONE,                    // addressMode, 
                     QSPI_ADDRESS_24_BITS,                 // addressSize, 
                     QSPI_DATA_NONE);                      // dataMode
  HAL_Delay(2);
  
  /*uint8_t param = (1 << 4);
  QSPI_Send_Command(handle, 0xC0, 0, 0,
        QSPI_INSTRUCTION_4_LINES,
        QSPI_ADDRESS_NONE,
        0,
        QSPI_DATA_4_LINES);
  handle->hqspi->Instance->DLR = 8 - 1;
  HAL_QSPI_Transmit(handle->hqspi, &param, 500);*/
  
  handle->mode = qpi_mode;
}

void P25Qx_QPI_DisableQPI(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command(handle, DISABLE_QPI, 0, 0,
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_24_BITS,
                    QSPI_DATA_NONE);
  handle->mode = quadspi_mode;
}

void P25Qx_QPI_EnableWrite(P25Qx_HandleTypeDef *handle)
{
  if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_NONE, 0,
                      QSPI_DATA_NONE);
  } 
  else
  {
    QSPI_Send_Command(handle, WRITE_ENABLE_CMD, 0, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_NONE, 0,
                      QSPI_DATA_NONE);
  }
  Wait_Busy(handle);
}

/*
void P25Qx_QPI_DisableWrite(P25Qx_HandleTypeDef *handle)
{
  QSPI_Send_Command(handle, WRITE_DISABLE_CMD, 0, 0,
                    QSPI_INSTRUCTION_4_LINES,
                    QSPI_ADDRESS_NONE, QSPI_ADDRESS_8_BITS,
                    QSPI_DATA_NONE);
  Wait_Busy(handle);
}*/

void P25Qx_QPI_ProgramPage(P25Qx_HandleTypeDef *handle, uint32_t addr, uint8_t *data, uint16_t size)
{
  P25Qx_QPI_EnableWrite(handle);
  if(handle->mode == standart_mode)
  {
    QSPI_Send_Command(handle, 0x02, addr, 0,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_1_LINE,
                  QSPI_ADDRESS_24_BITS,
                  QSPI_DATA_1_LINE);
  }
  else if(handle->mode == quadspi_mode)
  {   
    QSPI_Send_Command(handle, 0x32, addr, 0,
              QSPI_INSTRUCTION_1_LINE,
              QSPI_ADDRESS_1_LINE,
              QSPI_ADDRESS_24_BITS,
              QSPI_DATA_4_LINES);
  }
  else if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, 0x02, addr, 0,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_4_LINES,
          QSPI_ADDRESS_24_BITS,
          QSPI_DATA_4_LINES);
  }
  else
  {
    return;
  }
  handle->hqspi->Instance->DLR = size - 1;
  HAL_QSPI_Transmit(handle->hqspi, data, 500);
  Wait_Busy(handle);
}

void P25Qx_QPI_ErasePage(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  P25Qx_QPI_EnableWrite(handle);
  
  if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, PAGE_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_4_LINES, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  else
  {
    QSPI_Send_Command(handle, PAGE_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_1_LINE, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  Wait_Busy(handle);
}

void P25Qx_QPI_EraseSector(P25Qx_HandleTypeDef *handle, uint32_t addr)
{
  P25Qx_QPI_EnableWrite(handle);
  /* 22.07.2026: раньше здесь БЕЗ ПРОВЕРКИ handle->mode жёстко слались 4 линии
   * (инструкция+адрес) — работало только пока режим всегда был qpi/quadspi.
   * После появления ручного переключения на обычный SPI (0x2B, LOGLSMW)
   * команда стала уходить неверной длины линий → чип её не распознавал.
   * Правило то же, что в ErasePage/EraseChip: 4 линии только в qpi_mode. */
  if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, SECTOR_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_4_LINES, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  else
  {
    QSPI_Send_Command(handle, SECTOR_ERASE_CMD, addr, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_1_LINE, QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_NONE);
  }
  Wait_Busy(handle);
}

void P25Qx_QPI_EraseChip(P25Qx_HandleTypeDef *handle)
{
  P25Qx_QPI_EnableWrite(handle);
  if((handle->mode == standart_mode) || (handle->mode == quadspi_mode))
  {
    QSPI_Send_Command(handle, 0x60, 0, 0,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_NONE,
                  0,
                  QSPI_DATA_NONE);
  }
  else if(handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, 0x60, 0, 0,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_NONE,
          0,
          QSPI_DATA_NONE);
  }
  else
  {
    return;
  }
  //Wait_Busy(handle);
}

void P25Qx_QPI_Read(P25Qx_HandleTypeDef *handle, uint32_t addr, uint16_t len, uint8_t *data)
{
  if(handle->mode == standart_mode)
  {
    QSPI_Send_Command(handle, 0x0B, addr,
                  8,
                  QSPI_INSTRUCTION_1_LINE,
                  QSPI_ADDRESS_1_LINE,
                  QSPI_ADDRESS_24_BITS,
                  QSPI_DATA_1_LINE);
  }
  else if(handle->mode == quadspi_mode)
  {
    QSPI_Send_Command(handle, 0x6B, addr,
              8,
              QSPI_INSTRUCTION_1_LINE,
              QSPI_ADDRESS_1_LINE,
              QSPI_ADDRESS_24_BITS,
              QSPI_DATA_4_LINES);
  }
  else if(handle->mode == qpi_mode)
  {
    /* ОТКАЧЕНО обратно на 10, 02.07.2026 вечером. По ходу диагностики этот
     * dummy менялся дважды (10→8 по догадке, затем 8→4 по датащиту
     * Components/P25Q128H (2023).pdf §10.22 — таблица P5-P4 говорит, что
     * заводской дефолт после сброса/входа в QPI это 4, не 8 и не 10), но ни
     * одно из значений не изменило симптом (FLASH_READ 0x07 не отвечает
     * вообще, а не отвечает мусором — что для чисто фазового сдвига
     * дискретизации нехарактерно). Пользователь подтвердил: этот же файл
     * p25q128.c (побайтово идентичный, см. rp_device/Core/Src/p25q128.c)
     * реально проверялся на этом железе ДО перехода на LTP — запись байтов
     * в область Flash, затем чтение той же области, сверка — совпадало,
     * именно с dummy=10 в этой ветке. Это прямое свидетельство, что
     * dummy-циклы не были и не являются причиной текущего бага — баг искать
     * в LTP-слое (com.c, ltp.c, sendPacket), появившемся поверх этого же
     * Flash-драйвера при миграции с WAKE, а не здесь. Возврат на 10 —
     * просто чтобы не плодить необоснованные отличия от подтверждённо
     * рабочей версии, пока не найдена реальная причина. */
    QSPI_Send_Command(handle, 0x0B, addr,
          10,
          QSPI_INSTRUCTION_4_LINES,
          QSPI_ADDRESS_4_LINES,
          QSPI_ADDRESS_24_BITS,
          QSPI_DATA_4_LINES);
  }
  else
  {
    return;
  }
  QSPI_Receive(handle, data, len);
}

/* Перевод QUADSPI в memory-mapped: параметры чтения = те же, что в
 * P25Qx_QPI_Read по режимам (0x0B/dummy10/4 линии для QPI, 0x6B/dummy8 для
 * quadspi, 0x0B/dummy8/1 линия для standart). После этого флеш читается как
 * память по P25Q_MMAP_BASE. */
void P25Qx_MemMapped(P25Qx_HandleTypeDef *handle)
{
  QSPI_CommandTypeDef cmd = {0};
  QSPI_MemoryMappedTypeDef mm = {0};

  cmd.Instruction       = 0x0B;
  cmd.Address           = 0;
  cmd.AddressSize       = QSPI_ADDRESS_24_BITS;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
  cmd.DdrHoldHalfCycle  = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

  if (handle->mode == qpi_mode)
  {
    cmd.InstructionMode = QSPI_INSTRUCTION_4_LINES;
    cmd.AddressMode     = QSPI_ADDRESS_4_LINES;
    cmd.DataMode        = QSPI_DATA_4_LINES;
    cmd.DummyCycles     = 10;
  }
  else if (handle->mode == quadspi_mode)
  {
    cmd.Instruction     = 0x6B;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode     = QSPI_ADDRESS_1_LINE;
    cmd.DataMode        = QSPI_DATA_4_LINES;
    cmd.DummyCycles     = 8;
  }
  else
  {
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode     = QSPI_ADDRESS_1_LINE;
    cmd.DataMode        = QSPI_DATA_1_LINE;
    cmd.DummyCycles     = 8;
  }

  mm.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
  mm.TimeOutPeriod     = 0;
  HAL_QSPI_MemoryMapped(handle->hqspi, &cmd, &mm);
}

/* Выход из memory-mapped обратно в indirect (иначе обычные команды не пройдут). */
void P25Qx_ExitMemMapped(P25Qx_HandleTypeDef *handle)
{
  HAL_QSPI_Abort(handle->hqspi);
}

uint32_t P25Qx_QPI_ReadID(P25Qx_HandleTypeDef *handle)
{
  uint32_t id;
  /* 22.07.2026: та же правка, что в EraseSector — проверка handle->mode. */
  if (handle->mode == qpi_mode)
  {
    QSPI_Send_Command(handle, REMS_CMD, 0, 0,
                      QSPI_INSTRUCTION_4_LINES,
                      QSPI_ADDRESS_4_LINES,
                      QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_4_LINES);
  }
  else
  {
    QSPI_Send_Command(handle, REMS_CMD, 0, 0,
                      QSPI_INSTRUCTION_1_LINE,
                      QSPI_ADDRESS_1_LINE,
                      QSPI_ADDRESS_24_BITS,
                      QSPI_DATA_1_LINE);
  }
  QSPI_Receive(handle, (uint8_t*)&id, 3);
  return id;
}
