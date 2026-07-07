// Генератор тестового дампа Logger v2 на N циклов (по умолчанию 20).
// Запуск: node Scripts/gen_logger_dump_20.js
//
// Каждый цикл — 2 страницы: ЗАГОЛОВОК (0xFE) + СТОП (0xF6), без промежуточных
// ДАННЫЕ-фреймов (archiveParseLoggerPage в mainwindow.cpp их для сводки
// вкладки «Данные» не использует). Формат — actual/data_format_spec_v1.md.
//
// Прогрессия цикла та же, что в gen_registrator_dump_20.js (для согласованности
// между двумя тестовыми дампами): duration 45,60,75,90 (период 4); gap (простой
// перед циклом) 120,180,240 (период 3); temperature 22..27°C (период 6).

const fs = require('fs');
const path = require('path');

const N_CYCLES = 20;
const TS0 = 1781827200; // 2026-06-19 00:00:00 UTC — та же база, что в RG-дампе

function buildCycles(n) {
  const cycles = [];
  let tsEndPrev = TS0;
  let durationTotal = 0;
  for (let i = 0; i < n; i++) {
    const duration = 45 + (i % 4) * 15;
    const gap = i === 0 ? 0 : 120 + (i % 3) * 60;
    const tsStart = i === 0 ? TS0 : tsEndPrev + gap;
    durationTotal += duration;
    const tempC = 22 + (i % 6);
    cycles.push({ i, tsStart, duration, durationTotal, tempC });
    tsEndPrev = tsStart + duration;
  }
  return cycles;
}

function headerPage(tsStart, tempC) {
  const page = Buffer.alloc(256, 0xFF);
  page[0] = 0xFE;
  page.writeUInt32LE(tsStart, 1);
  page[5] = tempC + 60; // temperature raw
  page[6] = 7;          // status: voltage_zone=7
  page[7] = 0;          // sensorType = DSO
  page[8] = 0;          // ODR exp = 0
  page[9] = 0;          // FS byte = 0
  return page;
}

function stopPage(duration, durationTotal) {
  const page = Buffer.alloc(256, 0xFF);
  page[0] = 0xF6;
  page[1] = 0; // FS byte = 0
  page.writeUInt32LE(duration, 242);
  page.writeUInt32LE(durationTotal, 246);
  return page;
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

// Пишем только значащие (не 0xFF) диапазоны страницы — отдельными строками
// не длиннее 16 байт, как в исходном дампе.
function pageLines(addr, page) {
  const lines = [];
  let i = 0;
  const n = page.length;
  while (i < n) {
    if (page[i] === 0xFF) { i++; continue; }
    let j = i;
    while (j < n && page[j] !== 0xFF) j++;
    let k = i;
    while (k < j) {
      const end = Math.min(k + 16, j);
      lines.push(ihexLine(addr + k, page.subarray(k, end)));
      k = end;
    }
    i = j;
  }
  return lines;
}

function main() {
  const cycles = buildCycles(N_CYCLES);
  const outPath = path.normalize(path.join(__dirname, '..', 'actual', 'test_dumps', 'logger_dump.hex'));
  let lines = [];
  for (const c of cycles) {
    const hdrAddr = 0x0100 + c.i * 0x0200;
    const stopAddr = hdrAddr + 0x0100;
    lines = lines.concat(pageLines(hdrAddr, headerPage(c.tsStart, c.tempC)));
    lines = lines.concat(pageLines(stopAddr, stopPage(c.duration, c.durationTotal)));
  }
  lines.push(':00000001FF');
  fs.writeFileSync(outPath, lines.join('\r\n') + '\r\n');
  console.log('Записано:', outPath, '(' + lines.length + ' строк)');
  console.log();
  console.log('i  ts_start    duration  duration_total  temp');
  for (const c of cycles) {
    console.log(
      String(c.i).padStart(2) + ' ' +
      String(c.tsStart).padStart(10) + '  ' +
      String(c.duration).padStart(3) + ' s     ' +
      String(c.durationTotal).padStart(5) + ' s        ' +
      c.tempC + ' C'
    );
  }
}

main();
