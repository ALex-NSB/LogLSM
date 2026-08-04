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

#define IFLASH_ACT_PAGE    121u                 /* Page C: журнал активаций («жизни») */
#define IFLASH_ACT_PAGES   1u                   /* 1 стр. = 2 КБ = 256 слотов */
#define IFLASH_CFG_PAGE    123u                 /* Page B: паспорт + конфиг (1 стр.) */
#define IFLASH_JRN_PAGE0   124u                 /* Page A: журнал событий... */
#define IFLASH_JRN_PAGES   4u                   /* ...4 стр. = 8 КБ = 1024 слота */
#define IFLASH_ACT_ADDR    IFLASH_ADDR(IFLASH_ACT_PAGE)
#define IFLASH_CFG_ADDR    IFLASH_ADDR(IFLASH_CFG_PAGE)
#define IFLASH_JRN_ADDR    IFLASH_ADDR(IFLASH_JRN_PAGE0)
#define IFLASH_ACT_SLOTS   ((IFLASH_ACT_PAGES * IFLASH_PAGE_SIZE) / 8u)   /* 256 */
#define IFLASH_JRN_SLOTS   ((IFLASH_JRN_PAGES * IFLASH_PAGE_SIZE) / 8u)   /* 1024 */

/* ⚠ РАСКЛАДКА (три РАЗДЕЛЬНЫЕ области, стираются РАЗНЫМИ командами):
 *   стр.121  журнал активаций (история «жизней», ts)  → «Очистить журналы»
 *   стр.122  флаг «несохранённые данные» (DATAFLAG, com.c) → поток стирания NOR
 *   стр.123  заводское: паспорт/калибровки/конфиг      → только «шеф»-команда
 *   стр.124..127 журнал рестартов (счётчики)           → «Сброс WDT» + «Очистить журналы»
 * Стр.121 — та самая старая страница активации, из-за которой был ECC-hardfault.
 * Теперь безопасна: пишется ТОЛЬКО под кабелем (Сервис), append-only, читается
 * ОДИН раз на старте в кэш (com.c s_actts), а не в горячем пути GET_STATS. */

/* --- Типы событий журнала (младший байт слота) --- */
#define EVT_POWERLOSS      0xF0u   /* потеря питания (POR/BOR / backup-домен стёрт) */
#define EVT_WATCHDOG       0xF1u   /* watchdog-рестарт (IWDG сбросил зависший МК) */
#define EVT_ACT_END        0xF4u   /* конец «жизни»: сохранение/снятие с активации.
                                    * Слот стр.121 как у EVT_ACTIVATION, но тип 0xF4,
                                    * [2..5]=ts окончания. Пара ACTIVATION…ACT_END =
                                    * интервал жизни. «Активирован сейчас» = последнее
                                    * событие журнала = EVT_ACTIVATION (не закрыто). */
#define EVT_ACTIVATION     0xF3u   /* активация прибора = начало новой «жизни».
                                    * Слот журнала активаций (стр.121): [0]=0xF3,
                                    * [1]=0xFF(рез.), [2..5]=ts активации (unix-сек,
                                    * LE), [6..7]=0xFFFF. Append-only, писать только
                                    * под кабелем в Сервисе. Деактивации как
                                    * операции НЕТ: журнал накапливает только
                                    * активации, «не активирован» = стёртая стр.121
                                    * («Очистить журналы»). См. SESSION_HANDOFF §2. */
#define EVT_CLOCKZERO      0xF2u   /* часы обнулились (маркёр 0xBEBE в RTC_BKP_DR0
                                    * отсутствовал → RTC уехал в 00:00 01.01.2000).
                                    * Счётчик = «поколение часов»: инкремент строго
                                    * при реальном обнулении времени, НЕ по каждому
                                    * reset (в отличие от EVT_POWERLOSS). Кладётся
                                    * u16 в запись цикла [40..41] = точка стыка
                                    * журнала. См. clock_epoch_seam_spec_v1.md. */

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

/* --- Журнал активаций (Page C, стр.121) --- */
/* Дописать активацию: слот 8 байт [0]=EVT_ACTIVATION [1]=0xFF [2..5]=ts(LE)
 * [6..7]=0xFFFF. Append-only. Вызывать ТОЛЬКО под внешним питанием (Сервис). */
void     iflash_activation_append(uint32_t ts);
/* Дописать КОНЕЦ жизни (сохранение/снятие с активации): слот тип EVT_ACT_END. */
void     iflash_activation_end(uint32_t ts);
/* ts текущей ОТКРЫТОЙ активации (последнее событие = EVT_ACTIVATION), или
 * 0xFFFFFFFF если журнал пуст либо последняя жизнь закрыта (EVT_ACT_END). */
uint32_t iflash_activation_last(void);
/* Число активаций в журнале (сколько «жизней» прожил прибор). */
uint16_t iflash_activation_count(void);
/* Выгрузить список ts активаций (по порядку) в out[0..max-1]. Возвращает
 * число реально записанных элементов (<= max, <= общего числа активаций). */
uint16_t iflash_activation_list(uint32_t *out, uint16_t max);
/* Выгрузить ВСЕ события журнала (начала EVT_ACTIVATION и концы EVT_ACT_END) по
 * порядку: types[i]/ts[i]/restarts[i] (общий счётчик перезапусков на момент
 * события). Для истории «жизней» парами начало→конец. Возврат — n. */
uint16_t iflash_activation_events(uint8_t *types, uint32_t *ts,
                                  uint16_t *restarts, uint16_t max);
/* Стереть журнал активаций (стр.121) → «не активирован». */
void     iflash_activation_reset(void);
/* Гарантировать пригодность журнала активаций (аналог journal_ensure_ready):
 * первый слот не 0xFF и не EVT_ACTIVATION → мусор → стереть. На загрузке ДО last. */
void     iflash_activation_ensure_ready(void);

/* «Очистить журналы» (provision): СЛЕПОЕ стирание журнала активаций (стр.121) И
 * журнала рестартов (стр.124..127) без предварительного чтения — прибор
 * становится «не активирован», счётчики 0. Стр.123 (заводское) НЕ трогается.
 * Слепое стирание безопасно и лечит любой битый ECC на легаси-стр.121. */
void     iflash_journals_clear_all(void);

/* --- Заводская конфигурация (Page B, стр.123): паспорт + калибровки --- */
/* Единый образ страницы: пишется ЦЕЛИКОМ (erase+program), читается напрямую.
 * Пока хранит только калибровку скорости; поля паспорта — резерв под будущее.
 * Размер кратен 8 (double-word). Валидность — по IFLASH_CFG_MAGIC. */
#define IFLASH_CFG_MAGIC   0xCA11B123u   /* 'calib 123' */
#define IFLASH_CFG_VER     1u
#define IFLASH_SPEEDCAL_MAX 16u          /* максимум узлов таблицы скорости */

typedef struct {
  uint32_t magic;                        /* IFLASH_CFG_MAGIC — признак валидности */
  uint16_t version;                      /* IFLASH_CFG_VER */
  uint16_t speed_n;                      /* число узлов калибровки скорости (<=MAX) */
  float    speed_r[IFLASH_SPEEDCAL_MAX]; /* об/мин (измеренные, по возрастанию) */
  float    speed_k[IFLASH_SPEEDCAL_MAX]; /* коэффициент задано/измерено */
  float    rtc_ppm;                      /* поправка RTC smooth-calib, ppm (знак: <0 = замедление) */
  uint8_t  rtc_valid;                    /* 1 = rtc_ppm задан пользователем (иначе дефолт) */
  uint8_t  _pad[3];                      /* выравнивание */
  /* ---- Паспорт устройства (03.08.2026). Пишется отдельной командой (0x36),
   * независимо от калибровок (read-modify-write в одной странице стр.123).
   * На СТАРОМ образе (144 Б, без паспорта) эти dword'ы = 0xFF → passport_valid
   * != 1 → паспорт «не задан», калибровки читаются как прежде. */
  char     serial[16];                   /* серийный номер, ASCII, дополнен '\0' */
  uint8_t  variant;                      /* 0x0A = A (LSM6DSO) / 0x0B = B (LSM6DSV) */
  uint8_t  passport_valid;               /* 1 = паспорт задан пользователем */
  uint16_t rel_year;                     /* дата выпуска: год (напр. 2026) */
  uint8_t  rel_month;                    /* месяц 1..12 */
  uint8_t  rel_day;                      /* день 1..31 */
  uint8_t  _pad2[2];                     /* выравнивание до кратности 8 */
} IflashCfg;                             /* 144 + 24 = 168 байт = 21 dword */

/* Прочитать конфиг стр.123. 0=ok (magic валиден), -1=нет валидного образа. */
int      iflash_cfg_read(IflashCfg *out);
/* Записать конфиг (erase стр.123 + program). ⚠ Только под кабелем/шеф-паролем. */
int      iflash_cfg_write(const IflashCfg *in);

#endif /* IFLASH_H */
