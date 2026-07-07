# Заметки сессии — 10 июня 2026
## Тема: подготовка к старту прошивки LOGLSMA

---

## 1. Что выяснили сегодня

### Существующий firmware проект
- Путь: `Firmware/LogLSM/` (уже существует, создан 21 мая)
- Toolchain: **CMake** (не Makefile) — `CMakeLists.txt`, `CMakePresets.json`
- IDE: судя по `.cursorrules` — работали в Cursor
- MCU: STM32L433CCTx, LQFP48 ✓
- **Проект сконфигурирован под LOGLSMB (вариант B)**, не под собранный стенд A:
  - QUADSPI CLK на PA3 (пин 13) → вариант B
  - QPWR на PB10 (GPIO Output) → вариант B
  - SPI3 (PB4/PB5) → вариант B
  - USB (PA11/PA12) → вариант B
  - нет USART2 (PA2/PA3) — сервисный порт варианта A

### Существующий код в App/
- `lsm6dsv_i2c.c` / `.h` — драйвер I2C для LSM6DSV320X (вариант B)
  - `WHO_AM_I_ID = 0x70` — это LSM6DSV, не LSM6DSO (0x6C на собранной плате A)
- `lsm6dsv_data.c` — обработка данных
- `log_ringbuf.c` — кольцевой буфер (готов, можно переиспользовать)
- `main.c` — только инициализация HAL, пустой loop

### Qt приложение CtrlRVZD
- Путь: `SoftWare/CtrlRVZD/`
- Qt5, qmake, QCustomPlot — инструмент отладки для ПК
- Протокол: **WAKE** (реализован в qwake.cpp/hpp) — незакончен (Arrived() пустой, блоки закомментированы)
- Скорость: **57600 бод**, 8N1
- Команды из комментария в Com.h: `PageTest N`, `ChipErase`

### LTP протокол
- Путь: `Doc/LogLSM Transport Protocol/`
- Главный файл: `TMP/LTP_PROTOCOL.md`
- LTP = эволюция WAKE, те же escape-константы (FEND=0xC0 и т.д.)
- Добавлено vs WAKE: **CRC16-CCITT**, **SEQ** (порядковый номер), **FLAGS**
- Формат пакета: `FEND | ADDR | CMD | FLAGS | SEQ | LEN | PAYLOAD | CRC16`
- Архитектура: UART → DMA → Ring Buffer → FSM Parser → Dispatcher → Services
- **Транспортный уровень специфицирован. Таблица команд — только примеры, нужно заполнить.**

---

## 2. Решения принятые сегодня

| Вопрос | Решение |
|---|---|
| IDE | STM32CubeIDE |
| RTOS | Нет — голый HAL (FreeRTOS убивает deep sleep) |
| Toolchain | CMake (как в существующем проекте LOGLSMB) |
| Протокол | **LTP** (не WAKE) — CRC16 обязателен для надёжности |
| Первый стенд | LOGLSMA+MA00 (собран, доступен) |

---

## 3. Что делать завтра

### Шаг 1 — Создать проект LOGLSMA (30 мин)
- STM32CubeIDE → New STM32 Project → STM32L433CCTx → папка `Firmware/LOGLSMA/`
- Toolchain: CMake
- Настроить пины по чек-листу: `actual/cubemx_config_LOGLSMA+MA00.md`

### Шаг 2 — Зафиксировать таблицу LTP команд (до написания кода)
Минимальный набор для старта:

| CMD | Название | Описание |
|---|---|---|
| 0x01 | CMD_PING | Пинг — ответ ACK (проверка связи) |
| 0x02 | CMD_WHO_AM_I | Запрос идентификатора устройства |
| 0x10 | CMD_FLASH_PAGE_TEST | Тест страницы Flash (PageTest N) |
| 0x11 | CMD_FLASH_ERASE | Стереть Flash (ChipErase) |
| 0x20 | CMD_IMU_READ | Чтение данных IMU (WHO_AM_I + accel/gyro) |
| 0xFF | CMD_ERROR | Ошибка / неизвестная команда |

### Шаг 3 — Первая прошивка (первый рабочий день)
Цель: **LTP PING работает через USART2 (XP2, сервисный разъём)**

Последовательность:
1. Инициализация USART2 (57600, 8N1, PA2/PA3)
2. USART RX через прерывание → кольцевой буфер (переиспользовать `log_ringbuf.c`)
3. LTP FSM парсер (7 состояний из спеки)
4. Обработчик CMD_PING → отправить ACK
5. Проверить через CtrlRVZD или любой терминал с ручной отправкой байт

### Шаг 4 — IMU тест (второй рабочий день)
1. Включить QPWR (PA8 HIGH) → инициализировать I2C1
2. Читать WHO_AM_I от LSM6DSO → ожидаем `0x6C`
3. Ответить на CMD_WHO_AM_I через LTP

---

## 4. Ключевые файлы для завтра

| Файл | Зачем |
|---|---|
| `actual/cubemx_config_LOGLSMA+MA00.md` | Чек-лист настройки пинов в CubeMX |
| `actual/A+E1_pinout_from_schematic.md` | Полная распиновка варианта A |
| `Doc/LogLSM Transport Protocol/TMP/LTP_PROTOCOL.md` | Спека LTP — FSM, формат пакета, CRC |
| `Firmware/LogLSM/App/Src/log_ringbuf.c` | Кольцевой буфер — переиспользовать |
| `SoftWare/CtrlRVZD/qwake.cpp` | Референс WAKE/LTP парсера (Qt сторона) |

---

## 5. Важные константы

```c
/* LTP framing */
#define LTP_FEND   0xC0
#define LTP_FESC   0xDB
#define LTP_TFEND  0xDC
#define LTP_TFESC  0xDD

/* CRC16-CCITT: poly=0x1021, init=0xFFFF, no reflect, xor=0x0000 */

/* USART2: 57600 бод, 8N1, PA2(TX)/PA3(RX) */

/* LSM6DSO WHO_AM_I = 0x6C, I2C addr = 0x6A (SA0=GND), HAL addr = 0xD4 */
/* LSM6DSV WHO_AM_I = 0x70 — это для варианта B, на стенде A не используем */
```

---

## 6. Состояние документации (закрыто)

| Документ | Статус |
|---|---|
| `actual/A+E1_pinout_from_schematic.md` | ✅ Закрыт, верифицирован нетлистом |
| `actual/B_pinout_from_schematic.md` | ✅ Закрыт, верифицирован нетлистом |
| `actual/cubemx_config_LOGLSMA+MA00.md` | ✅ Готов к использованию |
| `actual/data_format_spec_v1.md` | ✅ Финальный |
| `Doc/LogLSM Transport Protocol/TMP/LTP_PROTOCOL.md` | ✅ Транспорт готов, команды — завтра |
| `md/LogLSM_TZ_v4_consolidated.docx` | ✅ Актуален |

---

*Стенд: LOGLSMA+MA00 физически собран, доступен для отладки.*
*Программатор: ST-Link через XP1 (SWD).*
*Сервисный порт: XP2 (USART2, PA2/PA3, 57600 бод).*
