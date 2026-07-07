# CubeMX — конфигурация LOGLSMA+MA00

MCU: **STM32L433CCTx** (LQFP48)  
Источник: `Hardware/LOGLSMA.net` (нетлист, разобран 09.06.2026) + `actual/A+E1_pinout_from_schematic.md`

---

## 1. Выбор MCU

`STM32L433CCTx` → LQFP48

---

## 2. RCC — тактирование

| Параметр | Значение |
|---|---|
| Low Speed Clock (LSE) | Crystal/Ceramic Resonator |
| High Speed Clock (HSE) | Disabled (кварца HSE нет, используем HSI/PLL) |

> Системная частота — от HSI16 через PLL, конкретные множители определить при первой
> прошивке. Для старта достаточно HSI16 = 16 МГц без PLL.

---

## 3. RTC

Включить RTC. Источник — LSE (PC14/PC15, кварц 32.768 кГц уже разведён как BQ1).

---

## 4. QUADSPI — Flash P25Q128H (DD1)

Mode: **1 line** → потом переключить на **4 lines** (Quad SPI) в драйвере.

| Сигнал CubeMX | GPIO | № пина | Метка |
|---|---|---|---|
| QUADSPI_BK1_IO0 | PB1 | 19 | QSPI0 |
| QUADSPI_BK1_IO1 | PB0 | 18 | QSPI1 |
| QUADSPI_BK1_IO2 | PA7 | 17 | QSPI2 |
| QUADSPI_BK1_IO3 | PA6 | 16 | QSPI3 |
| QUADSPI_CLK | PB10 | 21 | QSCK |
| QUADSPI_NCS | PB11 | 22 | QNCS |

**Дополнительно — GPIO Output:**

| GPIO | № пина | Label | Default | Назначение |
|---|---|---|---|---|
| PA8 | 29 | QPWR | **LOW** | Питание Flash (HIGH = вкл) |
| PB15 | 28 | — | Analog | Рудимент QRPWR — не использовать, Analog Hi-Z |

> PA8 = QPWR: держать LOW при старте, перевести HIGH перед инициализацией QUADSPI.
> PB15 (QRPWR): подключён через R6 к цепи QPWR — оставить в **Analog** режиме чтобы избежать
> паразитного тока через R6.

---

## 5. I2C1 — IMU LSM6DSO-MOD (A1)

| Сигнал | GPIO | № пина |
|---|---|---|
| I2C1_SCL | PB8 | 45 |
| I2C1_SDA | PB9 | 46 |

Speed Mode: Fast Mode (400 кГц), подтяжки R2/R3 (4k7) уже на плате.

---

## 6. I2C2 — термодатчик TMP117AIDRV (DA.1 на MA00)

| Сигнал | GPIO | № пина |
|---|---|---|
| I2C2_SCL | PB13 | 26 |
| I2C2_SDA | PB14 | 27 |

Speed Mode: Fast Mode (400 кГц), подтяжки R7*/R8* на плате MA00.

---

## 7. SPI1 — FRAM FM25V10A-G (DD.2 на MA00)

NSS — программный (GPIO Output), не аппаратный.

| Сигнал | GPIO | № пина | Метка |
|---|---|---|---|
| SPI1_MISO | PA11 | 32 | — |
| SPI1_MOSI | PA12 | 33 | — |
| SPI1_SCK | PB3 | 39 | — |
| GPIO Output (NSS) | PA15 | 38 | M_NSS |

SPI Mode: Full-Duplex Master, NSS = Software.  
FRAM FM25V10A-G: CPOL=0, CPHA=0 (Mode 0), до 40 МГц.

---

## 8. USART1 — BLE модуль HLK-B40 (DD.1 на MA00)

| Сигнал | GPIO | № пина |
|---|---|---|
| USART1_TX | PA9 | 30 |
| USART1_RX | PA10 | 31 |

Mode: Asynchronous. **Baud rate: 115200 бод** (HLK-B40 по умолчанию, подтверждено даташитом).  
BLE-эфир ≈ 30–50 KB/s, 115200 не является узким местом. Смена скорости — AT-командой.

---

## 9. USART2 — сервисный интерфейс (XP2)

| Сигнал | GPIO | № пина |
|---|---|---|
| USART2_TX | PA2 | 12 |
| USART2_RX | PA3 | 13 |

Mode: Asynchronous. **Baud rate: 921600 бод** — быстрая выгрузка Flash по проводу (~3 мин на 16 МБ).  
Альтернатива: LPUART1 на тех же пинах (AF8) — для пробуждения из Stop режима по UART.

---

## 10. GPIO — управление питанием периферии

| GPIO | № пина | Label | Вкл (ON) | Выкл (OFF) | Назначение |
|---|---|---|---|---|---|
| PA8 | 29 | QPWR | Output HIGH | **Analog (Hi-Z)** | Flash P25Q128H |
| PB4 | 40 | T_PWR | Output HIGH | **Analog (Hi-Z)** | TMP117 |
| PB5 | 41 | M_PWR | Output HIGH | **Analog (Hi-Z)** | FRAM |
| PB6 | 42 | B_PWON | Output LOW | Analog (Hi-Z) или HIGH | BLE (BSS215P через P-канальный транзистор + делитель R1/R2 на VDD) |

> **⚠️ Правило управления питанием:**  
> **OFF = Analog (Hi-Z), не Output-LOW.**  
> Output-LOW — активный сток: конденсаторы питания периферии разряжаются через вывод MCU.  
> Analog — вывод Hi-Z, нода питания держится внешними резисторами схемы.  
>
> **B_PWON (PB6):** управление через транзистор, затвор BSS215P подтянут к VDD через R1.  
> При Hi-Z и при LOW транзистор закрыт, затвор = VDD → p-FET OFF. Оба варианта приемлемы.
>
> **В коде — два состояния вывода:**
> ```c
> /* Выключить периферию (QPWR, T_PWR, M_PWR, B_PWON) */
> GPIO_InitTypeDef g = {PIN, GPIO_MODE_ANALOG, GPIO_NOPULL, 0};
> HAL_GPIO_Init(PORT, &g);
>
> /* Включить периферию */
> GPIO_InitTypeDef g = {PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW};
> HAL_GPIO_Init(PORT, &g);
> HAL_GPIO_WritePin(PORT, PIN, GPIO_PIN_SET);  /* HIGH для QPWR/T_PWR/M_PWR/B_PWON */
> ```
>
> **CubeMX:** стартовое состояние всех power-enable пинов — Output LOW (минимально безопасное).  
> В runtime при отключении — переключать в Analog программно через power_manager.  
>
> **При старте все периферийные узлы обесточены. Power-on последовательность — в `power_manager`.**

---

## 11. GPIO — прерывания и входы

| GPIO | № пина | Label | Режим | Назначение |
|---|---|---|---|---|
| PC13 | 2 | IM_INT1 | GPIO_EXTI13, falling, pull-down | INT1 от IMU → WKUP2 |
| PA0 | 10 | SR_UP1 | GPIO_Input, pull-down | WKUP1 — детект внешнего питания (+VEX на XP2.4) |
| PB12 | 25 | B_LINK | GPIO_Input, no pull | Статус/линк BLE модуля |
| PA4 | 14 | — | GPIO_Input, no pull | ALRT TMP117 — трасса разведена, **не обрабатывать** |

---

## 12. GPIO — управление BLE

| GPIO | № пина | Label | Режим | Default | Назначение |
|---|---|---|---|---|---|
| PB7 | 43 | B_RES | GPIO_Output | **LOW** (сброс) | BLE reset (активный LOW) |
| PA5 | 15 | B_KEY | GPIO_Output | **HIGH** | BLE mode key |

---

## 13. Не подключены — оставить по умолчанию (Analog)

| GPIO | № пина | Причина |
|---|---|---|
| PH0 | 5 | Не подключён — площадка |
| PH1 | 6 | Не подключён — площадка |
| PA1 | 11 | Не подключён (LED HL1 только в варианте B) |

---

## 14. SYS — отладка

| Параметр | Значение |
|---|---|
| Debug | Serial Wire (SWD) |
| SWDIO | PA13 (пин 34) — назначается автоматически |
| SWCLK | PA14 (пин 37) — назначается автоматически |

---

## 15. Итоговый список используемых периферийных блоков CubeMX

- [x] RCC — LSE Crystal
- [x] RTC
- [x] QUADSPI
- [x] I2C1
- [x] I2C2
- [x] SPI1
- [x] USART1
- [x] USART2
- [x] SYS → Serial Wire

---

## 16. Порядок проверки после генерации

1. Пересмотреть пины QUADSPI: убедиться что PB1/PB0/PA7/PA6 назначены как BK1_IO0-IO3
2. PA8 (QPWR) — GPIO Output, default LOW ✓
3. PB15 — Analog ✓ (не GPIO Output!)
4. PB6 (B_PWON) — default HIGH (BLE обесточен при старте) ✓
5. PA13/PA14 — SWD (не трогать)
6. Все power-enable пины (PB4, PB5, PA8) — default LOW ✓

---

*Источник: `Hardware/LOGLSMA.net`, `actual/A+E1_pinout_from_schematic.md`*  
*Вариант B (LOGLSMB+MB00) — отдельный .ioc файл, аналогично на основе `actual/B_pinout_from_schematic.md`*
