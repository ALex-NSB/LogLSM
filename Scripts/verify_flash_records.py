#!/usr/bin/env python3
"""
verify_flash_records.py — проверка записей регистратора LOGLSMA (Режим 1) в
сыром дампе NOR Flash P25Q128H.

Раскладка записи (24 байта, little-endian) — см. CLAUDE.md
«Организация Flash P25Q128H» / Firmware/LOGLSMA/App/Src/Data.c:

    [0..3]   tsStart       uint32  секунды с 2000-01-01 00:00:00
    [4..7]   duration      uint32  длительность цикла, с
    [8..11]  durationTotal uint32  суммарная наработка (totalSec), с
    [12..15] maxVibro      float   максимальная вибрация (сейчас всегда 0.0)
    [16..19] maxRpm        float   максимальная скорость, об/мин
    [20..21] reserved      0x0000
    [22..23] crc16         CRC16-CCITT по байтам 0..21 (poly=0x1021, init=0xFFFF)

Раскладка Flash: страница 0 = заголовок устройства (не парсится этим
скриптом), лог начинается со страницы 1, 10 записей на страницу (240 из
256 байт используются, остаток — padding).

Использование:
    python3 verify_flash_records.py dump.bin
    python3 verify_flash_records.py dump.bin --start-addr 0x100

dump.bin — сырой бинарный дамп, снятый через «Тест памяти» → «Прочитать»
в LOGLSMW (или через FLASH_READ вручную), начиная со страницы 1
(адрес 0x000100), либо с произвольного адреса, указанного в --start-addr
(должен быть кратен 256 — иначе разбор слотов уедет).

Скрипт также умеет читать текстовый hex-дамп (пробел/перенос строк между
байтами) — передайте файл с расширением .hex или флаг --hex.
"""
import argparse
import struct
import sys
from datetime import datetime, timedelta

PAGE_SIZE = 256
RECORDS_PER_PAGE = 10
RECORD_BYTES = 24
EPOCH_2000 = datetime(2000, 1, 1)


def crc16_ccitt(data: bytes) -> int:
    """poly=0x1021, init=0xFFFF, no reflect, xor=0x0000 — совпадает с ltp_crc16() в ltp.c."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def load_bytes(path: str, as_hex: bool) -> bytes:
    if as_hex or path.lower().endswith((".hex", ".txt")):
        text = open(path, "r", encoding="utf-8", errors="ignore").read()
        tokens = text.replace(",", " ").replace("0x", " ").split()
        return bytes(int(t, 16) for t in tokens)
    return open(path, "rb").read()


def decode_record(raw: bytes, page: int, slot: int):
    ts_start, duration, duration_total = struct.unpack_from("<III", raw, 0)
    max_vibro, max_rpm = struct.unpack_from("<ff", raw, 12)
    stored_crc = struct.unpack_from("<H", raw, 22)[0]
    calc_crc = crc16_ccitt(raw[0:22])

    is_empty = ts_start == 0xFFFFFFFF
    crc_ok = (stored_crc == calc_crc)

    result = {
        "page": page, "slot": slot,
        "empty": is_empty,
        "crc_ok": crc_ok,
        "ts_start_raw": ts_start,
        "duration": duration,
        "duration_total": duration_total,
        "max_vibro": max_vibro,
        "max_rpm": max_rpm,
        "stored_crc": stored_crc,
        "calc_crc": calc_crc,
    }
    if not is_empty:
        try:
            result["ts_start_dt"] = EPOCH_2000 + timedelta(seconds=ts_start)
        except OverflowError:
            result["ts_start_dt"] = None
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="файл с сырым дампом Flash (бинарный или hex-текст)")
    ap.add_argument("--start-addr", type=lambda x: int(x, 0), default=0,
                     help="адрес первого байта дампа относительно чипа (по умолчанию 0 = дамп уже начинается со страницы лога)")
    ap.add_argument("--hex", action="store_true", help="принудительно читать как hex-текст")
    args = ap.parse_args()

    data = load_bytes(args.dump, args.hex)
    if len(data) % PAGE_SIZE != 0:
        print(f"[!] Длина дампа ({len(data)} байт) не кратна {PAGE_SIZE} — "
              f"дамп обрежется по последней полной странице.", file=sys.stderr)

    n_pages = len(data) // PAGE_SIZE
    first_page_num = args.start_addr // PAGE_SIZE

    total = 0
    ok = 0
    bad = 0
    stopped_early = False

    for p in range(n_pages):
        page_off = p * PAGE_SIZE
        page_num = first_page_num + p
        page_empty_at_slot0 = data[page_off:page_off + 4] == b"\xff\xff\xff\xff"

        if page_empty_at_slot0:
            print(f"Страница {page_num}: пуста (0xFFFFFFFF) — конец записанных данных.")
            stopped_early = True
            break

        for slot in range(RECORDS_PER_PAGE):
            off = page_off + slot * RECORD_BYTES
            raw = data[off:off + RECORD_BYTES]
            if len(raw) < RECORD_BYTES:
                break
            rec = decode_record(raw, page_num, slot)
            if rec["empty"]:
                print(f"  Страница {page_num}, слот {slot}: пуст — конец страницы.")
                break

            total += 1
            status = "OK " if rec["crc_ok"] else "CRC-FAIL"
            if rec["crc_ok"]:
                ok += 1
            else:
                bad += 1
            ts_str = rec["ts_start_dt"].strftime("%Y-%m-%d %H:%M:%S") if rec.get("ts_start_dt") else "?"
            print(f"[{status}] стр.{page_num:>3} слот.{slot} | "
                  f"tsStart={ts_str} | duration={rec['duration']:>4}с | "
                  f"total={rec['duration_total']:>6}с | maxVibro={rec['max_vibro']:.3f} | "
                  f"maxRpm={rec['max_rpm']:.1f} | crc=0x{rec['stored_crc']:04X}"
                  + ("" if rec["crc_ok"] else f" (ожидался 0x{rec['calc_crc']:04X})"))

    print()
    print(f"Итого: {total} записей, {ok} с корректным CRC, {bad} с несовпадением CRC.")
    if bad:
        print("⚠ Есть записи с несовпадением CRC — либо старый формат (до 02.07.2026, "
              "без CRC), либо реальное повреждение записи.")
    if not stopped_early and n_pages > 0:
        print("Дамп закончился раньше, чем нашлась пустая страница/слот — "
              "возможно, в дампе не весь диапазон записанных данных.")


if __name__ == "__main__":
    main()
