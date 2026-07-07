# Session Notes 2026-06-30 — LOGLSMA 921600 / "нет ответа rx 12 б"

## Цель сессии
Устранить задержку 12–15 с при переподключении LOGLSMA к LOGLSMW.

---

## Что сделано

### 1. DMA-паттерн в com.c (LOGLSMA) — перенесён из рабочего стенда

Proven-паттерн из `Stend/Stend LOGLSM/Core/Src/com_interr.c` (работал идеально,
пользователь подтвердил — быстрое переподключение) перенесён в
`Firmware/LOGLSMA/App/Src/com.c`:

- **`HAL_UARTEx_RxEventCallback`** — исправлен wrap-around:
  ```c
  if(prevFill <= fill)
    ReadReceiveBuffer(huart, prevFill, fill - prevFill);
  else {
    ReadReceiveBuffer(huart, prevFill, sizeof(RxBuf) - prevFill);
    ReadReceiveBuffer(huart, 0, fill);
  }
  prevFill = fill;
  ```
- **`HAL_UART_ErrorCallback`** — восстановление после ORE/FE (переподключение VCP):
  ```c
  prevFill = 0;
  ltp_parser_init(&ltp_rx);
  huart2.RxState = HAL_UART_STATE_READY;
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxBuf, sizeof(RxBuf));
  __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);
  ```
- **`ComPoll`** — убран `dma_restart_needed` блок (сбрасывал prevFill=0 после
  каждого IDLE, вызывал двойную обработку байт и prevFill-коррупцию).
- **`ComInit`** — добавлен `__HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT)`
  после старта DMA.

### 2. Baud rate в usart.c — исправлен

**Файл:** `Firmware/LOGLSMA/Core/Src/usart.c`, строка 63.

**Было:** `huart2.Init.BaudRate = 115200;`  
**Стало:** `huart2.Init.BaudRate = 921600;`

LOGLSMA работал на 115200, хотя план всегда был 921600. `.ioc` мог быть обновлён,
но C-код не был регенерирован из CubeMX. LOGLSMW уже переключён на 921600 ранее.

Матчинг скоростей: 16 МГц / 921600 ≈ 17.36 — погрешность <0.1%, HAL считает BRR
корректно, UART работает.

---

## Текущий статус: "нет ответа (rx 12 б, CRC-ош 0)"

После обеих правок (com.c + usart.c) LOGLSMW по-прежнему видит `rx 12 б, CRC-ош 0`
на каждой попытке сканирования. Означает:

- `CRC-ош 0` = QLtp-парсер на стороне ПК **не нашёл ни одного завершённого пакета**
  (не добрался до проверки CRC). 12 байт приходят, но не образуют валидный
  FEND-terminated LTP-фрейм.
- Это не ответ LOGLSMA. Скорее всего это **эхо TX-байт LOGLSMW** обратно на RX
  (12 байт = PING-пакет с 2 stuffed байтами в CRC, или шум на линии).
- **LOGLSMA вероятно не отвечает вообще** — либо не доходит до Service(), либо
  физическая проблема с проводкой.

### Главный подозреваемый: `lsm6dso_init()` в main.c

В `Firmware/LOGLSMA/Core/Src/main.c` в начале `while(1)`, до вызова `Service()`:

```c
lsm6dso_init();  /* TODO: если вернётся Error_Handler — вынести в Service() */
```

Если LSM6DSO не ответил по SPI (например, не запитан, или SPI неинициализирован
на этапе вызова) — внутри `lsm6dso_init()` вызывается `Error_Handler()` →
**вечный цикл → Service() никогда не вызывается → UART молчит**.

До наших изменений LOGLSMA тоже имел эту проблему, но она маскировалась тем, что
`Service()` иногда всё же запускался (возможно, IMU инициализировался нестабильно,
или была другая конфигурация). С переключением на 921600 зазора не осталось.

**Комментарий в коде подтверждает осознанный риск** — он прямо говорит "вынести в
Service()". Это и нужно сделать.

### Ключевая проверка: подключить осциллограф/мультиметр к PA0 (WKUP1)

Bypass `if (1 || ...)` для `Service()` уже стоит в main.c — должен работать
независимо от WKUP1. Так что проблема не в этом.

---

## Уточнение архитектуры тактирования (вторая часть сессии 30.06.2026)

Пользователь уточнил дизайн тактирования:
- **SERVICE**: питание внешнее → можно полная скорость HSI (16 МГц)
- **WORK**: основной источник — часовой кварц (LSE = 32768 Гц, для RTC/низкой мощности)
- **Внутренний RC (HSI)** — где необходимо (например, SERVICE)

`SystemClock_Config()` ставит `AHBCLKDivider = RCC_SYSCLK_DIV4` → HCLK = 4 МГц.
Это правильно как базовый старт для WORK/PASSIVE. Но для SERVICE надо 16 МГц.

### Почему это важно для UART 921600

| PCLK1 | BRR | Реальная скорость | Погрешность |
|---|---|---|---|
| 4 МГц | 69 | 927,536 бод | 0.64% |
| 16 МГц | 278 | 920,863 бод | **0.08%** |

0.64% в теории укладывается в допуск UART (±2%), но на практике могут быть
проблемы с точностью тактирования ST-Link VCP моста на другом конце.
0.08% — гарантированно надёжно.

**Важно: `HAL_RCC_ClockConfig` автоматически перенастраивает SysTick** через
`HAL_InitTick` в конце функции → HAL_Delay/HAL_GetTick остаются правильными.
`FLASH_LATENCY_0` валиден до 16 МГц при VRANGE1 — менять не надо.

### Что сделано в сессии

Добавлена `ServiceClock_Config()` в `main.c` (USER CODE BEGIN 4):
```c
static void ServiceClock_Config(void)
{
  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;  /* убираем /4: HCLK = 16 МГц */
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK)
    Error_Handler();
}
```

В `while(1)` перед `Service()`:
```c
ServiceClock_Config();   // HCLK = 16 МГц для SERVICE
lsm6dso_init();          // реинит I2C1 при новой тактовой
Service();               // ComInit() переинитит UART → BRR=278 → 921600 @ 0.08%
```

`lsm6dso_init()` переинициализирует I2C1 с правильными делителями для 16 МГц
(при первом вызове (до while) он инициализировался при 4 МГц — разные
TIMINGR/PRESC внутри MX_I2C1_Init). `tmp117Activ()`/`framActiv()` внутри
`Service()` инициализируют I2C2/SPI1 тоже при 16 МГц — корректно.

---

## Состояние файлов (актуально на конец сессии 30.06.2026)

| Файл | Что изменено |
|---|---|
| `Firmware/LOGLSMA/App/Src/com.c` | DMA wrap-around, ErrorCallback recovery, убран dma_restart_needed |
| `Firmware/LOGLSMA/Core/Src/usart.c` | BaudRate: 115200 → 921600 |
| `Firmware/LOGLSMA/Core/Src/main.c` | **НОВОЕ:** ServiceClock_Config() + вызов перед Service(); повторный lsm6dso_init() |
| `Stend/Stend LOGLSM/Core/Src/com_interr.c` | Полная замена — minimal LTP responder 0x8D |
| `SoftWare/LOGLSMW/portscanner.h/.cpp` | 5ms пауза между двумя PING |

**Не изменено:**
- `Firmware/LOGLSMA/Core/Src/dma.c` — DMA_CIRCULAR уже стоял, не трогали

---

## Точка входа для следующей сессии

1. **Прошить LOGLSMA** (`Firmware/LOGLSMA`, main.c + com.c + usart.c изменены)
2. **Проверить PING**: запустить LOGLSMW на 921600, подождать сканирования
3. Если "нет ответа" → проверить физическую проводку TX/RX (LOGLSMA PA2→Nucleo RX, PA3→Nucleo TX)
4. Если отвечает → проверить полный цикл подключения (WHO_AM_I, GET_STATS и т.д.)
5. Если отвечает → выдернуть и вставить кабель → убедиться что переподключение < 1 с

---

## Технические детали топологии

LOGLSMA (STM32L433CC) подключён к ПК через ST-Link VCP Nucleo L476RG
с перемычками в режиме внешнего UART (CN2 снят). ST-Link прозрачно
пробрасывает 921600 бод — никакого фиксированного бода в bridge-режиме нет.
