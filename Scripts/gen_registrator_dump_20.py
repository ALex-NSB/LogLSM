# -*- coding: utf-8 -*-
"""
Генератор тестового дампа Регистратор v2 на N циклов (по умолчанию 20).
Записи: 24 байта/запись (пересмотрено 20.06.2026 — было 28, см. ниже),
упаковка по 10 записей на страницу Flash (256 байт = 240 байт данных + 16
байт резерва в хвосте; пересмотрено 20.06.2026 — было 1 запись = 1 страница),
журнал начинается со страницы 0x0100 (страница 0 — заголовок устройства, см.
CLAUDE.md, "Организация Flash"). Формат записи — actual/data_format_spec_v1.md:

  [0]  timestamp_start   u32 LE  (якорь, Unix-эпоха)
  [4]  duration          u32 LE  (конец цикла = timestamp_start + duration,
                                  отдельно не хранится — timestamp_end убран
                                  как избыточное поле)
  [8]  duration_total    u32 LE
  [12] MAX_vibration     f32 LE
  [16] MAX_RPM           f32 LE
  [20] temperature       u8  (raw = °C + 60)
  [21] status            u8  (биты[2:0] = voltage_zone)
  [22] CRC16             u16 LE (CCITT: poly=0x1021, init=0xFFFF, без
                                 reflect, xorout=0x0000, по первым 22 байтам)

В отличие от первого тестового дампа (3 записи, CRC16-плейсхолдер 0x0000 —
см. старый actual/test_dumps/README.md), здесь CRC16 считается реально:
парсер LOGLSMW его не проверяет (см. известные пробелы, обсуждённые в чате),
но дамп от этого не должен быть менее аккуратным, чем спека требует.

Запуск: python gen_registrator_dump_20.py
Результат: actual/test_dumps/registrator_dump.hex (перезаписывает старый
3-цикловый дамп) + печатает таблицу значений на стандартный вывод (для
README).
"""
import struct
import os

N_CYCLES = 20
TS0 = 1781827200  # 2026-06-19 00:00:00 UTC — та же база, что в первом дампе

def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def build_records(n: int):
    records = []
    ts_end_prev = TS0
    duration_total = 0
    for i in range(n):
        duration = 45 + (i % 4) * 15          # 45,60,75,90 повторяясь
        gap      = 120 + (i % 3) * 60         # 120,180,240 повторяясь (простой между циклами)
        ts_start = TS0 if i == 0 else ts_end_prev + gap
        ts_end   = ts_start + duration
        duration_total += duration
        max_vibro = round(1.5 + (i % 10) * 0.3, 2)     # 1.5..4.2 g, период 10
        max_rpm   = float(60 + (i % 8) * 12)            # 60..144 об/мин, период 8
        temp_c    = 22 + (i % 6)                         # 22..27 °C
        temp_raw  = temp_c + 60
        status    = 7                                    # voltage_zone=7 везде

        body = struct.pack('<IIIffBB',
                            ts_start, duration, duration_total,
                            max_vibro, max_rpm, temp_raw, status)
        assert len(body) == 22
        crc = crc16_ccitt(body)
        rec = body + struct.pack('<H', crc)
        assert len(rec) == 24
        records.append(dict(i=i, ts_start=ts_start, ts_end=ts_end, duration=duration,
                             gap=gap if i > 0 else 0, duration_total=duration_total,
                             max_vibro=max_vibro, max_rpm=max_rpm, temp_c=temp_c,
                             status=status, crc=crc, raw=rec))
        ts_end_prev = ts_end
    return records

def ihex_line(addr: int, data: bytes) -> str:
    ln = len(data)
    ah, al = (addr >> 8) & 0xFF, addr & 0xFF
    rec_type = 0x00
    s = ln + ah + al + rec_type + sum(data)
    cc = (0x100 - (s & 0xFF)) & 0xFF
    hexdata = ''.join('%02X' % b for b in data)
    return ':%02X%02X%02X%02X%s%02X' % (ln, ah, al, rec_type, hexdata, cc)

def main():
    records = build_records(N_CYCLES)
    out_path = os.path.join(os.path.dirname(__file__), '..', 'actual', 'test_dumps', 'registrator_dump.hex')
    out_path = os.path.normpath(out_path)
    RECORDS_PER_PAGE = 10
    lines = []
    for rec in records:
        page_index = rec['i'] // RECORDS_PER_PAGE
        slot_offset = (rec['i'] % RECORDS_PER_PAGE) * 24
        addr = 0x0100 + page_index * 0x0100 + slot_offset
        lines.append(ihex_line(addr, rec['raw']))
    lines.append(':00000001FF')
    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('Записано:', out_path)
    print()
    print('i  ts_start    ts_end      dur  gap  dur_total  vibro  rpm   tempC  CRC')
    for rec in records:
        print('%2d %10d  %10d  %3d  %3d  %9d  %5.2f  %5.1f  %3d   0x%04X' % (
            rec['i'], rec['ts_start'], rec['ts_end'], rec['duration'], rec['gap'],
            rec['duration_total'], rec['max_vibro'], rec['max_rpm'], rec['temp_c'], rec['crc']))

if __name__ == '__main__':
    main()
