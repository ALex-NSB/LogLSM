/* ---------------------------------------------------------------------------
 * iflash.c — драйвер служебного хранилища во внутренней Flash STM32L433.
 * См. iflash.h и actual/internal_flash_service_storage.md.
 * ------------------------------------------------------------------------- */
#include "iflash.h"
#include "stm32l4xx_hal.h"

#define IFLASH_ERASED_DWORD  0xFFFFFFFFFFFFFFFFULL

/* Внутренняя Flash отображена в адресное пространство — чтение прямое.
 * Стёртое двойное слово читается как все 1 (ECC для стёртого валиден). */
uint64_t iflash_read_dword(uint32_t addr)
{
  return *(volatile uint64_t *)addr;
}

/* Запись одного двойного слова (64 бит). ⚠ Один раз после стирания (ECC). */
int iflash_program_dword(uint32_t addr, uint64_t data)
{
  if (HAL_FLASH_Unlock() != HAL_OK)
    return -1;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, data);
  HAL_FLASH_Lock();
  return (st == HAL_OK) ? 0 : -1;
}

/* Стирание count страниц начиная с page (абсолютный номер 0..127).
 * L433 — одна банка (FLASH_BANK_1). Стёрто = все 1. */
int iflash_erase_pages(uint32_t page, uint32_t count)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t pageErr = 0xFFFFFFFFu;
  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.Banks     = FLASH_BANK_1;
  er.Page      = page;
  er.NbPages   = count;
  if (HAL_FLASH_Unlock() != HAL_OK)
    return -1;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &pageErr);
  HAL_FLASH_Lock();
  return (st == HAL_OK) ? 0 : -1;
}

/* Собрать 8-байтную запись слота:
 * [0]=type [1]=rccMask [2..5]=totalSec(LE) [6..7]=0xFFFF (резерв, «стёрт»). */
static uint64_t jrn_pack(uint8_t type, uint32_t totalSec, uint8_t rccMask)
{
  return  (uint64_t)type
        | ((uint64_t)rccMask   << 8)
        | ((uint64_t)totalSec  << 16)
        | ((uint64_t)0xFFFFULL << 48);
}

/* Записать событие в первый свободный слот журнала. Если журнал полон —
 * молча не пишем (сброс — провижном через iflash_journal_reset). */
void iflash_journal_append(uint8_t type, uint32_t totalSec, uint8_t rccMask)
{
  for (uint32_t i = 0; i < IFLASH_JRN_SLOTS; i++)
  {
    uint32_t addr = IFLASH_JRN_ADDR + i * 8u;
    if (iflash_read_dword(addr) == IFLASH_ERASED_DWORD)
    {
      iflash_program_dword(addr, jrn_pack(type, totalSec, rccMask));
      return;
    }
  }
}

/* Число записанных слотов данного типа. Журнал заполняется подряд, поэтому
 * на первом же стёртом слоте останавливаемся. */
uint16_t iflash_journal_count(uint8_t type)
{
  uint16_t n = 0;
  for (uint32_t i = 0; i < IFLASH_JRN_SLOTS; i++)
  {
    uint64_t rec = iflash_read_dword(IFLASH_JRN_ADDR + i * 8u);
    if (rec == IFLASH_ERASED_DWORD)
      break;
    if ((uint8_t)(rec & 0xFFu) == type)
      n++;
  }
  return n;
}

/* Стереть весь журнал (4 страницы) — провижн перед установкой в поле. */
void iflash_journal_reset(void)
{
  iflash_erase_pages(IFLASH_JRN_PAGE0, IFLASH_JRN_PAGES);
}

/* Гарантировать пригодность журнала к записи. Если первый слот НЕ стёрт
 * (0xFF..FF) и НЕ является настоящим событием (тип 0xF0/0xF1) — страницы
 * содержат мусор (никогда не инициализировались этой прошивкой) → стираем
 * один раз. После этого append/count работают. Валидный (пустой или с
 * событиями) журнал не трогаем. Вызывать на загрузке ДО первого append. */
void iflash_journal_ensure_ready(void)
{
  uint64_t s0 = iflash_read_dword(IFLASH_JRN_ADDR);
  uint8_t  t0 = (uint8_t)(s0 & 0xFFu);
  if (s0 != IFLASH_ERASED_DWORD && t0 != EVT_POWERLOSS && t0 != EVT_WATCHDOG)
    iflash_journal_reset();
}
