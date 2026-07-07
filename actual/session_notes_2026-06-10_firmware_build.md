# Заметки сессии — 10 июня 2026 (часть 3)
## Тема: создание и сборка проекта Firmware/LOGLSMA

Продолжение: `session_notes_2026-06-10_section17.md`

---

## Статус: проект LOGLSMA компилируется ✅

**Build Finished. 0 errors, 0 warnings.**  
RAM: 6600 B / 48 KB (13.4%), FLASH: 47584 B / 256 KB (18.1%)

---

## Что сделано

### Структура проекта
- База: `Firmware/rp_device/` — скопирован и переименован в `Firmware/LOGLSMA/`
- Toolchain: CMake + arm-none-eabi-gcc (не EWARM)
- CubeMX: пользователь создал `.ioc` с нуля под LOGLSMA+MA00, выбрал CMake+GCC
- Имя проекта в STM32CubeIDE: **LOGLSMA** (папка: `Firmware/LOGLSMA/`)

### Структура файлов
```
Firmware/LOGLSMA/
├── Core/Src/        ← CubeMX-генерация (main.c, rtc.c, usart.c, ...)
├── Core/Inc/        ← CubeMX-заголовки (main.h, usart.h, ...)
├── App/Src/         ← пользовательский код (17 файлов из rp_device)
├── App/Inc/         ← пользовательские заголовки
└── CMakeLists.txt
```

### Изменения в CMakeLists.txt
Исключены из сборки (конфликты/дубли):
- `App/Src/command.c` — дублирует `Service()`, `print()`, `send()` из com.c
- `App/Src/rtc.c` — дубль `Core/Src/rtc.c` (уже в cmake/stm32cubemx)

### Новые файлы
- `App/Inc/globals.h` — extern-объявления: `lsm`, `flash`, `flash_powered`, `regist`, `ble_flag`
- `App/Inc/myfunc.h` — объявления `swapBytes`, `bigEndianToInt`, `intToBigEndian`

### Изменения в существующих файлах

**`Core/Src/main.c`** — добавлены глобалы в USER CODE PV:
```c
LSM6DSO_Object_t lsm;
P25Qx_HandleTypeDef flash;
uint8_t flash_powered;
RegistratorData regist;
uint8_t ble_flag;   // ← добавлен
```

**`App/Src/pwr.c`** — добавлены includes: `quadspi.h`, `spi.h`, `i2c.h`, `usart.h`, `p25q128.h`, `fm25xx.h`, `tmp117.h`, `globals.h`

**`App/Src/com.c`** — добавлены: `globals.h`, forward-декларации `FlashOn()`/`FlashOff()`  
(FlashQ.h исключён — дублирует `union StatusRegBits` из p25q128.h)

**`Core/Inc/usart.h`** — добавлены в USER CODE:
```c
extern UART_HandleTypeDef huart1;
void MX_USART1_UART_Init(void);
```

**`Core/Src/usart.c`** — добавлены в USER CODE:
- `UART_HandleTypeDef huart1;`
- `MX_USART1_UART_Init()` — 115200, PA9/PA10
- MSP для USART1: GPIO PA9/PA10 AF7, `__HAL_RCC_USART1_CLK_ENABLE()`
- MspDeInit для USART1

> ⚠️ USART1 добавлен вручную через USER CODE sections — CubeMX его не генерировал.
> При следующей конфигурации .ioc добавить USART1 официально и удалить ручные блоки.

---

## Управление питанием периферии ✅ ПОДТВЕРЖДЕНО

**Правило: OFF = Analog (Hi-Z), НЕ Output-LOW**

| GPIO | Label | ON | OFF |
|---|---|---|---|
| PA8 | QPWR | Output HIGH | Analog (Hi-Z) |
| PB4 | T_PWR | Output HIGH | Analog (Hi-Z) |
| PB5 | M_PWR | Output HIGH | Analog (Hi-Z) |
| PB6 | B_PWON | Output LOW | Analog (Hi-Z) или HIGH |

Обоснование: Output-LOW создаёт активный сток через пин MCU → конденсаторы периферии
разряжаются через MCU-вывод. Analog — Hi-Z, нода держится внешними резисторами.

**Исправлено в `pwr.c`:**
- `framDeactiv()` — теперь FPWR_Pin → Analog (Hi-Z) через `HAL_GPIO_Init`
- `tmp117Deactiv()` — теперь TPWR_Pin → Analog (Hi-Z)

> Примечание: `flashActiv()` / `flashDeactiv()` уже корректны в `FlashQ.c`
> (FlashOff устанавливает GPIO_MODE_ANALOG).

---

## Следующие шаги

1. **Прошить через ST-Link** (XP1, SWD PA13/PA14) — проверить базовый старт
2. **Проверить SERVICE режим** — USART2 921600, команды ping/WHO_AM_I
3. **Добавить USART1 в .ioc** — пересгенерировать CubeMX, убрать ручные блоки из usart.c
4. **Доработать com.c** — заменить старый WAKE-протокол на LTP
5. **CtrlRVZD** — пересобрать Qt-приложение под новым именем

---

## Примечания

- `command.c` — старая версия сервисного обработчика (interrupt-based WAKE).
  Оставлен в репо, исключён из сборки. Может понадобиться как справочник.
- `ble.c` не существует нигде в репо — BLE функции (`BLE_Init`, `BLE_Link` и т.д.)
  нужно написать с нуля.
- Предупреждения компилятора в `com.c` (знаковость `char*`/`uint8_t*`) — косметика,
  не мешают работе.
