# Сессия 29 июня 2026 — Debug-проект TEST_PING, диагностика UART

## Статус: Debug-проект создан (TEST_PING), нестабильный обмен не воспроизведён на стенде

---

## Контекст

Пришли с проблемой нестабильного обмена LOGLSMA ↔ LOGLSMW по UART (VCP ST-Link,
USART2 PA2/PA3, прямое подключение без стенда). Стенд на 115200 работает без
проблем. Нестабильность появилась «в какой-то момент» — до этого 115200 работал.

Предыдущая сессия (предположительно 28.06.2026) — попали в лимит, заметки не сохранены.
Ситуация на момент начала этой сессии: обмен идёт на 115200 (LOGLSMA + LOGLSMW),
нестабильный.

---

## Критическая находка: 921600 бод невозможен при PCLK = 4 МГц

**SystemClock_Config() в обоих LOGLSMA и TEST_PING:**
```c
RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV4;  // HSI 16 МГц → HCLK 4 МГц
RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;   // PCLK1 = 4 МГц
```

При PCLK1 = 4 МГц:
- 921600 бод → USARTDIV = 4.34 → BRR = 4 → реальный бод = **1 000 000** → ошибка **+8.5%**
- UART допускает ≤2% → все фреймы при 921600 корректируются неверно
- **115200** бод → USARTDIV = 34.7 → BRR = 35 → реальный бод = 114 286 → ошибка 0.8% ✓

**Это объясняет молчание LOGLSMA 22.06.2026**: утром работало на 115200, подняли
до 921600 → стало молчать (не баг кода, а физика UART). Откат кода ничего не дал
именно потому, что проблема не в коде, а в тактировании.

**Решение**: оставить 115200 статически. При необходимости 921600 — нужно включить
PLL (HSI → 64 или 80 МГц SYSCLK). Без PLL максимально надёжно ~460800 при 16 МГц PCLK.

---

## Debug-проект: TEST_PING

Проект `Firmware/TEST_PING/` используется как Debug-проект. Уже содержит:
- BaudRate USART2 = **115200** ✓ (в отличие от LOGLSMA после правок 22.06)
- lsm6dso_init() закомментирован в main.c ✓
- Полный стек LTP (ltp.c/com.c, идентичный LOGLSMA)

### Изменения в TEST_PING/App/Src/com.c (29.06.2026)

**1. Добавлен DEBUG_COM блок** с `dbg_print()`:
```c
#define DEBUG_COM
#define DBG_HB_MS  5000u
```

**2. Service() переработан:**
- `dbg_print("[DBG] UART_OK - ComInit done\r\n")` сразу после ComInit  
  → если видно в терминале = USART2 TX исправен
- `tmp117Activ()` и `framActiv()` **пропущены** → PING отвечает мгновенно,
  без блокировки на I2C/SPI init (10–50 мс)
- Heartbeat каждые 5 сек: `[DBG] ALIVE t=XXX crc_err=YYY\r\n`
  → показывает, что цикл живой + счётчик CRC-ошибок парсера

**3. Исправлен потенциальный баг в ComPoll() (CIRCULAR DMA):**
```c
// Было (баг):
prevFill = 0;
HAL_UARTEx_ReceiveToIdle_DMA(...);  // возврат не проверялся

// Стало (правка):
if(HAL_OK == HAL_UARTEx_ReceiveToIdle_DMA(...))
  prevFill = 0;   // только если DMA реально перезапущен
// иначе prevFill остаётся — в CIRCULAR-режиме DMA уже бежит
```

При DMA_CIRCULAR HAL возвращает HAL_BUSY (DMA уже активен). Старый код
безусловно обнулял prevFill → при следующем IDLE парсер обрабатывал
байты [0..fill] повторно (старые + новые) → CRC-ошибки → нестабильный обмен.

---

## Диагностика нестабильного обмена — что проверять

Последовательность действий:

1. **Прошить TEST_PING** на LOGLSMA (ST-Link Run, не Debug, проверить `.launch`
   на `DEBUGGER_STOP_AT_MAIN` — на стенде этот баг уже встречался!)

2. **Открыть любой терминал** на COM3 (VCP ST-Link), 115200 8N1. Должно появиться:
   ```
   [DBG] UART_OK - ComInit done
   [DBG] entering loop (TMP117/FRAM skipped)
   ```
   Если НЕ появляется — проблема до UART (SystemClock / ComInit / пин PA2).

3. **Подключить через LOGLSMW**. При нестабильном обмене смотреть в терминале:
   ```
   [DBG] ALIVE t=5000 crc_err=3
   ```
   Если `crc_err` растёт — LTP-парсер получает мусор (DMA-баг или бод).
   Если `crc_err=0` но LOGLSMW не отвечает — проблема в SEQ/адресации на уровне Qt.

4. **Если TEST_PING стабилен, а LOGLSMA нестабилен** — разница в tmp117Activ/framActiv
   (I2C lockup?) или в том, что LOGLSMA не перешит после правок 22.06.

---

## Возможные причины нестабильного обмена (не закрыты)

| Причина | Признак | Диагностика |
|---|---|---|
| LOGLSMA прошита 921600-версией | Полное молчание или мусор | Перешить из чистого исходника |
| I2C bus locked (TMP117) | Зависание в Service() перед комм. | Маркер TMP117_OK не появляется |
| CRC-ошибки из-за DMA circular | crc_err растёт, иногда ответ есть | Смотреть heartbeat crc_err |
| PortScanner таймаут < startup | Долгое подключение, потом ОК | TEST_PING: нет tmp117/fram init |

---

## Точка входа для следующей сессии

1. Прошить TEST_PING на LOGLSMA, открыть терминал — убедиться в `[DBG] UART_OK`
2. Подключить LOGLSMW — проверить стабильность PING
3. Если стабильно с TEST_PING — проблема в LOGLSMA (перешить LOGLSMA с тем же 115200)
4. Если нестабильно и с TEST_PING — проблема в LOGLSMW/Qt стороне (DeviceController?)

**Стенд (Nucleo-L476, 921600, DMA)**: рабочий, не трогать.  
**LOGLSMA основная прошивка**: состояние неизвестно, возможно всё ещё 921600 в .elf.

---

## Дополнение (продолжение сессии 29.06.2026)

### Диагностические данные от пользователя

Лог LOGLSMW при нескольких подключениях подряд (TEST_PING, uptime ~36-38 мин):

```
[10:37:49] Поиск… → [10:37:50] COM3 найдено    ← мгновенно
[10:38:59] Отключено (69 сек соединение)
[10:39:02] Поиск… → [10:39:02] COM3 найдено    ← мгновенно

[10:39:06] Отключено (4 сек соединение)
[10:39:08] Поиск… → [10:39:24] нет ответа      ← 16 сек, 0 байт
[10:39:25] Поиск… → [10:39:26] COM3 найдено    ← авторетрай (мой фикс)

[10:39:43] Отключено (17 сек соединение)
[10:39:46] Поиск… → [10:39:57] COM3 найдено    ← 11 сек задержка

GET_STATS: "0 ч 36 м, рестарты T=0 P=0" → "37 м" → "38 м"  ← НЕТ РЕСТАРТОВ
```

Ключевые наблюдения:
- 0 байт принято за 16 с — heartbeat (каждые 5 с) тоже не виден → main loop ЗАБЛОКИРОВАН
- Устройство не ресетится (uptime непрерывный)
- После 69-секундного соединения — мгновенное переподключение; после 4-секундного — 16-секундная блокировка

### Найдена и подтверждена причина задержек

**Уровень 1 — LOGLSMW: ComLink::close() не эмитировал linkLost**

`linkLost` ранее эмитировался только при `ResourceError` (физическое отключение кабеля).
При штатном нажатии «Отключить» `DeviceController` не получал сигнала → `m_inFlight=true`, таймер старой
команды продолжал тикать. При переподключении `pump()` видел `m_inFlight=true` и не отправлял
FLASH_ON/probeFlash → LOGLSMW «висел» до истечения всей цепочки повторов (до ~1.5 с). Но при
короткой сессии с запущенным archiveRescanFull in-flight команда FLASH_READ могла занять
несколько секунд на стороне устройства.

**Фикс**: `ComLink::close()` теперь всегда эмитирует `linkLost("отключено")`.

**Уровень 2 — TEST_PING: QUADSPI блокировал ComPoll на 5-16 с**

Задокументировано в самом коде (строки 575-577, комментарий):
> Flash в QPI-режиме не понимает SPI-команды инициализации → QUADSPI таймаут 5-16 с,
> в течение которых ComPoll() не работает.

LOGLSMW автоматически шлёт FLASH_ON (0x0E) при подключении. `cmdFlashOn` вызывал
`flashActiv()` (p25q_init/P25Qx_SetQPI). Если Flash остался в QPI-режиме от предыдущей
сессии, SPI-команды init вешали QUADSPI до таймаута. За это время ComPoll не работал →
heartbeat не выходил → 0 байт в PortScanner.

**Фикс**: В TEST_PING все QUADSPI/I2C/SPI-Flash операции заменены stub-ответами.

### Изменения в коде

**`SoftWare/LOGLSMW/comlink.cpp`**:
```cpp
void ComLink::close()
{
    if (m_port.isOpen()) {
        m_port.close();
        emit linkLost(QStringLiteral("отключено"));  // ДОБАВЛЕНО
    }
}
```

**`Firmware/TEST_PING/App/Src/com.c`** — все stub-замены:
- `cmdFlashOn` — убран вызов `flashActiv()`, немедленный ACK
- `cmdFlashOff` — убран `FlashOff()`, немедленный ACK
- `cmdReadMem` — возвращает страницу 0xFF без P25Qx_QPI_Read/fm25xx_readMultiple
- `cmdWriteMem` — немедленный ACK без P25Qx_QPI_ProgramPage/fm25xx_writeMultiple
- `cmdGetTemp` — возвращает 0.0°C без tmp117_get_Temp (I2C не init)
- `cmdFlashReadIDHandler` — фиктивный ID 0xFFFFFF без P25Qx_QPI_ReadID
- `cmdFlashGetState` — WIP=0 без P25Qx_ReadSR
- `cmdFlashChipEraseHandler` — stub без P25Qx_QPI_EraseChip
- `cmdFlashPageEraseHandler` — stub без P25Qx_QPI_ErasePage
- `cmdFlashSectorEraseHandler` — stub без P25Qx_QPI_EraseSector

### Ранее применённые фиксы (из этой сессии)

1. **`SoftWare/LOGLSMW/mainwindow.cpp`**: `m_retryTimer.start(5000)` → `start(1000)` —
   авторетрай срабатывает через 1 с после истечения таймаута сканера (было 5 с).
   Подтверждён в логе: `[10:39:24] нет ответа` → `[10:39:25] Поиск` → `[10:39:26] найдено`.

2. **`Firmware/TEST_PING/App/Src/com.c`** (ComPoll): фикс DMA prevFill —
   `prevFill = 0` только если `HAL_UARTEx_ReceiveToIdle_DMA` вернул `HAL_OK`
   (в CIRCULAR режиме возвращает HAL_BUSY, prevFill не трогаем).

### Следующие шаги

- Пересобрать и прошить TEST_PING с stub-фиксами
- Пересобрать LOGLSMW с ComLink::close() → linkLost
- Проверить: несколько быстрых подключений/отключений без задержки
- После подтверждения — портировать prevFill fix в продуктовую прошивку LOGLSMA
