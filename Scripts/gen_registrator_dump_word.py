# -*- coding: utf-8 -*-
"""
Генератор тестового дампа Регистратора — ФОРМАТ «1 цикл = 1 слово» (28.07.2026).

Слово = страница 256 Б. Раскладка:
  [0]        маркёр слова: 0xF5 базовая запись цикла (0xF4 — расширенная, резерв)
  [1..48]    базовая запись (тот же 48-байтный формат v2, что и раньше):
     [1]  tsStart        u32 LE   секунды от 2000-01-01 (rtcToSec прошивки, НЕ Unix!)
     [5]  duration       u32 LE
     [9]  durationTotal  u32 LE
     [13] rpm_max        f32 LE
     [17] rpm_avg        f32 LE
     [21] vib1_peak      f32 LE   мг (канал 1 — уровень)
     [25] vib1_rms       f32 LE   мг
     [29] vib2_peak      f32 LE   мг (канал 2 — удары/jerk)
     [33] vib2_rms       f32 LE   мг
     [37] temperature    u8       raw = °C + 60
     [38] status         u8
     [39] rec_version    u8 = 2
     [40] variant        u8 = 0x0A (вариант A)
     [41..46] reserved   = 0
     [47] CRC16          u16 LE   CCITT 0x1021/0xFFFF по 46 байтам записи (без маркёра)
  [49..255]  0xFF (не используется; на устройстве — стёртая память)

1 запись = 1 страница. Пустая страница ([0]==0xFF) = конец журнала.

Запуск:  python gen_registrator_dump_word.py [--pages 200] [--seed 20260728] [--broken-tail]
Результат: SoftWare/LOGLSMW/test_dumps/registrator_dump_word.hex
"""
import argparse
import datetime as _dt
import math
import os
import random
import struct

RECORD_BYTES = 48
COMPACT_RECS = 5          # уплотнённый: записей на слово
MARK_BASIC = 0xF5         # базовый: 1 запись/слово
MARK_COMPACT = 0xF3       # уплотнённый: до 5 записей/слово
LOG_START_PAGE = 1        # стр.0 служебная
PAGE_BYTES = 256

# База прошивки — 2000-01-01; старт данных образа — 2026-01-01.
TS0 = int((_dt.datetime(2026, 1, 1) - _dt.datetime(2000, 1, 1)).total_seconds())


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def log_uniform(rng, lo, hi):
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


def build_records(n, rng, broken_tail):
    """Возвращает список dict: raw = 48-байтная запись (без маркёра), + поля для печати."""
    records = []
    ts = TS0
    duration_total = 0
    temp_c = 12.0
    for i in range(n):
        # Пауза между циклами — широкий разброс: от коротких до многочасовых.
        ts += int(log_uniform(rng, 60, 12 * 3600))
        # Длительность цикла — от коротышей 15 с до длинных прогонов ~4 ч.
        duration = int(log_uniform(rng, 15, 4 * 3600))
        duration_total += duration

        # Скорость: смесь режимов — иногда тихий ход, иногда у предела A (~320).
        rpm_max = round(rng.uniform(8.0, 320.0), 1)
        rpm_avg = round(rpm_max * rng.uniform(0.55, 0.97), 1)

        # Вибрация, мг: широкий разброс + редкие сильные удары (тяжёлый хвост).
        vib1_peak = round(log_uniform(rng, 20.0, 12000.0), 1)
        vib1_rms  = round(vib1_peak * rng.uniform(0.08, 0.40), 1)
        vib2_peak = round(vib1_peak * rng.uniform(1.05, 3.0), 1)
        vib2_rms  = round(vib2_peak * rng.uniform(0.06, 0.32), 1)

        # Температура: ПЛАВНЫЕ ВОЛНЫ нагрев/остывание по ходу журнала, пики до
        # +150 °C (raw = °C+60 = 210 ≤ 255). Две гладкие синусоиды разной длины +
        # едва заметный шум, чтобы линия дышала, но оставалась плавной.
        w1 = math.sin(2 * math.pi * i / max(1.0, n / 3.5))
        w2 = math.sin(2 * math.pi * i / max(1.0, n / 9.0) + 1.3)
        temp_c = 62.0 + 78.0 * w1 + 12.0 * w2 + rng.uniform(-1.0, 1.0)
        temp_c = min(150.0, max(-40.0, temp_c))
        temp_raw = int(round(temp_c)) + 60
        # voltage_zone (биты[2:0]) — почти всегда «полно» (7), иногда просадка.
        status = 7 if rng.random() > 0.12 else rng.randint(2, 6)

        body = struct.pack('<IIIffffffBBBB',
                           ts, duration, duration_total,
                           rpm_max, rpm_avg,
                           vib1_peak, vib1_rms, vib2_peak, vib2_rms,
                           temp_raw, status, 2, 0x0A)
        body += b'\x00' * 6            # reserved[40..45] записи
        assert len(body) == 46
        rec = body + struct.pack('<H', crc16_ccitt(body))
        records.append(dict(i=i, ts_start=ts, duration=duration,
                            duration_total=duration_total,
                            rpm_max=rpm_max, rpm_avg=rpm_avg,
                            v1p=vib1_peak, v1r=vib1_rms,
                            v2p=vib2_peak, v2r=vib2_rms,
                            temp_c=int(round(temp_c)), status=status,
                            broken=False, raw=rec))
        ts += duration

    # Прерванная питанием последняя запись: ts записан, CRC = 0xFFFF (broken).
    if broken_tail and records:
        r = records[-1]
        r['raw'] = r['raw'][:46] + b'\xFF\xFF'   # CRC поле = 0xFFFF
        r['broken'] = True
    return records


def ihex_data_line(addr, data):
    ln = len(data)
    ah, al = (addr >> 8) & 0xFF, addr & 0xFF
    s = ln + ah + al + 0x00 + sum(data)
    cc = (0x100 - (s & 0xFF)) & 0xFF
    return ':%02X%02X%02X00%s%02X' % (ln, ah, al,
                                      ''.join('%02X' % b for b in data), cc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pages', type=int, default=200,
                    help='число записей = число страниц (1 цикл = 1 слово)')
    ap.add_argument('--seed', type=int, default=20260728)
    ap.add_argument('--broken-tail', action='store_true', default=True,
                    help='последняя запись — прерванная (CRC=0xFFFF)')
    ap.add_argument('--no-broken-tail', dest='broken_tail', action='store_false')
    ap.add_argument('--compact', action='store_true', default=False,
                    help='уплотнённый формат: 5 записей/слово, маркёр 0xF3 (иначе базовый 0xF5, 1/слово)')
    args = ap.parse_args()

    # В базовом --pages = число записей (= число страниц). В уплотнённом на
    # страницу ложится 5 записей, поэтому записей = pages*5.
    compact = args.compact
    per_word = COMPACT_RECS if compact else 1
    mark = MARK_COMPACT if compact else MARK_BASIC
    n = args.pages * per_word
    rng = random.Random(args.seed)
    records = build_records(n, rng, args.broken_tail)

    fname = 'registrator_dump_compact.hex' if compact else 'registrator_dump_word.hex'
    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', 'SoftWare', 'LOGLSMW', 'test_dumps', fname))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    lines = []
    for rec in records:
        word_idx = rec['i'] // per_word          # номер слова (страницы)
        slot     = rec['i'] %  per_word          # слот внутри слова
        page = LOG_START_PAGE + word_idx
        addr = page * PAGE_BYTES + 1 + slot * RECORD_BYTES   # запись в слоте
        if slot == 0:                             # открытие слова — маркёр в [0]
            lines.append(ihex_data_line(page * PAGE_BYTES, bytes([mark])))
        lines.append(ihex_data_line(addr, rec['raw']))       # 48-байтная запись
    lines.append(':00000001FF')

    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')

    print('Записано:', out_path)
    print('Формат: %s, маркёр 0x%02X, %d записей/слово. Слов: %d, записей: %d, сид: %d, broken-tail: %s'
          % ('УПЛОТНЁННЫЙ' if compact else 'БАЗОВЫЙ', mark, per_word,
             args.pages, n, args.seed, args.broken_tail))
    print('    i    page    dur   rpmM  rpmA     v1p     v2p   tC  st  brk')
    shown = records[:6] + records[-4:]
    for rec in shown:
        print('%5d  %5d %6d %6.1f %5.1f %7.1f %7.1f %4d %3d  %s' % (
            rec['i'], LOG_START_PAGE + rec['i'], rec['duration'],
            rec['rpm_max'], rec['rpm_avg'], rec['v1p'], rec['v2p'],
            rec['temp_c'], rec['status'], 'да' if rec['broken'] else '—'))


if __name__ == '__main__':
    main()
