# Сессия 10 июня 2026 — LTP и подготовка к отладке

> Читать при старте вместе с предыдущими заметками сессии.

---

## Что сделано в эту сессию

### Qt-приложение LOGLSMW — финальное состояние

**Работающий функционал (проверено в железе):**
- COM-порт: подключение / отключение / обновление списка
- Приём WAKE-пакетов и корректный парсинг ответов:
  - `CMD_GET_DATETIME (0x1B)` → `[DateTime] HH:MM:SS DD/MM/20YY`
  - `CMD_GET_AXES_RAW (0x1A)` → `[IMU] Acc: X=.. Y=.. Z=.. | Gyro: X=.. Y=.. Z=..`
  - `CMD_GET_TEMP_IMU (0x10)` → `[Temp IMU] XX.XX °C`
  - `CMD_GET_TEMP (0x0A)` → `[TMP117] XX.XXXX °C`
  - `CMD_GET_TEMP_CHIP (0x11)` → `[Temp Chip] XX.XX °C`
- Кнопки ручного опроса: DateTime, IMU Raw, Temp LSM, Temp TMP117, Temp STM32
- Периодический опрос (QTimer): чекбоксы LSM / TMP117 / STM32, интервал 100–60000 мс
- Лог выводится на обеих вкладках одновременно (LogText + flashLog)
- `[TX]` логируется только при успешной отправке (`com->send()` возвращает bool)

**Файлы проекта:**
```
SoftWare/LOGLSMW/
├── mainwindow.h / mainwindow.cpp  — вся логика UI и парсинга
├── Com.h / Com.cpp               — COM-порт, send() возвращает bool, isConnected()
├── qwake.hpp / qwake.cpp         — WAKE-парсер (временный, заменяется на LTP)
└── mainwindow.ui                 — вкладка "Команды" с кнопками и polling-секцией
```

### Спецификация LTP — финальная

Документ создан и утверждён:
- `Doc/LogLSM Transport Protocol/LTP_PROTOCOL_v1.0_RU.docx` — **основной (русский)**
- `Doc/LogLSM Transport Protocol/LTP_PROTOCOL_v1.0.docx` — английская версия

13 разделов: введение с историей WAKE, архитектура, framing/stuffing, формат пакета,
FLAGS, SEQ/LEN, CRC16-CCITT (код + тест-вектор 0x29B1), FSM-парсер, таблица команд,
коды ошибок, форматы PAYLOAD, транспорты, WAKE vs LTP сравнение, примеры, чеклист.

---

## Текущее состояние железа

- **LOGLSMA + MA00** — собрана, на стенде, прошита, USART2 921600 бод
- Firmware: `rp_device` (CMake), работает на WAKE-протоколе
- Проверено в этой сессии: DateTime, IMU Raw, Temp LSM, TMP117, Temp STM32 — все отвечают
- TMP117 на плате установлен и работает
- BLE (HLK-B40) пока не тестировался

---

## План на следующую сессию

### 1. Переход на LTP в firmware (rp_device)

Создать `App/Inc/ltp.h` и `App/Src/ltp.c`:

```c
// Минимальный API
void     ltp_parser_init(LtpParser *p);
void     ltp_parser_feed(LtpParser *p, uint8_t byte);  // main loop only
void     ltp_on_packet(LtpPacket *pkt);                // dispatcher callback
uint16_t ltp_crc16(const uint8_t *buf, size_t len);
int      ltp_build(uint8_t *out, uint8_t addr, uint8_t cmd,
                   uint8_t flags, uint16_t seq,
                   const uint8_t *payload, uint16_t plen);
```

Обновить `com.c`:
- Заменить WAKE-сборку пакетов на `ltp_build()`
- Заменить WAKE-парсер на `ltp_parser_feed()` в main loop
- Добавить CRC-проверку входящих пакетов
- Добавить FLAGS.DIR=1 во все ответы устройства
- Добавить SEQ: копировать из запроса в ответ

Добавить `App/Src/ltp.c` в `CMakeLists.txt` → `target_sources`.

### 2. Переход на LTP в Qt (LOGLSMW)

Создать `qltp.hpp` / `qltp.cpp` на основе `qwake.hpp`:

```cpp
class QLtp : public QObject {
    Q_OBJECT
public:
    void feed(uint8_t byte);
    static QByteArray build(uint8_t cmd, uint8_t addr,
                            uint8_t flags = 0, uint16_t seq = 0,
                            const QByteArray &payload = {});
signals:
    void packetReceived(uint8_t addr, uint8_t cmd,
                        uint8_t flags, uint16_t seq, QByteArray payload);
};
```

Обновить `mainwindow.h/cpp`:
- `QWake *wake` → `QLtp *ltp`
- `onPacketReceived(addr, cmd, data)` → `onPacketReceived(addr, cmd, flags, seq, data)`
- Добавить проверку `FLAGS.ERR` перед разбором PAYLOAD
- SEQ: автоинкремент при отправке

Обновить `CMakeLists.txt` / `.pro`: убрать qwake, добавить qltp.

### 3. Отладка LTP в железе

Последовательность:
1. Прошить firmware с LTP
2. Проверить ping (CMD_PING 0x01) — FEND + заголовок 8 байт + CRC
3. Проверить CMD_GET_DATETIME — сравнить с WAKE-версией
4. Проверить CMD_GET_TEMP_IMU в режиме polling (QTimer)
5. Убедиться в корректности CRC: тест-вектор 0x29B1 на "123456789"

### 4. ТЗ на Qt-приложение (LOGLSMW)

Проработать структуру и написать ТЗ. Основные разделы:

**Режимы работы приложения:**
- SERVICE режим: полный набор команд (Flash, IMU, Temp, DateTime)
- MONITOR режим: периодический опрос, графики в реальном времени (QCustomPlot)
- LOG режим: приём и сохранение потоковых данных из Flash

**Каркас окон (скелет UI):**
- `MainWindow` — главное окно с вкладками
  - Вкладка "Подключение" — COM / BLE выбор, статус
  - Вкладка "Команды" — ручной опрос (текущая)
  - Вкладка "Мониторинг" — графики QCustomPlot (Acc X/Y/Z, Gyro X/Y/Z, Temp)
  - Вкладка "Flash" — управление памятью (статус, стирание, дамп)
  - Вкладка "Настройки" — адрес устройства, скорость, интервал опроса
- `AboutDialog` — версия, описание протокола

**Протокол LTP в приложении:**
- QLtp как центральный парсер/сборщик пакетов
- SEQ-матчинг запросов и ответов
- Таймаут ответа (500 мс по умолчанию)
- Отображение ERR-ответов красным в лог

---

## Ключевые константы (не забыть)

```
WAKE_DEVICE_ADDR = 0x8D (ADDR с битом 7=1)
USART2: 921 600 бод (сервисный кабель XP2)
USART1: 115 200 бод (HLK-B40 BLE)
CRC16-CCITT: poly=0x1021, init=0xFFFF, no reflect, xor=0
Тест-вектор CRC: "123456789" -> 0x29B1
LTP заголовок: 8 байт (ADDR+CMD+FLAGS+SEQ[2]+LEN[2])
Stuffing: 0xC0 -> DB DC,  0xDB -> DB DD
CRC: big-endian, покрывает ADDR..PAYLOAD (сырые байты, до stuffing)
```

---

## Файлы для чтения при старте следующей сессии

1. `actual/session_notes_2026-06-10_firmware_build.md` — состояние firmware
2. `actual/session_notes_2026-06-10_ltp_and_software.md` — **этот файл**
3. `Doc/LogLSM Transport Protocol/LTP_PROTOCOL_v1.0_RU.docx` — спека LTP
4. `Firmware/rp_device/App/Src/com.c` — текущий com (WAKE, переписывать на LTP)
5. `SoftWare/LOGLSMW/mainwindow.cpp` — текущее Qt-приложение
