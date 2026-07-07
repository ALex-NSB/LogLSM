# Session Notes — 30.06.2026 (CYCLE_PUSH + правая колонка стенда)

## Что сделано

### Прошивка LOGLSMA (Task #3)

**com.h:**
- `void Service()` → `void Service(RegistratorData *r)` — регистратор передаётся параметром
- Добавлен `void PushCycleRecord(RegistratorData *r)` — отправка CMD=0x20

**com.c:**
- `Service(RegistratorData *r)`: добавлена петля обнаружения вращения прямо внутри
  `while(!QUIT)` — такая же логика state=0/1 как в main.c, но работает пока кабель
  подключён (BENCH-режим). После каждого завершённого цикла вызывает `PushCycleRecord(r)`.
- `PushCycleRecord(RegistratorData *r)`: новая функция. Проверяет `WKUP1_GPIO_Port/Pin`,
  при RESET — возвращается (кабель не подключён). При SET — строит 16-байтный payload
  и вызывает `sendPacket(LTP_DEV_ADDRESS, 0x20, payload, 16)`.
  
  Payload (LE):
  - `[0..5]` = `rot.startTimeStamp` (RTC_DateTime: year,month,day,hour,min,sec)
  - `[6..9]` = duration_s (uint32) = `RTC_SubTimeDateSec(stop, start)` 
  - `[10..13]` = `r->totalSec` (uint32)
  - `[14..15]` = `r->rot.maxRate` (uint16, уже в RPM)

**main.c:**
- Убран `1 ||` из условия входа в Service: теперь
  `if (0 != __HAL_PWR_GET_FLAG(PWR_FLAG_WUF1) || wkup1_pin_set())`
- Обновлён вызов: `Service(&regist)`
- Заменён raw-пуш (старый `send(&head,1); send(&regist.rot, sizeof(RotationData))`)
  на `PushCycleRecord(&regist)` в WORK-ветке

### LOGLSMW (Task #4)

**devicecontroller.h:**
- Добавлена константа `LtpCmd::CYCLE_PUSH = 0x20`
- Добавлен сигнал `void unsolicitedFromReg(quint8 cmd, QByteArray payload)`

**devicecontroller.cpp (`onPacket`):**
- Перехват до SEQ-фильтра: если `addr == m_targetAddr && cmd == CYCLE_PUSH` →
  `emit unsolicitedFromReg(cmd, payload); return;`
  (SEQ у unsolicited push не совпадёт с pending request — старый код дропал бы пакет)

**mainwindow.h:**
- `StendCycleRecord` расширена полями: `hasRegData`, `startReg`, `speedReg`,
  `durationRegS`, `totalRegS`

**mainwindow.cpp:**
- `connect(m_dev, &DeviceController::unsolicitedFromReg, ...)` → вызывает `stendFillRegColumn(payload)`
- `stendFillRegColumn(QByteArray)`: разбирает 16-байтный payload, находит последнюю
  запись истории без `hasRegData`, заполняет поля, вызывает `stendRenderReport()`
- `stendRenderReport()`: переписан — три ветки:
  1. `inProgress=true`: только левый столбик (без рег данных, известна только плановая длительность)
  2. `inProgress=false, hasRegData=false`: только левый столбик (цикл завершён, push ещё не пришёл)
  3. `inProgress=false, hasRegData=true`: обе колонки + дельта (secDelta/об/мин)
  
  Выравнивание: стенд-значение через `leftJustified(13)`, Δ форматируется с русскими
  падежами (секунда/секунды/секунд), скорость: `+N об/мин` или `−N об/мин`.

## Архитектура BENCH vs FIELD

```
BENCH (WKUP1=1 всегда):
  main → Service(&regist) → [ComPoll + вращение в одном цикле]
       → по каждому завершённому циклу: PushCycleRecord() → sendPacket 0x20
  LOGLSMW: onPacket перехватывает CMD=0x20 до SEQ-фильтра → stendFillRegColumn

FIELD (WKUP1=0, автономная батарейка):
  main → WORK-петля → [обнаружение вращения]
       → SaveParamOnEEPROM → PushCycleRecord() (возвращается сразу, кабеля нет)
  После теста: подключить кабель → [вариант 2: чтение Flash вручную — НЕ реализован]
```

## Что НЕ реализовано (следующие задачи)

- `RTC_SubTimeDateSec` в Data.c — возвращает 0 (заглушка). `duration_s` в пуше = 0 пока
  не будет реализована. `total_s` тоже = 0.
- Вариант 2 (FIELD): чтение Flash регистратора + корреляция с журналом стенда по
  порядку записей / `duration_total`.
- Пропуск записи регистратора: обнаружение через сравнение `duration_total` и вставка
  `"— пропуск —"` в правый столбец.

## Потенциальная проблема: конфликт обнаружения вращения

В BENCH-режиме `Service(&regist)` не возвращается (пока WKUP1=1). WORK-петля после неё
в main.c — мёртвый код. Всё обнаружение вращения и пуш происходят внутри Service().

В FIELD-режиме Service() не вызывается, работает WORK-петля в main.c.

Конфликта нет — два пути полностью исключают друг друга через WKUP1.
