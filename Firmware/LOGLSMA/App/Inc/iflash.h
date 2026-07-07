#ifndef IFLASH_H
#define IFLASH_H
/* ---------------------------------------------------------------------------
 * iflash — служебное хранилище во ВНУТРЕННЕЙ Flash STM32L433.
 * Дизайн: actual/internal_flash_service_storage.md (06.07.2026).
 * Назначение: данные, переживающие потерю питания (батарейка в зажиме +
 * вибрация оси колеса → кратковременные разрывы), доступные сразу на загрузке
 * без инициализации внешнего QSPI-чипа.
 *
 * ⚠ L4-нюанс: Flash с ECC → каждое двойное слово (8 байт) пишется РОВНО ОДИН
 * раз после стирания. Поэтому событие журнала = отдельный 8-байтный слот
 * (битовый трюк «догасить бит в том же байте» на L4 НЕ работает).
 * ------------------------------------------------------------------------- */
#include <stdint.h>

/* --- Раскладка (256 КБ Flash, страница 2 КБ, прошивка ~31 стр., верх свободен) */
#define IFLASH_PAGE_SIZE   2048u
#define IFLASH_BASE        0x08000000u
#define IFLASH_ADDR(page)  (IFLASH_BASE + (uint32_t)(page) * IFLASH_PAGE_SIZE)

#define IFLASH_CFG_PAGE    123u                 /* Page B: паспорт + конфиг (1 стр.) */
#define IFLASH_JRN_PAGE0   124u                 /* Page A: журнал событий... */
#define IFLASH_JRN_PAGES   4u                   /* ...4 стр. = 8 КБ = 1024 слота */
#define IFLASH_CFG_ADDR    IFLASH_ADDR(IFLASH_CFG_PAGE)
#define IFLASH_JRN_ADDR    IFLASH_ADDR(IFLASH_JRN_PAGE0)
#define IFLASH_JRN_SLOTS   ((IFLASH_JRN_PAGES * IFLASH_PAGE_SIZE) / 8u)   /* 1024 */

/* --- Типы событий журнала (младший байт слота) --- */
#define EVT_POWERLOSS      0xF0u   /* потеря питания (POR/BOR / backup-домен стёрт) */
#define EVT_WATCHDOG       0xF1u   /* watchdog-рестарт (IWDG сбросил зависший МК) */

/* --- Низкоуровневый драйвер (double-word = 8 байт) --- */
uint64_t iflash_read_dword(uint32_t addr);
int      iflash_program_dword(uint32_t addr, uint64_t data);  /* 0=ok, -1=ошибка */
int      iflash_erase_pages(uint32_t page, uint32_t count);   /* 0=ok, -1=ошибка */

/* --- Журнал событий (Page A) --- */
/* Записать событие: type + маска RCC-флагов + totalSec (где по наработке рвануло).
 * Слот 8 байт: [0]=type [1]=rccMask [2..5]=totalSec(LE) [6..7]=резерв(0xFFFF). */
void     iflash_journal_append(uint8_t type, uint32_t totalSec, uint8_t rccMask);
/* Число записанных слотов данного типа (скан журнала). Для GET_STATS. */
uint16_t iflash_journal_count(uint8_t type);
/* Стереть весь журнал (провижн перед работой — обнулить полевые счётчики). */
void     iflash_journal_reset(void);
/* Гарантировать пригодность журнала к записи: если страницы содержат мусор
 * (никогда не стирались, не 0xFF и не валидное событие) — стереть один раз.
 * Вызывать на загрузке ДО первого append. */
void     iflash_journal_ensure_ready(void);

#endif /* IFLASH_H */
