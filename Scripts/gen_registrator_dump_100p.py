# -*- coding: utf-8 -*-
"""
Генератор БОЛЬШОГО тестового дампа Регистратор v2 — 100 страниц (1000 записей),
данные меняются случайным образом (02.07.2026, для отладки вывода/обработки
и проверки двоичного поиска первой свободной страницы: образ занимает
стр.1..100, значит поиск должен показать первую свободную = 101).

Случайность ВОСПРОИЗВОДИМА: фиксированный сид (можно поменять аргументом
--seed N, число страниц — --pages N) — чтобы при отладке разбора значения
на экране можно было сверять с таблицей, напечатанной этим скриптом.

Формат записи (24 байта) — actual/data_format_spec_v1.md, тот же, что в
gen_registrator_dump_20.py:

  [0]  timestamp_start   u32 LE  (якорь, Unix-эпоха)
  [4]  duration          u32 LE
  [8]  duration_total    u32 LE  (накопительная)
  [12] MAX_vibration     f32 LE
  [16] MAX_RPM           f32 LE
  [20] temperature       u8  (raw = °C + 60)
  [21] status            u8  (биты[2:0] = voltage_zone)
  [22] CRC16             u16 LE (CCITT: 0x1021/0xFFFF, по первым 22 байтам)

Правдоподобие случайных данных:
  duration     — лог-равномерно 30 с .. 2 ч (коротких циклов больше);
  простой      — лог-равномерно 2 мин .. 8 ч;
  MAX_RPM      — 30..320 об/мин (потолок гироскопа A: ±2000 dps ≈ 333);
  MAX_vibr     — лог-равномерно 0.05..8 g (редкие большие удары);
  температура  — случайное блуждание ±3 °C в границах −30..+85;
  voltage_zone — деградация 7→4 по ходу журнала с редкими провалами
                 (семантика «минимум зоны за цикл» из спеки).

Запуск:  python gen_registrator_dump_100p.py [--pages 100] [--seed 20260702]
Результат: actual/test_dumps/registrator_dump_100p.hex (старый
registrator_dump.hex НЕ трогается) + краткая таблица на stdout.
"""
import argparse
import math
import os
import random
import struct

TS0 = 1767225600  # 2026-01-01 00:00:00 UTC — 1000 циклов растянутся до ~июля
RECORDS_PER_PAGE = 10
LOG_START_ADDR = 0x0100   # журнал со страницы 1 (стр.0 — заголовок устройства)


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def log_uniform(rng: random.Random, lo: float, hi: float) -> float:
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


def build_records(n: int, rng: random.Random):
    records = []
    ts = TS0
    duration_total = 0
    temp_c = 15.0
    for i in range(n):
        ts += int(log_uniform(rng, 120, 8 * 3600))          # простой перед циклом
        duration = int(log_uniform(rng, 30, 2 * 3600))
        duration_total += duration

        max_rpm = round(rng.uniform(30.0, 320.0), 1)
        max_vibro = round(log_uniform(rng, 0.05, 8.0), 3)

        temp_c = min(85.0, max(-30.0, temp_c + rng.uniform(-3.0, 3.0)))
        temp_raw = int(round(temp_c)) + 60

        # деградация батареи 7→4 по ходу журнала + редкие провалы на 1 зону
        zone = 7 - int(3 * i / max(1, n - 1))
        if rng.random() < 0.03:
            zone = max(0, zone - 1)
        status = zone & 0x07

        body = struct.pack('<IIIffBB',
                           ts, duration, duration_total,
                           max_vibro, max_rpm, temp_raw, status)
        assert len(body) == 22
        rec = body + struct.pack('<H', crc16_ccitt(body))
        records.append(dict(i=i, ts_start=ts, duration=duration,
                            duration_total=duration_total,
                            max_vibro=max_vibro, max_rpm=max_rpm,
                            temp_c=int(round(temp_c)), zone=status, raw=rec))
        ts += duration
    return records


def ihex_line(addr: int, data: bytes) -> str:
    ln = len(data)
    ah, al = (addr >> 8) & 0xFF, addr & 0xFF
    s = ln + ah + al + 0x00 + sum(data)
    cc = (0x100 - (s & 0xFF)) & 0xFF
    return ':%02X%02X%02X00%s%02X' % (ln, ah, al,
                                      ''.join('%02X' % b for b in data), cc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pages', type=int, default=100)
    ap.add_argument('--seed', type=int, default=20260702)
    args = ap.parse_args()

    n = args.pages * RECORDS_PER_PAGE
    rng = random.Random(args.seed)
    records = build_records(n, rng)

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', 'actual', 'test_dumps', 'registrator_dump_100p.hex'))
    lines = []
    for rec in records:
        page_index = rec['i'] // RECORDS_PER_PAGE
        slot_offset = (rec['i'] % RECORDS_PER_PAGE) * 24
        addr = LOG_START_ADDR + page_index * 0x0100 + slot_offset
        lines.append(ihex_line(addr, rec['raw']))
    lines.append(':00000001FF')
    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')

    days = (records[-1]['ts_start'] + records[-1]['duration'] - TS0) / 86400.0
    print('Записано:', out_path)
    print('Страниц: %d (журнал стр.1..%d), записей: %d, сид: %d' % (
        args.pages, args.pages, n, args.seed))
    print('Диапазон времени: ~%.0f суток от 2026-01-01; наработка: %d с (~%.1f ч)' % (
        days, records[-1]['duration_total'], records[-1]['duration_total'] / 3600.0))
    print('Первая свободная страница после записи образа: %d' % (args.pages + 1))
    print()
    print('    i  ts_start    dur    dur_total  vibro   rpm    tC  zone')
    shown = records[:5] + records[len(records)//2:len(records)//2+3] + records[-5:]
    prev = None
    for rec in shown:
        if prev is not None and rec['i'] != prev + 1:
            print('  ...')
        print('%5d %10d %6d %10d  %6.3f %6.1f  %4d  %d' % (
            rec['i'], rec['ts_start'], rec['duration'], rec['duration_total'],
            rec['max_vibro'], rec['max_rpm'], rec['temp_c'], rec['zone']))
        prev = rec['i']


if __name__ == '__main__':
    main()
