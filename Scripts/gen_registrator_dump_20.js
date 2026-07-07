// Генератор тестового дампа Регистратор v2 на N циклов (по умолчанию 20).
// Запуск: node Scripts/gen_registrator_dump_20.js
// (Node.js уже используется в проекте — см. Scripts/build_tz.js в CLAUDE.md,
// поэтому без Python — этот скрипт делает то же самое на JS.)
//
// Записи: 24 байта/запись (пересмотрено 20.06.2026 — было 28, см. ниже),
// упаковка по 10 записей на страницу Flash (256 байт = 240 байт данных +
// 16 байт резерва в хвосте; пересмотрено 20.06.2026 — было 1 запись = 1
// страница), журнал начинается со страницы 0x0100. Формат — actual/data_format_spec_v1.md:
//   [0]  timestamp_start   u32 LE  (якорь, Unix-эпоха)
//   [4]  duration          u32 LE  (конец цикла = timestamp_start + duration,
//                                   отдельно не хранится — timestamp_end убран
//                                   как избыточное поле)
//   [8]  duration_total    u32 LE
//   [12] MAX_vibration     f32 LE
//   [16] MAX_RPM           f32 LE
//   [20] temperature       u8  (raw = °C + 60)
//   [21] status            u8  (биты[2:0] = voltage_zone)
//   [22] CRC16             u16 LE (CCITT: poly=0x1021, init=0xFFFF,
//                                  без reflect, xorout=0x0000, по [0..21])

const fs = require('fs');
const path = require('path');

const N_CYCLES = 20;
const TS0 = 1781827200; // 2026-06-19 00:00:00 UTC

function crc16ccitt(buf) {
  let crc = 0xFFFF;
  for (const b of buf) {
    crc ^= (b << 8) & 0xFFFF;
    for (let k = 0; k < 8; k++) {
      crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF) : ((crc << 1) & 0xFFFF);
    }
  }
  return crc;
}

function buildRecords(n) {
  const records = [];
  let tsEndPrev = TS0;
  let durationTotal = 0;
  for (let i = 0; i < n; i++) {
    const duration = 45 + (i % 4) * 15;        // 45,60,75,90 по кругу
    const gap = i === 0 ? 0 : 120 + (i % 3) * 60; // 120,180,240 по кругу
    const tsStart = i === 0 ? TS0 : tsEndPrev + gap;
    const tsEnd = tsStart + duration;
    durationTotal += duration;
    const maxVibro = Math.round((1.5 + (i % 10) * 0.3) * 100) / 100; // 1.5..4.2 g
    const maxRpm = 60 + (i % 8) * 12;                                 // 60..144
    const tempC = 22 + (i % 6);                                       // 22..27
    const tempRaw = tempC + 60;
    const status = 7;

    const body = Buffer.alloc(22);
    body.writeUInt32LE(tsStart, 0);
    body.writeUInt32LE(duration, 4);
    body.writeUInt32LE(durationTotal, 8);
    body.writeFloatLE(maxVibro, 12);
    body.writeFloatLE(maxRpm, 16);
    body.writeUInt8(tempRaw, 20);
    body.writeUInt8(status, 21);

    const crc = crc16ccitt(body);
    const rec = Buffer.concat([body, Buffer.from([crc & 0xFF, (crc >> 8) & 0xFF])]);

    records.push({ i, tsStart, tsEnd, duration, gap: i === 0 ? 0 : gap,
                    durationTotal, maxVibro, maxRpm, tempC, status, crc, raw: rec });
    tsEndPrev = tsEnd;
  }
  return records;
}

function ihexLine(addr, data) {
  const len = data.length;
  const ah = (addr >> 8) & 0xFF, al = addr & 0xFF;
  let sum = len + ah + al + 0x00;
  for (const b of data) sum += b;
  const cc = (0x100 - (sum & 0xFF)) & 0xFF;
  const hex = Buffer.from(data).toString('hex').toUpperCase();
  const head = [len, ah, al, 0x00].map(v => v.toString(16).toUpperCase().padStart(2, '0')).join('');
  return ':' + head + hex + cc.toString(16).toUpperCase().padStart(2, '0');
}

function main() {
  const records = buildRecords(N_CYCLES);
  const outPath = path.normalize(path.join(__dirname, '..', 'actual', 'test_dumps', 'registrator_dump.hex'));
  const RECORDS_PER_PAGE = 10;
  const lines = [];
  for (const rec of records) {
    const pageIndex = Math.floor(rec.i / RECORDS_PER_PAGE);
    const slotOffset = (rec.i % RECORDS_PER_PAGE) * 24;
    const addr = 0x0100 + pageIndex * 0x0100 + slotOffset;
    lines.push(ihexLine(addr, rec.raw));
  }
  lines.push(':00000001FF');
  fs.writeFileSync(outPath, lines.join('\r\n') + '\r\n');
  console.log('Записано:', outPath);
  console.log();
  console.log('i  ts_start    ts_end      dur  gap  dur_total  vibro  rpm   tempC  CRC');
  for (const rec of records) {
    console.log(
      String(rec.i).padStart(2) + ' ' +
      String(rec.tsStart).padStart(10) + '  ' +
      String(rec.tsEnd).padStart(10) + '  ' +
      String(rec.duration).padStart(3) + '  ' +
      String(rec.gap).padStart(3) + '  ' +
      String(rec.durationTotal).padStart(9) + '  ' +
      rec.maxVibro.toFixed(2).padStart(5) + '  ' +
      rec.maxRpm.toFixed(1).padStart(5) + '  ' +
      String(rec.tempC).padStart(3) + '   0x' +
      rec.crc.toString(16).toUpperCase().padStart(4, '0')
    );
  }
}

main();
