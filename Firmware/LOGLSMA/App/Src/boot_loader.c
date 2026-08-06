/* ---------------------------------------------------------------------------
 * boot_loader.c — загрузчик LogLSM. Часть образа LOGLSMA, но лежит в отдельной
 * области флеша (.bootsec, стр.113..120) и при обновлении по кабелю не
 * перезаписывается. 05.08.2026.
 *
 * ЗАЧЕМ. Обновление прошивки по сервисному кабелю, без ST-Link: LOGLSMW шлёт
 * .bin по LTP, загрузчик кладёт его в стр.0..112.
 *
 * ⚠ ЭТОТ ФАЙЛ ЖИВЁТ ПО ОСОБЫМ ПРАВИЛАМ — см. «ТРИ ПРАВИЛА ЖИЗНИ В .bootsec»
 * в App/Inc/boot.h. Коротко: никаких вызовов наружу (включая memcpy и прочие
 * невидимые помощники компилятора), никаких инициализированных статиков, вход
 * только через указатель в первом слове секции.
 *
 * ⚠ ПРЕРЫВАНИЙ НЕТ, всё опросом. Приложение перед вызовом гасит их совсем:
 * таблица векторов лежит в стираемой области, и любое прерывание, прилетевшее
 * во время заливки, ушло бы в никуда. Заодно опрос снимает вопрос о том, что
 * выборка кода из флеша во время стирания/записи останавливается (L433 — одна
 * банка): протокол строго «запрос → ответ», ПК в этот момент молчит.
 * ------------------------------------------------------------------------- */
#include "stm32l433xx.h"
#include "boot.h"
#include "ltp_boot.h"

/* --- Версия загрузчика (ГГ ММ ДД ЧЧ ММ). Меняется РЕДКО: секция обновляется
 * только прошивкой по SWD. Видна в журнале LOGLSMW при заливке. */
#define BL_YY 26
#define BL_MM  8
#define BL_DD  6
#define BL_HH  0
#define BL_MI 30   /* аварийный вектор сброса в стр.0 на время заливки */

#define BL_ADDRESS  0x8D          /* тот же адрес LTP, что у приложения */
#define UART_BRR_80MHZ_921600 87u /* 80 000 000 / 921 600 = 86.8 → 87 (−0.22 %) */

/* Первое слово секции = адрес точки входа (правило 3 в boot.h). Приложение
 * читает его по фиксированному BOOT_ENTRY_PTR_ADDR и зовёт то, что там лежит:
 * так вызов остаётся верным, даже когда приложение обновилось по кабелю, а
 * секция осталась от прошивки по SWD. Константа — значит во флеше, не в .data.
 * `used` — чтобы её не выкинул --gc-sections. */
__attribute__((used, section(".bootsec_entry")))
void (* const boot_entry_ptr)(void) = boot_loader_run;

/* ========================================================================== */
/*  Состояние заливки                                                          */
/* ========================================================================== */
/* ⚠ Все — БЕЗ инициализаторов (правило 2): стартовый код приложения к этой
 * секции отношения не имеет. Обнуляются явно в boot_loader_run(). */
static uint32_t s_img_size;      /* размер образа из BEGIN */
static uint32_t s_img_crc;       /* ожидаемый CRC32 из BEGIN */
static uint32_t s_img_top;       /* максимум (смещение+длина) из принятых DATA */
static uint8_t  s_begun;         /* BEGIN получен и стирание прошло */
static uint8_t  s_page0_ready;   /* стр.0 освобождена под настоящие вектора (аварийный снят) */

static BootLtpParser s_parser;
static uint8_t       s_tx[BOOT_LTP_MAX_FRAME(64)];   /* ответы короткие */

/* Ручная замена memcpy: обращение к libc из этой секции запрещено (правило 1). */
static void boot_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
  while (n--) *dst++ = *src++;
}

/* ========================================================================== */
/*  Сторож                                                                     */
/* ========================================================================== */
/* IWDG приложения продолжает идти (сброса на входе не было). Ключ рефреша
 * безопасен и когда сторож не запущен — пишем безусловно. */
static void boot_iwdg_kick(void) { IWDG->KR = 0x0000AAAAu; }

/* ========================================================================== */
/*  Тактирование: 80 МГц (HSI16 /2 ×20 /2), как в «Сервисе» приложения          */
/* ========================================================================== */
/* Причина не в скорости, а в бодах: на 16 МГц делитель 921600 даёт ошибку
 * +2.1 %, на грани срыва кадра; на 80 МГц — −0.22 %.
 * ⚠ Прийти сюда можно с ЛЮБОГО клока (в «Сервисе» уже стоит PLL 80 МГц),
 * поэтому сначала уводим SYSCLK на HSI16 и только потом трогаем PLL:
 * выключать PLL, с которого тактируемся, нельзя. */
static void clock_80mhz(void)
{
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS;
  while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) { }

  RCC->CR |= RCC_CR_HSION;
  while (!(RCC->CR & RCC_CR_HSIRDY)) { }

  RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }

  RCC->CR &= ~RCC_CR_PLLON;
  while (RCC->CR & RCC_CR_PLLRDY) { }

  /* PLLSRC=HSI16(10) | PLLM=2 (поле = M−1) | PLLN=20 | PLLR=2 (поле 00) + PLLREN
   * VCO = 16/2×20 = 160 МГц (в допуске 64..344), SYSCLK = 160/2 = 80 МГц. */
  RCC->PLLCFGR = (2u << RCC_PLLCFGR_PLLSRC_Pos)
               | (1u << RCC_PLLCFGR_PLLM_Pos)
               | (20u << RCC_PLLCFGR_PLLN_Pos)
               | RCC_PLLCFGR_PLLREN;

  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY)) { }

  /* AHB/APB1/APB2 без деления → PCLK1 = 80 МГц (важно для BRR ниже) */
  RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2))
            | RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }
}

/* ========================================================================== */
/*  USART2 (PA2 TX / PA3 RX) — сервисный разъём XP2, 921600 8N1                 */
/* ========================================================================== */
static void uart_init(void)
{
  RCC->AHB2ENR  |= RCC_AHB2ENR_GPIOAEN;
  RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;

  /* ⚠ Приложение оставило USART2 на приёме через DMA. Сбрасываем периферию
   * целиком — иначе к нашему опросному приёму примешивался бы чужой
   * недоделанный DMA-запрос. */
  RCC->APB1RSTR1 |=  RCC_APB1RSTR1_USART2RST;
  RCC->APB1RSTR1 &= ~RCC_APB1RSTR1_USART2RST;

  /* PA2/PA3 → альтернативная функция AF7 (USART2). RX с подтяжкой вверх:
   * без кабеля вход не должен ловить наводку (тот же урок, что с PC11 на
   * стенде — плавающий RX сыпал мусор в парсер). */
  GPIOA->MODER   = (GPIOA->MODER & ~(GPIO_MODER_MODE2 | GPIO_MODER_MODE3))
                 | (2u << GPIO_MODER_MODE2_Pos) | (2u << GPIO_MODER_MODE3_Pos);
  GPIOA->OSPEEDR |= (3u << GPIO_OSPEEDR_OSPEED2_Pos) | (3u << GPIO_OSPEEDR_OSPEED3_Pos);
  GPIOA->PUPDR   = (GPIOA->PUPDR & ~(GPIO_PUPDR_PUPD2 | GPIO_PUPDR_PUPD3))
                 | (1u << GPIO_PUPDR_PUPD3_Pos);
  GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~(GPIO_AFRL_AFSEL2 | GPIO_AFRL_AFSEL3))
                 | (7u << GPIO_AFRL_AFSEL2_Pos) | (7u << GPIO_AFRL_AFSEL3_Pos);

  USART2->CR1 = 0;
  USART2->CR2 = 0;
  USART2->CR3 = 0;
  USART2->BRR = UART_BRR_80MHZ_921600;
  USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
  while (!(USART2->ISR & USART_ISR_TEACK)) { }
}

static void uart_send(const uint8_t *p, uint32_t n)
{
  while (n--)
  {
    while (!(USART2->ISR & USART_ISR_TXE)) { boot_iwdg_kick(); }
    USART2->TDR = *p++;
  }
  while (!(USART2->ISR & USART_ISR_TC)) { boot_iwdg_kick(); }
}

/* Принять байт, если есть. Возврат 1 = принят. Ошибки приёма (в первую очередь
 * ORE после долгого стирания) гасим и идём дальше — парсер LTP всё равно
 * синхронизируется по FEND. */
static int uart_poll(uint8_t *out)
{
  uint32_t isr = USART2->ISR;
  if (isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE))
    USART2->ICR = USART_ICR_ORECF | USART_ICR_NCF | USART_ICR_FECF | USART_ICR_PECF;
  if (!(isr & USART_ISR_RXNE))
    return 0;
  *out = (uint8_t)USART2->RDR;
  return 1;
}

static void send_reply(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t n)
{
  int size = boot_ltp_build(s_tx, sizeof(s_tx), BL_ADDRESS, cmd,
                            BOOT_LTP_FLAG_DIR, seq, data, n);
  if (size > 0)
    uart_send(s_tx, (uint32_t)size);
}

static void send_code(uint8_t cmd, uint16_t seq, uint8_t code)
{
  send_reply(cmd, seq, &code, 1);
}

/* ========================================================================== */
/*  Внутренняя Flash: стирание страницы / запись двойного слова                 */
/* ========================================================================== */
/* Последовательности сняты с HAL (FLASH_PageErase / FLASH_Program_DoubleWord),
 * но без HAL-обвязки: она живёт в области приложения и к моменту стирания
 * недоступна. L433 — ОДНА банка, бита BKER нет, номер страницы 0..127 кладётся
 * прямо в PNB. */
#define FLASH_ALL_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | \
                          FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                          FLASH_SR_MISERR | FLASH_SR_FASTERR)

static void flash_unlock(void)
{
  if (FLASH->CR & FLASH_CR_LOCK)
  {
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
  }
}

static void flash_lock(void) { FLASH->CR |= FLASH_CR_LOCK; }
static void flash_wait(void) { while (FLASH->SR & FLASH_SR_BSY) { } }

static int flash_erase_page(uint32_t page)
{
  boot_iwdg_kick();                 /* стирание страницы ~22 мс, их бывает сотня */
  flash_wait();
  FLASH->SR = FLASH_ALL_ERRORS | FLASH_SR_EOP;
  FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) | (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
  FLASH->CR |= FLASH_CR_STRT;
  flash_wait();
  FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
  if (FLASH->SR & FLASH_ALL_ERRORS) { FLASH->SR = FLASH_ALL_ERRORS; return -1; }
  FLASH->SR = FLASH_SR_EOP;
  return 0;
}

static int flash_program_dword(uint32_t addr, uint32_t lo, uint32_t hi)
{
  flash_wait();
  FLASH->SR = FLASH_ALL_ERRORS | FLASH_SR_EOP;
  FLASH->CR |= FLASH_CR_PG;
  *(volatile uint32_t *)addr       = lo;
  __ISB();
  *(volatile uint32_t *)(addr + 4) = hi;
  flash_wait();
  FLASH->CR &= ~FLASH_CR_PG;
  if (FLASH->SR & FLASH_ALL_ERRORS) { FLASH->SR = FLASH_ALL_ERRORS; return -1; }
  FLASH->SR = FLASH_SR_EOP;
  return 0;
}

/* После стирания/записи кэш команд может держать устаревшие строки — сбросить.
 * ⚠ Мы сами исполняемся из .bootsec, которая не переписывается, так что сброс
 * кэша безопасен. */
static void flash_cache_reset(void)
{
  FLASH->ACR &= ~FLASH_ACR_ICEN;
  FLASH->ACR |= FLASH_ACR_ICRST;
  FLASH->ACR &= ~FLASH_ACR_ICRST;
  FLASH->ACR |= FLASH_ACR_ICEN;
}

/* ========================================================================== */
/*  CRC32 (zlib) — программно, побитно                                          */
/* ========================================================================== */
/* Считается ОДИН раз, в END, и по УЖЕ ЗАПИСАННОЙ Flash, а не по принятому
 * потоку — так проверяется то, что реально легло в чип. На 80 МГц 226 КБ
 * худшего случая ≈ 0.2 c, таблица в 1 КБ ради этого не нужна. */
static uint32_t crc32_flash(uint32_t addr, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;
  const volatile uint8_t *p = (const volatile uint8_t *)addr;
  for (uint32_t i = 0; i < len; i++)
  {
    if ((i & 0x3FFFu) == 0) boot_iwdg_kick();
    crc ^= p[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
  }
  return ~crc;
}

/* ========================================================================== */
/*  Обработка команд                                                           */
/* ========================================================================== */
static void reply_whoami(uint8_t cmd, uint16_t seq)
{
  static const char name[] = BOOT_DEV_NAME;   /* const → .rodata секции, не .data */
  uint8_t resp[6 + sizeof(name)];
  resp[0] = BOOT_WHOAMI_MARK;    /* у приложения тут WHO_AM_I датчика — не спутать */
  resp[1] = BL_YY;
  resp[2] = BL_MM;
  resp[3] = BL_DD;
  resp[4] = BL_HH;
  resp[5] = BL_MI;
  boot_copy(&resp[6], (const uint8_t *)name, sizeof(name));   /* вместе с '\0' */
  send_reply(cmd, seq, resp, sizeof(resp));
}

static void handle_begin(const BootLtpPacket *p)
{
  if (p->n < 8) { send_code(p->cmd, p->seq, BOOT_ERR_SIZE); return; }

  uint32_t size = (uint32_t)p->data[0] | ((uint32_t)p->data[1] << 8)
                | ((uint32_t)p->data[2] << 16) | ((uint32_t)p->data[3] << 24);
  uint32_t crc  = (uint32_t)p->data[4] | ((uint32_t)p->data[5] << 8)
                | ((uint32_t)p->data[6] << 16) | ((uint32_t)p->data[7] << 24);

  if (size == 0 || size > APP_MAX_SIZE) { send_code(p->cmd, p->seq, BOOT_ERR_SIZE); return; }

  s_begun       = 0;
  s_img_size    = size;
  s_img_crc     = crc;
  s_img_top     = 0;
  s_page0_ready = 0;

  flash_unlock();

  /* ── АВАРИЙНЫЙ ВЕКТОР СБРОСА ─────────────────────────────────────────────
   * Пока идёт заливка, приложения фактически нет: его код стёрт. Если в этот
   * момент пропадёт питание, процессор возьмёт вектор сброса из стр.0 и уйдёт
   * в стёртую область — прибор не поднимется, лечение только SWD.
   *
   * Поэтому ПЕРВЫМ делом кладём в стр.0 временный вектор, указывающий на
   * ЗАГРУЗЧИК: [0]=вершина стека, [1]=его точка входа. Тогда обрыв в любой
   * момент долгой заливки означает всего лишь «прибор включился сразу в
   * загрузчик и ждёт повторной заливки» — по кабелю, без программатора.
   *
   * Настоящие вектора приложения приедут последними (LOGLSMW шлёт начало
   * образа в конце) и затрут этот аварийный — см. handle_data. Уязвимым
   * остаётся только тот момент, десятки миллисекунд.
   *
   * ⚠ Загрузчик к такому старту готов: он не рассчитывает ни на .data, ни на
   * обнулённую .bss (правило 2 в boot.h) и сам поднимает клок и UART. */
  if (flash_erase_page(APP_PAGE_FIRST) != 0)
  {
    flash_lock();
    send_code(p->cmd, p->seq, BOOT_ERR_ERASE);
    return;
  }
  {
    const uint32_t entry = *(volatile uint32_t *)BOOT_ENTRY_PTR_ADDR;
    /* Вершина стека — конец ОЗУ (48 КБ), как у самого приложения. */
    if (flash_program_dword(APP_BASE, 0x2000C000u, entry) != 0)
    {
      flash_lock();
      send_code(p->cmd, p->seq, BOOT_ERR_PROGRAM);
      return;
    }
  }

  uint32_t pages = (size + BOOT_PAGE_SIZE - 1u) / BOOT_PAGE_SIZE;   /* делитель — степень 2 */
  for (uint32_t i = 1; i < pages; i++)
  {
    if (flash_erase_page(APP_PAGE_FIRST + i) != 0)
    {
      flash_lock();
      send_code(p->cmd, p->seq, BOOT_ERR_ERASE);
      return;
    }
  }
  flash_lock();
  flash_cache_reset();

  s_begun = 1;
  send_code(p->cmd, p->seq, BOOT_OK);
}

static void handle_data(const BootLtpPacket *p)
{
  if (!s_begun || p->n < 5) { send_code(p->cmd, p->seq, BOOT_ERR_SEQ); return; }

  uint32_t off = (uint32_t)p->data[0] | ((uint32_t)p->data[1] << 8)
               | ((uint32_t)p->data[2] << 16) | ((uint32_t)p->data[3] << 24);
  uint32_t len = (uint32_t)p->n - 4u;

  /* Смещение обязано быть кратно 8: Flash L4 пишется двойными словами, и
   * каждое можно записать РОВНО ОДИН раз после стирания (ECC). */
  if ((off & 7u) != 0 || len > BOOT_CHUNK_MAX || (off + len) > s_img_size)
  { send_code(p->cmd, p->seq, BOOT_ERR_SEQ); return; }

  flash_unlock();

  /* Первый пакет, попадающий в стр.0 — время снять аварийный вектор (его
   * положил BEGIN) и освободить страницу под настоящие вектора приложения.
   * Всё остальное к этому моменту уже лежит, так что незащищённым остаётся
   * только промежуток отсюда до конца записи стр.0 — десятки миллисекунд.
   * Обрыв ДО этого места безопасен: прибор поднимется в загрузчик. */
  if (off < BOOT_PAGE_SIZE && !s_page0_ready)
  {
    if (flash_erase_page(APP_PAGE_FIRST) != 0)
    {
      flash_lock();
      s_begun = 0;
      send_code(p->cmd, p->seq, BOOT_ERR_ERASE);
      return;
    }
    s_page0_ready = 1;
    flash_cache_reset();
  }

  for (uint32_t i = 0; i < len; i += 8u)
  {
    /* Собираем двойное слово побайтно: смещение в пакете кратно 8, но сам
     * буфер пакета — нет, а невыровненное 32-битное чтение здесь ни к чему.
     * Хвост добиваем 0xFF («стёртое» состояние). */
    uint32_t lo = 0, hi = 0;
    for (uint32_t k = 0; k < 4u; k++)
    {
      uint32_t b0 = ((i + k)     < len) ? p->data[4 + i + k]     : 0xFFu;
      uint32_t b1 = ((i + k + 4) < len) ? p->data[4 + i + k + 4] : 0xFFu;
      lo |= b0 << (8u * k);
      hi |= b1 << (8u * k);
    }

    /* ПОВТОР ПАКЕТА — штатная ситуация: DeviceController переспрашивает, если
     * ответ потерялся, и тот же DATA приезжает дважды. Двойное слово пишется
     * РОВНО ОДИН РАЗ после стирания (ECC), так что вторая запись была бы
     * ошибкой. Поэтому: то же значение уже лежит — молча пропускаем; лежит
     * чужое — честная ошибка (значит, поехало смещение). */
    const uint32_t a = APP_BASE + off + i;
    const uint32_t cur_lo = *(volatile uint32_t *)a;
    const uint32_t cur_hi = *(volatile uint32_t *)(a + 4u);
    if (cur_lo == lo && cur_hi == hi) continue;
    if (cur_lo != 0xFFFFFFFFu || cur_hi != 0xFFFFFFFFu)
    {
      flash_lock();
      s_begun = 0;
      send_code(p->cmd, p->seq, BOOT_ERR_PROGRAM);
      return;
    }

    if (flash_program_dword(a, lo, hi) != 0)
    {
      flash_lock();
      s_begun = 0;
      send_code(p->cmd, p->seq, BOOT_ERR_PROGRAM);
      return;
    }
  }
  flash_lock();

  if ((off + len) > s_img_top) s_img_top = off + len;
  send_code(p->cmd, p->seq, BOOT_OK);
}

static void handle_end(const BootLtpPacket *p)
{
  if (!s_begun)               { send_code(p->cmd, p->seq, BOOT_ERR_SEQ);   return; }
  if (s_img_top < s_img_size) { send_code(p->cmd, p->seq, BOOT_ERR_SHORT); return; }

  flash_cache_reset();
  const int ok = (crc32_flash(APP_BASE, s_img_size) == s_img_crc);
  s_begun = 0;
  send_code(p->cmd, p->seq, ok ? BOOT_OK : BOOT_ERR_CRC);
}

static void handle_packet(const BootLtpPacket *p)
{
  switch (p->cmd)
  {
    case 0x01:                 /* PING — пустой ACK, как в приложении */
      send_reply(p->cmd, p->seq, 0, 0);
      break;

    case 0x02:                 /* WHO_AM_I */
    case LTP_CMD_BOOT_ENTER:   /* «я уже в загрузчике» — тот же ответ */
      reply_whoami(p->cmd, p->seq);
      break;

    case LTP_CMD_BOOT_BEGIN: handle_begin(p); break;
    case LTP_CMD_BOOT_DATA:  handle_data(p);  break;
    case LTP_CMD_BOOT_END:   handle_end(p);   break;

    case LTP_CMD_BOOT_GO:
      /* Уходим в приложение через СБРОС: так оно стартует с железа в состоянии
       * «после reset», как и ожидает. Заодно гаснет наш клок/UART и сторож,
       * который мы тут гладили. */
      send_code(p->cmd, p->seq, BOOT_OK);
      NVIC_SystemReset();
      break;

    default:
    {
      uint8_t err = BOOT_LTP_ERR_UNKNOWN_CMD;
      int size = boot_ltp_build(s_tx, sizeof(s_tx), BL_ADDRESS, p->cmd,
                                BOOT_LTP_FLAG_DIR | BOOT_LTP_FLAG_ERR, p->seq, &err, 1);
      if (size > 0) uart_send(s_tx, (uint32_t)size);
      break;
    }
  }
}

/* ========================================================================== */
/*  Точка входа                                                                */
/* ========================================================================== */
/* Сюда попадают ДВУМЯ путями:
 *   1) штатно — вызовом из приложения по указателю из первого слова секции
 *      (правило 3); приложение уже погасило прерывания, SysTick и UART;
 *   2) АВАРИЙНО — прямо с вектора сброса, если питание пропало посреди
 *      заливки: в стр.0 на это время лежит временный вектор, указывающий
 *      сюда (см. handle_begin). Тогда никакого стартового кода до нас не
 *      было вовсе — ни .data, ни обнулённой .bss. Именно поэтому всё
 *      состояние обнуляется ниже руками, а клок и UART поднимаются сами.
 * Не возвращается: работа заканчивается сбросом по команде GO. */
void boot_loader_run(void)
{
  /* Правило 2: обнуляем состояние сами, на стартовый код приложения не
   * рассчитываем (он от другой сборки). */
  s_img_size    = 0;
  s_img_crc     = 0;
  s_img_top     = 0;
  s_begun       = 0;
  s_page0_ready = 0;

  boot_iwdg_kick();
  clock_80mhz();
  uart_init();
  boot_ltp_parser_init(&s_parser);

  for (;;)
  {
    boot_iwdg_kick();

    uint8_t b;
    if (uart_poll(&b))
    {
      boot_ltp_parser_feed(&s_parser, b);
      if (s_parser.packet_recognized)
      {
        s_parser.packet_recognized = 0;
        if (s_parser.pkt.addr == BL_ADDRESS && !(s_parser.pkt.flags & BOOT_LTP_FLAG_DIR))
          handle_packet(&s_parser.pkt);
      }
    }
  }
}
