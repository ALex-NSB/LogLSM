# -*- coding: utf-8 -*-
"""
Генератор тестового дампа Регистратор **v2** (48 байт/запись, 5 записей/стр.)
с ДВУМЯ каналами вибрации (vib1 = общий уровень, vib2 = удары/jerk; на
варианте B это два физических акселерометра). 18.07.2026.

Формат записи v2 (data_format_spec_v1.md §v2):
  [0]  tsStart        u32 LE   секунды от 2000-01-01 (rtcToSec прошивки,
                               НЕ Unix! старые генераторы писали Unix-эпоху —
                               из-за этого в LOGLSMW даты показывались 2056 г.)
  [4]  duration       u32 LE
  [8]  durationTotal  u32 LE   накопительная
  [12] rpm_max        f32 LE
  [16] rpm_avg        f32 LE
  [20] vib1_peak      f32 LE   мг (канал 1 — уровень)
  [24] vib1_rms       f32 LE   мг
  [28] vib2_peak      f32 LE   мг (канал 2 — удары/jerk, обычно выше кан.1)
  [32] vib2_rms       f32 LE   мг
  [36] temperature    u8       raw = °C + 60
  [37] status         u8
  [38] rec_version    u8 = 2
  [39] variant        u8 = 0x0A (вариант A)
  [40..45] reserved   = 0
  [46] CRC16          u16 LE   CCITT 0x1021/0xFFFF по первым 46 байтам

Запуск:  python gen_registrator_dump_v2.py [--pages 100] [--seed 20260718]
Результат: SoftWare/LOGLSMW/test_dumps/registrator_dump_v2.hex
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


def build_records(n, rng):
    records = []
    ts = TS0
    duration_total = 0
    temp_c = 15.0
    for i in range(n):
        ts += int(log_uniform(rng, 120, 8 * 3600))
        duration = int(log_uniform(rng, 30, 2 * 3600))
        duration_total += duration

        rpm_max = round(rng.uniform(30.0, 320.0), 1)
        rpm_avg = round(rpm_max * rng.uniform(0.60, 0.95), 1)

        # Вибрация, мг: канал 2 (удары) обычно выше канала 1 (уровень);
        # RMS — доля пика (реалистичное соотношение).
        vib1_peak = round(log_uniform(rng, 50.0, 6000.0), 1)
        vib1_rms  = round(vib1_peak * rng.uniform(0.10, 0.35), 1)
        vib2_peak = round(vib1_peak * rng.uniform(1.1, 2.5), 1)
        vib2_rms  = round(vib2_peak * rng.uniform(0.08, 0.30), 1)

        temp_c = min(85.0, max(-30.0, temp_c + rng.uniform(-3.0, 3.0)))
        temp_raw = int(round(temp_c)) + 60
        status = 0

        body = struct.pack('<IIIffffffBBBB',
                           ts, duration, duration_total,
                           rpm_max, rpm_avg,
                           vib1_peak, vib1_rms, vib2_peak, vib2_rms,
                           temp_raw, status, 2, 0x0A)
        body += b'\x00' * 6            # reserved[40..45]
        assert len(body) == 46
        rec = body + struct.pack('<H', crc16_ccitt(body))
        records.append(dict(i=i, ts_start=ts, duration=duration,
                            duration_total=duration_total,
                            rpm_max=rpm_max, rpm_avg=rpm_avg,
                            v1p=vib1_peak, v1r=vib1_rms,
                            v2p=vib2_peak, v2r=vib2_rms,
                            temp_c=int(round(temp_c)), raw=rec))
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
    ap.add_argument('--pages', type=int, default=100)
    ap.add_argument('--seed', type=int, default=20260718)
    args = ap.parse_args()

    n = args.pages * RECORDS_PER_PAGE
    rng = random.Random(args.seed)
    records = build_records(n, rng)

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', 'SoftWare', 'LOGLSMW', 'test_dumps', 'registrator_dump_v2.hex'))
    lines = []
    for rec in records:
        page_index = rec['i'] // RECORDS_PER_PAGE
        slot_offset = (rec['i'] % RECORDS_PER_PAGE) * RECORD_BYTES
        # запись 48 байт → две ihex-строки по 24 байта (макс 255, но короче — читаемее)
        addr = LOG_START_ADDR + page_index * 0x0100 + slot_offset
        lines.append(ihex_line(addr, rec['raw'][:24]))
        lines.append(ihex_line(addr + 24, rec['raw'][24:]))
    lines.append(':00000001FF')
    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')

    print('Записано:', out_path)
    print('Страниц: %d, записей v2: %d (5/стр.), сид: %d' % (args.pages, n, args.seed))
    print('    i     dur   rpmM  rpmA    v1p    v1r    v2p    v2r   tC')
    shown = records[:5] + records[-3:]
    for rec in shown:
        print('%5d %6d %6.1f %5.1f %6.1f %6.1f %6.1f %6.1f %4d' % (
            rec['i'], rec['duration'], rec['rpm_max'], rec['rpm_avg'],
            rec['v1p'], rec['v1r'], rec['v2p'], rec['v2r'], rec['temp_c']))


if __name__ == '__main__':
    main()
