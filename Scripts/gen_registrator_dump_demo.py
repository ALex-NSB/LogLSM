# -*- coding: utf-8 -*-
"""
Генератор ДЕМО-дампа Регистратор v2 (48 байт/запись, 5 записей/стр.) с
ОСМЫСЛЕННЫМ профилем — чтобы графики в LOGLSMW читались красиво:
  - скорость: плавный «горный» профиль с плато и явным пиком (не шум);
  - вибрация: ровный базовый УРОВЕНЬ (кан.1), растущий со скоростью, +
    отдельные выраженные УДАРЫ-пики (кан.2) в нескольких циклах — обычные
    данные и пики хорошо разложены и видны по-отдельности;
  - температура: кривая разогрева с выходом на плато.

Формат записи — тот же, что gen_registrator_dump_v2.py (data_format_spec_v1.md
§v2). Совместим с разбором «Данные»/«Образ» в LOGLSMW.

Запуск:  python gen_registrator_dump_demo.py [--pages 50] [--seed 27]
Результат: SoftWare/LOGLSMW/test_dumps/registrator_dump_demo.hex
"""
import argparse
import datetime as _dt
import math
import os
import random
import struct

RECORDS_PER_PAGE = 5
RECORD_BYTES = 48
LOG_START_ADDR = 0x0100
TS0 = int((_dt.datetime(2026, 1, 1) - _dt.datetime(2000, 1, 1)).total_seconds())


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_records(n, rng):
    records = []
    ts = TS0
    duration_total = 0

    # Циклы-«удары» — выраженные пики вибрации кан.2 (события), разложены по
    # прогону, чтобы стояли на фоне ровного уровня и были хорошо видны.
    # Удары — выраженные пики (ряд «пики» = vib1_peak): побольше и повыше,
    # чтобы на фикс-масштабе 16 g явно выстреливали над фоном.
    impacts = {int(n * f) for f in
               (0.07, 0.17, 0.26, 0.35, 0.47, 0.58, 0.68, 0.79, 0.90)}

    speed = 130.0                       # состояние случайного хода скорости
    for i in range(n):
        u = i / max(1, n - 1)                    # позиция в прогоне 0..1

        # ── СКОРОСТЬ: динамичный случайный ход с широким размахом (не «забор»):
        # мягкая общая горка-тренд + сильные перепады цикл-к-циклу.
        trend   = 150.0 + 90.0 * math.sin(math.pi * u)
        speed   = 0.60 * speed + 0.40 * trend + rng.uniform(-75.0, 75.0)
        rpm_avg = round(max(35.0, min(315.0, speed)), 1)
        rpm_max = round(min(320.0, rpm_avg * rng.uniform(1.05, 1.18)), 1)

        # ── ВИБРАЦИЯ (ряд «уровень» = vib1_rms, ряд «пики» = vib1_peak; канал 2
        # на варианте A не отображается — удары кладём в vib1_peak) ──
        # УРОВЕНЬ: идёт за скоростью, но с сильным разбросом — живой, не ровный.
        vib1_rms  = 30.0 + rpm_avg * rng.uniform(0.8, 2.2) + rng.uniform(-40.0, 40.0)
        vib1_rms  = round(max(20.0, vib1_rms), 1)
        # ПИК: низкий подвижный фон + высокие удары 8..15 g в отдельных циклах —
        # резко торчат на фикс-масштабе 16 g.
        if i in impacts:
            vib1_peak = round(rng.uniform(8000.0, 15000.0), 1)
        else:
            vib1_peak = round(vib1_rms * rng.uniform(1.4, 2.4) + rng.uniform(0.0, 250.0), 1)
        vib2_peak = round(vib1_peak * rng.uniform(1.05, 1.4), 1)
        vib2_rms  = round(vib2_peak * rng.uniform(0.10, 0.22), 1)

        # ── ТЕМПЕРАТУРА: плавно вверх к 150 °C, вниз, снова вверх (полтора
        # «горба» за прогон) — диапазон измерения до +150 °C. ──
        temp_c   = 20.0 + 130.0 * (0.5 - 0.5 * math.cos(2.0 * math.pi * 1.5 * u)) + rng.uniform(-2.0, 2.0)
        temp_c   = max(0.0, min(150.0, temp_c))
        temp_raw = int(round(temp_c)) + 60

        # ── Время: широкий разброс интервала и длительности → «активное время»
        # (синий ряд) тоже пляшет, не ровное. ──
        ts += int(rng.uniform(120, 6000))
        duration = int(90 + rpm_avg * rng.uniform(2, 10) + rng.uniform(0, 700))
        duration_total += duration
        status = 0x02 if i in impacts else 0x00     # пометка события-удара

        body = struct.pack('<IIIffffffBBBB',
                           ts, duration, duration_total,
                           round(rpm_max, 1), round(rpm_avg, 1),
                           vib1_peak, vib1_rms, vib2_peak, vib2_rms,
                           temp_raw, status, 2, 0x0A)
        body += b'\x00' * 6
        assert len(body) == 46
        rec = body + struct.pack('<H', crc16_ccitt(body))
        records.append(dict(i=i, ts_start=ts, duration=duration,
                            duration_total=duration_total,
                            rpm_max=round(rpm_max, 1), rpm_avg=round(rpm_avg, 1),
                            v1p=vib1_peak, v1r=vib1_rms, v2p=vib2_peak, v2r=vib2_rms,
                            temp_c=int(round(temp_c)), impact=(i in impacts), raw=rec))
        ts += duration
    return records


def ihex_line(addr, data):
    ln = len(data)
    ah, al = (addr >> 8) & 0xFF, addr & 0xFF
    s = ln + ah + al + 0x00 + sum(data)
    cc = (0x100 - (s & 0xFF)) & 0xFF
    return ':%02X%02X%02X00%s%02X' % (ln, ah, al,
                                      ''.join('%02X' % b for b in data), cc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pages', type=int, default=50)
    ap.add_argument('--seed', type=int, default=27)
    args = ap.parse_args()

    n = args.pages * RECORDS_PER_PAGE
    rng = random.Random(args.seed)
    records = build_records(n, rng)

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', 'SoftWare', 'LOGLSMW', 'test_dumps', 'registrator_dump_demo.hex'))
    lines = []
    for rec in records:
        page_index = rec['i'] // RECORDS_PER_PAGE
        slot_offset = (rec['i'] % RECORDS_PER_PAGE) * RECORD_BYTES
        addr = LOG_START_ADDR + page_index * 0x0100 + slot_offset
        lines.append(ihex_line(addr, rec['raw'][:24]))
        lines.append(ihex_line(addr + 24, rec['raw'][24:]))
    lines.append(':00000001FF')
    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')

    print('Записано:', out_path)
    print('Страниц: %d, записей v2: %d (5/стр.), сид: %d' % (args.pages, n, args.seed))
    print('Циклы-удары (пики кан.2):', sorted(r['i'] for r in records if r['impact']))
    print('    i    rpmA  rpmM     v1r    v1p    v2p   tC  удар')
    for rec in records[::max(1, n // 20)]:
        print('%5d %5.0f %5.0f %7.0f %6.0f %6.0f %4d  %s' % (
            rec['i'], rec['rpm_avg'], rec['rpm_max'],
            rec['v1r'], rec['v1p'], rec['v2p'], rec['temp_c'],
            '◄' if rec['impact'] else ''))


if __name__ == '__main__':
    main()
