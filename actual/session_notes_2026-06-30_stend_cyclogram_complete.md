# Session Notes — 30.06.2026 (вечер) — Стенд: циклограмма завершена

## Статус на конец сессии

Все 4 режима циклограммы стенда работают. CMD_CYCLE_PUSH получает реальные данные
(время, интервал, скорость). Правая колонка «Регистратор» в cmpReport заполняется.

---

## Что было сделано

### 1. Исправлен порядок push/reset в `com.c`

**Симптом:** maxRate в CYCLE_PUSH всегда 0.  
**Причина:** `r->rot.maxRate = 0` выполнялась ДО `PushCycleRecord(r)`.  
**Исправлено:** swap порядка — сначала push, потом reset.

```c
/* Файл: Firmware/LOGLSMA/App/Src/com.c */
PushCycleRecord(r);          // сначала пушим (maxRate актуален)
r->state = 0;
r->rot.maxRate = r->rot.maxVibr = 0;  // потом сбрасываем
```

### 2. Poll_Sensor: 10 сэмплов → 3 (`Data.c`)

**Симптом:** ComPoll() не успевал обрабатывать LTP во время блокировки Poll_Sensor.  
**Причина:** 10 сэмплов × 80 мс (ODR=12.5 Гц) = 800 мс блокировки.  
**Исправлено:** 3 сэмпла = 240 мс.

### 3. Исправлен баг `stendPickNextSpeed` в `mainwindow.cpp`

**Симптом:** режим «Только +» выдавал скорости 7, 22 об/мин при диапазоне 100–200.  
**Причина:** `bounded(prev+1, hi+1)` при prev=0 давал [1, hi] вместо [lo, hi].  
**Исправлено:**
```cpp
return QRandomGenerator::global()->bounded(qMax(lo, prev + 1), hi + 1);
```

### 4. Убран лишний STEND_START перед STEND_SPEED

**Симптом:** двойная команда вызывала некорректное поведение мотора.  
**Исправлено:** `sendStendCmd(LtpCmd::STEND_START)` убран из `stendBeginWork()` и
`stendNextNStartStop()` — STEND_SPEED сам запускает PWM если таймер остановлен.

### 5. Prescaler off-by-one в `StepDriver.c` (стенд)

**Симптом:** скорость стенда ~6.25% выше заданной.  
**Причина:** HAL хранит prescaler как N-1, код делил на N-1 вместо N.  
**Исправлено:**
```c
uint32_t period = GetTimFreq(&htim1) / (htim1.Init.Prescaler + 1) / f;
```
После перешивки Nucleo скорость совпала с регистратором в пределах 1–3%.

### 6. Реализован `RTC_SubTimeDateSec` (был заглушкой, возвращал 0)

**Файл:** `Firmware/LOGLSMA/App/Src/Data.c`

```c
static uint32_t rtcToSec(const RTC_DateTime *dt)
{
  static const uint8_t kDpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t  yy     = dt->year;
  uint8_t  isLeap = (yy % 4U == 0U) ? 1U : 0U;
  uint32_t days   = (uint32_t)yy * 365U;
  if (yy > 0U) days += (uint32_t)((yy - 1U) / 4U) + 1U;
  for (uint8_t m = 1U; m < dt->month; m++) {
    days += kDpm[m - 1U];
    if (m == 2U && isLeap) days += 1U;
  }
  days += (uint32_t)dt->day - 1U;
  return days * 86400U
       + (uint32_t)dt->hour    * 3600U
       + (uint32_t)dt->minutes *   60U
       + (uint32_t)dt->seconds;
}

uint32_t RTC_SubTimeDateSec(RTC_DateTime *dt1, RTC_DateTime *dt2)
{
  uint32_t s1 = rtcToSec(dt1);
  uint32_t s2 = rtcToSec(dt2);
  return (s1 >= s2) ? (s1 - s2) : 0U;
}
```

**Важно:** если RTC не синхронизирован ДО начала теста, и синхронизация происходит
в середине цикла — `totalSec` получит мусорное значение (~15 часов вместо секунд).
**Правило:** всегда подключаться в LOGLSMW (синхронизация RTC) ПЕРЕД запуском стенда.

### 7. Дельта скорости → проценты в `mainwindow.cpp`

```cpp
const double dSpeedPct = r.speedStend > 0
    ? double(r.speedReg - r.speedStend) / r.speedStend * 100.0
    : 0.0;
const QString dSpeedStr = dSpeedPct >= 0
    ? QStringLiteral("+%1%").arg(dSpeedPct, 0, 'f', 1)
    : QStringLiteral("−%1%").arg(-dSpeedPct, 0, 'f', 1);
```

### 8. GYRO_THRESHOLD: 1000 → 600 (`Data.h`)

| Порог | Мин. скорость детекта |
|---|---|
| 1000 LSB | ~11.7 об/мин |
| 800 LSB | ~9.3 об/мин |
| **600 LSB** | **~7 об/мин** (текущее) |

Формула: threshold × 0.070 dps/LSB × 60/360 = об/мин.  
Шум LSM6DSO в покое: 3–5 LSB — риск ложных срабатываний минимален.

### 9. Дебаунс завершения цикла в `com.c`

**Симптом:** одиночный шумовой провал ниже GYRO_THRESHOLD прерывал цикл досрочно.
При 10–13 об/мин (сигнал ~857 LSB, порог 600) Интервал показывал 0–1 сек при
реальных 4–5 сек.  
**Исправлено:** требуем 3 подряд «нет детекта» перед завершением цикла (~720 мс).

```c
/* в RegistratorData (Data.h): */
uint8_t noDetectCount;

/* в com.c, ветка state==1: */
if (RotationDetected(r)) {
    r->noDetectCount = 0;
    HandleSensorData(r);
} else {
    r->noDetectCount++;
    if (r->noDetectCount >= 3) {
        /* Цикл завершён */
        RTC_GetTimeDate(&r->rot.stopTimeStamp);
        r->totalSec += RTC_SubTimeDateSec(...);
        PushCycleRecord(r);
        r->state = 0;
        r->rot.maxRate = r->rot.maxVibr = 0;
    }
}
```

**Побочный эффект:** stopTimeStamp записывается в момент 3-го пропуска, а не реального
останова мотора → Интервал от регистратора на ~720 мс длиннее, чем от стенда.
На практике: +1–2 сек к Интервалу. Приемлемо.

---

## Текущие параметры прошивки LOGLSMA

| Параметр | Значение |
|---|---|
| GYRO_THRESHOLD | 600 LSB (~7 об/мин мин. детект) |
| Poll_Sensor сэмплов | 3 (240 мс на вызов) |
| noDetectCount порог | 3 (≈720 мс дебаунс) |
| USART2 бодрейт | 921600 |
| Service() | всегда (if(1 ||...)) — отладочный режим |

---

## Результаты тестирования режимов стенда

### Только + (100–300 об/мин) — ✅ подтверждён
- Скорость: 162→153, 246→230, 278→263 (до prescaler-фикса, ~6% систематика)
- После prescaler-фикса стенда: расхождение ~1–3%
- Интервал: совпадение 0 секунд

### N старт/стоп (10–20 об/мин) — ✅ работает
- При синхронизированном RTC: Интервал ±1–2 сек (дебаунс)
- Скорость: +0.0% при 13–14 об/мин

### Только − — протестирован ✅
### Случайно ± — протестирован ✅

---

## Известные ограничения

1. **Интервал регистратора на ~720 мс длиннее** из-за дебаунса. Это нормально —
   дебаунс нужен для надёжности. При необходимости снизить до 2 пропусков (~480 мс).

2. **Общая (totalSec)** накапливается с момента включения устройства, не сбрасывается
   при переподключении LOGLSMW. После power cycle сбрасывается в 0.

3. **Артефакт sync-mid-cycle:** если синхронизация RTC происходит в середине цикла,
   `totalSec` получает ~15 часов мусора. Правило: синхронизация ДО теста.

4. **Низкие скорости (< ~10 об/мин):** детект ненадёжный — сигнал на уровне шума.
   GYRO_THRESHOLD=600 даёт порог ~7 об/мин, ниже — случайные срабатывания.

---

## Файлы изменены в этой сессии

| Файл | Изменение |
|---|---|
| `Firmware/LOGLSMA/App/Src/com.c` | push перед reset, дебаунс noDetectCount |
| `Firmware/LOGLSMA/App/Src/Data.c` | Poll_Sensor 3 сэмпла, RTC_SubTimeDateSec реализован |
| `Firmware/LOGLSMA/App/Inc/Data.h` | GYRO_THRESHOLD=600, noDetectCount в структуре |
| `Stend/Stend LOGLSM/Core/Src/StepDriver.c` | Prescaler+1 fix |
| `SoftWare/LOGLSMW/mainwindow.cpp` | stendPickNextSpeed fix, убран STEND_START, дельта % |

---

## Что осталось (не в этой сессии)

- Симуляция в cmpReport (заглушка «об/мин» в тексте, не проценты) — косметика
- Чтение данных регистратора после цикла (chkStendNoRead)
- Запись журнала испытания на диск
- Реализация `SaveTotalSec`/`LoadTotalSec` (FRAM) для сохранения totalSec через питание
- iNemo FSM (отдельная подзадача, см. `iNemo/CLAUDE.md`)
