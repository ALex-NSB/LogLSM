# -*- coding: utf-8 -*-
"""
Генератор тестового дампа Logger v2 на N циклов (по умолчанию 20).
Каждый цикл — минимально 2 страницы: ЗАГОЛОВОК (0xFE) + СТОП (0xF6),
без промежуточных ДАННЫЕ-фреймов (вкладке «Данные» они не нужны для сводки —
см. archiveParseLoggerPage в mainwindow.cpp, который их игнорирует). Формат —
actual/data_format_spec_v1.md.

ЗАГОЛОВОК (10 байт значащих):
  [0]=0xFE [1..4]=ts u32 LE [5]=temperature raw (°C+60) [6]=status
  [7]=sensorType(0) [8]=ODR(0) [9]=FS(0)

СТОП (значащие байты на [0..1] и [242..249]):
  [0]=0xF6 [1]=FS(0) [242..245]=duration u32 LE [246..249]=duration_total u32 LE

Те же формулы прогрессии цикла, что в gen_registrator_dump_20.py (для
согласованности между двумя тестовыми дампами):
  duration: 45,60,75,90 (период 4); gap (простой перед циклом): 120,180,240
  (период 3); temperature: 22..27°C (период 6); status=7 везде.

Запуск: python gen_logger_dump_20.py
Результат: actual/test_dumps/logger_dump.hex (перезаписывает старый
4-страничный дамп на 1 цикл).
"""
import struct
import os

N_CYCLES = 20
TS0 = 1781827200  # 2026-06-19 00:00:00 UTC — та же база, что в RG-дампе

def build_cycles(n: int):
    cycles = []
    ts_end_prev = TS0
    duration_total = 0
    for i in range(n):
        duration = 45 + (i % 4) * 15
        gap      = 0 if i == 0 else 120 + (i % 3) * 60
        ts_start = TS0 if i == 0 else ts_end_prev + gap
        duration_total += duration
        temp_c = 22 + (i % 6)
        cycles.append(dict(i=i, ts_start=ts_start, duration=duration,
                            duration_total=duration_total, temp_c=temp_c))
        ts_end_prev = ts_start + duration
    return cycles

def header_page(ts_start: int, temp_c: int) -> bytes:
    page = bytearray(b'\xFF' * 256)
    page[0] = 0xFE
    page[1:5] = struct.pack('<I', ts_start)
    page[5] = temp_c + 60          # temperature raw
    page[6] = 7                     # status: voltage_zone=7
    page[7] = 0                     # sensorType = DSO
    page[8] = 0                     # ODR exp = 0
    page[9] = 0                     # FS byte = 0
    return bytes(page)

def stop_page(duration: int, duration_total: int) -> bytes:
    page = bytearray(b'\xFF' * 256)
    page[0] = 0xF6
    page[1] = 0                     # FS byte = 0
    page[242:246] = struct.pack('<I', duration)
    page[246:250] = struct.pack('<I', duration_total)
    return bytes(page)

def ihex_line(addr: int, data: bytes) -> str:
    ln = len(data)
    ah, al = (addr >> 8) & 0xFF, addr & 0xFF
    s = ln + ah + al + 0x00 + sum(data)
    cc = (0x100 - (s & 0xFF)) & 0xFF
    return ':%02X%02X%02X%02X%s%02X' % (ln, ah, al, 0x00,
                                         ''.join('%02X' % b for b in data), cc)

def page_lines(addr: int, page: bytes):
    """Записываем только значащие непустые диапазоны страницы (как в первом
    дампе) — несколько HEX-строк вместо одной на 256 байт 0xFF."""
    lines = []
    i = 0
    n = len(page)
    while i < n:
        if page[i] == 0xFF:
            i += 1
            continue
        j = i
        while j < n and page[j] != 0xFF:
            j += 1
        # ограничиваем длину строки 16 байтами (стиль исходного дампа)
        k = i
        while k < j:
            chunk_end = min(k + 16, j)
            lines.append(ihex_line(addr + k, page[k:chunk_end]))
            k = chunk_end
        i = j
    return lines

def main():
    cycles = build_cycles(N_CYCLES)
    out_path = os.path.normpath(os.path.join(os.path.dirname(__file__),
                                              '..', 'actual', 'test_dumps', 'logger_dump.hex'))
    lines = []
    for c in cycles:
        hdr_addr  = 0x0100 + c['i'] * 0x0200
        stop_addr = hdr_addr + 0x0100
        lines += page_lines(hdr_addr,  header_page(c['ts_start'], c['temp_c']))
        lines += page_lines(stop_addr, stop_page(c['duration'], c['duration_total']))
    lines.append(':00000001FF')
    with open(out_path, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('Записано:', out_path, '(%d строк)' % len(lines))
    print()
    print('i  ts_start    duration  duration_total  temp')
    for c in cycles:
        print('%2d %10d  %3d s     %5d s        %d C' % (
            c['i'], c['ts_start'], c['duration'], c['duration_total'], c['temp_c']))

if __name__ == '__main__':
    main()
