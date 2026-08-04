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
  if (s0 != IFLASH_ERASED_DWORD && t0 != EVT_POWERLOSS && t0 != EVT_WATCHDOG
      && t0 != EVT_CLOCKZERO)
    iflash_journal_reset();
}

/* ===========================================================================
 * Журнал активаций (Page C, стр.121) — история «жизней» прибора.
 * Слот 8 байт: [0]=EVT_ACTIVATION [1]=0xFF(рез.) [2..5]=ts(LE) [6..7]=0xFFFF.
 * Append-only, писать ТОЛЬКО под кабелем (Сервис). Читается один раз на старте.
 * ------------------------------------------------------------------------- */

/* Собрать слот события: [0]=тип [1]=0xFF [2..5]=ts(LE) [6]=счётчик рестартов по
 * ТАЙМЕРУ (WATCHDOG) [7]=по ПИТАНИЮ (POWERLOSS) на момент события (общие счётчики
 * журнала стр.124..127, клип 255). Разница END−START = перезапуски за «жизнь»;
 * панель «за цикл» = текущий счётчик − счётчик на момент активации. */
static uint64_t act_pack(uint8_t type, uint32_t ts, uint8_t timer, uint8_t power)
{
  return  (uint64_t)type
        | ((uint64_t)0xFFu    << 8)
        | ((uint64_t)ts       << 16)
        | ((uint64_t)timer    << 48)
        | ((uint64_t)power    << 56);
}

/* Дописать событие в первый свободный слот. Полный журнал — молча не пишем. */
static void act_append(uint8_t type, uint32_t ts)
{
  uint16_t tw = iflash_journal_count(EVT_WATCHDOG);
  uint16_t tp = iflash_journal_count(EVT_POWERLOSS);
  uint8_t timer = (tw > 255u) ? 255u : (uint8_t)tw;
  uint8_t power = (tp > 255u) ? 255u : (uint8_t)tp;
  for (uint32_t i = 0; i < IFLASH_ACT_SLOTS; i++)
  {
    uint32_t addr = IFLASH_ACT_ADDR + i * 8u;
    if (iflash_read_dword(addr) == IFLASH_ERASED_DWORD)
    {
      iflash_program_dword(addr, act_pack(type, ts, timer, power));
      return;
    }
  }
}

/* Начало жизни (активация). */
void iflash_activation_append(uint32_t ts) { act_append(EVT_ACTIVATION, ts); }
/* Конец жизни (сохранение/снятие с активации). */
void iflash_activation_end(uint32_t ts)    { act_append(EVT_ACT_END, ts); }

/* ts ОТКРЫТОЙ активации: смотрим ПОСЛЕДНЕЕ событие журнала. Если оно
 * EVT_ACTIVATION — жизнь открыта, отдаём его ts; если EVT_ACT_END или журнал
 * пуст — 0xFFFFFFFF (не активирован сейчас). */
uint32_t iflash_activation_last(void)
{
  uint32_t lastTs = 0xFFFFFFFFu;
  uint8_t  lastType = 0;
  for (uint32_t i = 0; i < IFLASH_ACT_SLOTS; i++)
  {
    uint64_t rec = iflash_read_dword(IFLASH_ACT_ADDR + i * 8u);
    if (rec == IFLASH_ERASED_DWORD)
      break;
    lastType = (uint8_t)(rec & 0xFFu);
    lastTs   = (uint32_t)(rec >> 16);      /* байты 2..5 = ts */
  }
  return (lastType == EVT_ACTIVATION) ? lastTs : 0xFFFFFFFFu;
}

/* Число активаций («жизней») в журнале. */
uint16_t iflash_activation_count(void)
{
  uint16_t n = 0;
  for (uint32_t i = 0; i < IFLASH_ACT_SLOTS; i++)
  {
    uint64_t rec = iflash_read_dword(IFLASH_ACT_ADDR + i * 8u);
    if (rec == IFLASH_ERASED_DWORD)
      break;
    if ((uint8_t)(rec & 0xFFu) == EVT_ACTIVATION)
      n++;
  }
  return n;
}

/* Выгрузить ts активаций по порядку в out (не более max). Возврат — число. */
uint16_t iflash_activation_list(uint32_t *out, uint16_t max)
{
  uint16_t n = 0;
  for (uint32_t i = 0; i < IFLASH_ACT_SLOTS && n < max; i++)
  {
    uint64_t rec = iflash_read_dword(IFLASH_ACT_ADDR + i * 8u);
    if (rec == IFLASH_ERASED_DWORD)
      break;
    if ((uint8_t)(rec & 0xFFu) == EVT_ACTIVATION)
      out[n++] = (uint32_t)(rec >> 16);
  }
  return n;
}

/* Выгрузить ВСЕ события журнала (начала и концы) по порядку, с рестартами. */
uint16_t iflash_activation_events(uint8_t *types, uint32_t *ts,
                                  uint16_t *restarts, uint16_t max)
{
  uint16_t n = 0;
  for (uint32_t i = 0; i < IFLASH_ACT_SLOTS && n < max; i++)
  {
    uint64_t rec = iflash_read_dword(IFLASH_ACT_ADDR + i * 8u);
    if (rec == IFLASH_ERASED_DWORD)
      break;
    uint8_t t = (uint8_t)(rec & 0xFFu);
    if (t == EVT_ACTIVATION || t == EVT_ACT_END)
    {
      types[n]    = t;
      ts[n]       = (uint32_t)(rec >> 16);       /* байты 2..5 */
      restarts[n] = (uint16_t)(rec >> 48);       /* байты 6..7 */
      n++;
    }
  }
  return n;
}

/* Стереть журнал активаций (стр.121) → «не активирован». */
void iflash_activation_reset(void)
{
  iflash_erase_pages(IFLASH_ACT_PAGE, IFLASH_ACT_PAGES);
}

/* Гарантировать пригодность журнала активаций к записи. Первый слот не стёрт
 * и не EVT_ACTIVATION → мусор → стереть один раз. Вызывать на старте ДО last. */
void iflash_activation_ensure_ready(void)
{
  uint64_t s0 = iflash_read_dword(IFLASH_ACT_ADDR);
  uint8_t  t0 = (uint8_t)(s0 & 0xFFu);
  if (s0 != IFLASH_ERASED_DWORD && t0 != EVT_ACTIVATION)
    iflash_activation_reset();
}

/* «Очистить журналы» (provision): СЛЕПОЕ стирание — сначала стр.121, затем
 * 124..127, без предварительного чтения (лечит любой битый ECC на легаси-121).
 * Стр.123 (заводское) НЕ трогается. */
void iflash_journals_clear_all(void)
{
  iflash_erase_pages(IFLASH_ACT_PAGE,  IFLASH_ACT_PAGES);   /* активации  (121)     */
  iflash_erase_pages(IFLASH_JRN_PAGE0, IFLASH_JRN_PAGES);   /* рестарты   (124..127)*/
}

/* ===========================================================================
 * Заводская конфигурация (Page B, стр.123). Образ пишется целиком.
 * ------------------------------------------------------------------------- */
#include <string.h>

/* Прочитать конфиг: memory-mapped чтение образа стр.123. Валиден при совпадении
 * magic. Стёртая (0xFF) или чужая страница → -1 (вызывающий берёт дефолт). */
int iflash_cfg_read(IflashCfg *out)
{
  const IflashCfg *p = (const IflashCfg *)IFLASH_CFG_ADDR;
  if (p->magic != IFLASH_CFG_MAGIC)
    return -1;
  memcpy(out, p, sizeof(IflashCfg));
  return 0;
}

/* Записать конфиг: стереть стр.123 и запрограммировать образ по double-word.
 * Размер IflashCfg кратен 8. Возврат 0=ok, -1=ошибка стирания/записи. */
int iflash_cfg_write(const IflashCfg *in)
{
  if (iflash_erase_pages(IFLASH_CFG_PAGE, 1) != 0)
    return -1;
  const uint8_t *src = (const uint8_t *)in;
  for (uint32_t off = 0; off < sizeof(IflashCfg); off += 8u)
  {
    uint64_t dw;
    memcpy(&dw, src + off, 8);
    if (iflash_program_dword(IFLASH_CFG_ADDR + off, dw) != 0)
      return -1;
  }
  return 0;
}
