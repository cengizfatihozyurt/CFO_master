#!/usr/bin/env node
/*
 * Deney analizi: Node-RED "Deney Kaydi" panelindeki isaretleri ESP32'nin anomali
 * olaylariyla eslestirir ve basarim olcutlerini hesaplar.
 *
 * Kullanim (proje klasorunde):
 *   node deney/analiz.js                  tum deneyler (no >= 900 test sayilir, dahil edilmez)
 *   node deney/analiz.js --deney 3        yalnizca 3 numarali deney
 *   node deney/analiz.js --kesinti 5      5 numarali kesinti deneyinin veri butunlugu raporu
 *   node deney/analiz.js --test           test deneylerini (>= 900) de dahil et
 *
 * Cikti: deney/sonuclar/denemeler.csv, yanlis_alarmlar.csv, ozet.md (+ kesinti_<no>.md)
 *
 * Veri kaynaklari:
 *   InfluxDB  experiment (isaretler, ms), event (ESP32 olaylari, ESP32 saati, s)
 *   ESP32     /log.csv ve /events (kesinti raporu, cihazin yerel kaydi)
 *   Bulut API /api/v1/telemetry ve /api/v1/events (received_at ile)
 *
 * Ortam degiskenleri (yoksa yerel dosyalardan okunur):
 *   INFLUX_URL (http://localhost:8086), INFLUX_ORG (stm32iot), INFLUX_BUCKET (sensor), INFLUX_TOKEN
 *   ESP32_HOST (yoksa MQTT info mesajindan), API_URL, RENDER_VIEW_KEY, API_DEVICE (cihaz01)
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const OUT_DIR = path.join(__dirname, 'sonuclar');

const ANOMALY = ['A01', 'A02', 'A03', 'A04'];
const NAMES = {
  A01: 'Ani darbe', A02: 'Duruşta anormal titreşim', A03: 'Anormal sefer', A04: 'Hızlı ısınma',
  NORMAL: 'Uzun süreli normal çalışma', KESINTI: 'İnternet kesintisi',
};
// Uyaran isaretine gore olay arama penceresi (s). "once": dugmeye uyarandan biraz sonra
// basilabilir; "sonra": yontemin dogasi geregi gecikme (A03 sefer bitince, A04 dakikalik ortalama).
const WINDOW = {
  A01: {once: 5, sonra: 15},
  A02: {once: 5, sonra: 120},
  A03: {once: 5, sonra: 300},
  A04: {once: 5, sonra: 600},
};

/* ------------------------------------------------------------------ ayarlar */

function arg(name) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
}
const HAS = (name) => process.argv.includes(name);

function influxConfig() {
  let token = process.env.INFLUX_TOKEN;
  if (!token) {
    // Node-RED akisindaki sekme ortam degiskeninden (yerel; depoda yok)
    const flows = JSON.parse(fs.readFileSync(path.join(os.homedir(), '.node-red', 'flows.json'), 'utf8'));
    const tab = flows.find((n) => n.type === 'tab' && (n.env || []).some((e) => e.name === 'INFLUX_TOKEN'));
    token = tab && tab.env.find((e) => e.name === 'INFLUX_TOKEN').value;
    if (!token) throw new Error('InfluxDB token bulunamadi (INFLUX_TOKEN ile verin)');
  }
  return {
    url: process.env.INFLUX_URL || 'http://localhost:8086',
    org: process.env.INFLUX_ORG || 'stm32iot',
    bucket: process.env.INFLUX_BUCKET || 'sensor',
    token,
  };
}

// ESP32'nin guncel IP adresi: ag degisince degisir. ESP32_HOST yoksa yerel broker'daki
// retained "info" mesajindan okunur (mosquitto_sub, esp32 kullanicisi), olmazsa ev agi adresi.
function esp32Host() {
  if (process.env.ESP32_HOST) return process.env.ESP32_HOST;
  try {
    const notes = fs.readFileSync(path.join(ROOT, 'ESP32', 'node-red', 'mqtt_kullanicilar.txt'), 'utf8');
    const pw = notes.match(/^\s*esp32\s*:\s*(\S+)/m)[1];
    const out = require('child_process').execFileSync('C:\\Program Files\\Mosquitto\\mosquitto_sub.exe',
      ['-h', 'localhost', '-u', 'esp32', '-P', pw, '-t', 'stm32iot-fa6dbb/cihaz01/info', '-C', '1', '-W', '5'],
      {encoding: 'utf8', timeout: 8000});
    return JSON.parse(out).ip;
  } catch (e) {
    return '192.168.1.66';
  }
}

function viewKey() {
  if (process.env.RENDER_VIEW_KEY) return process.env.RENDER_VIEW_KEY;
  const notes = fs.readFileSync(path.join(ROOT, 'ESP32', 'node-red', 'mqtt_kullanicilar.txt'), 'utf8');
  const m = notes.match(/VIEW_KEY\s*:\s*(\S+)/);
  if (!m) throw new Error('VIEW_KEY bulunamadi (RENDER_VIEW_KEY ile verin)');
  return m[1];
}

/* ------------------------------------------------------------------ veri */

// Flux sorgusu -> satir nesneleri (annotated CSV degil, duz CSV)
async function flux(cfg, query) {
  const r = await fetch(`${cfg.url}/api/v2/query?org=${encodeURIComponent(cfg.org)}`, {
    method: 'POST',
    headers: {Authorization: `Token ${cfg.token}`, 'Content-Type': 'application/vnd.flux', Accept: 'application/csv'},
    body: query,
  });
  if (!r.ok) throw new Error(`InfluxDB ${r.status}: ${await r.text()}`);
  const rows = [];
  let header = null;
  for (const line of (await r.text()).split(/\r?\n/)) {
    if (!line.trim()) { header = null; continue; }
    const cells = parseCsvLine(line);
    if (!header) { header = cells; continue; }
    rows.push(Object.fromEntries(header.map((h, i) => [h, cells[i]])));
  }
  return rows;
}

function parseCsvLine(line) {
  const out = [];
  let cur = '', q = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (q) {
      if (c === '"' && line[i + 1] === '"') { cur += '"'; i++; } else if (c === '"') q = false; else cur += c;
    } else if (c === '"') q = true;
    else if (c === ',') { out.push(cur); cur = ''; } else cur += c;
  }
  out.push(cur);
  return out;
}

async function loadMarkers(cfg) {
  const rows = await flux(cfg, `from(bucket: "${cfg.bucket}") |> range(start: 0)
    |> filter(fn: (r) => r._measurement == "experiment")
    |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
    |> keep(columns: ["_time", "exp", "type", "phase", "trial", "severity", "cond"])`);
  const exps = new Map();
  for (const r of rows.sort((a, b) => Date.parse(a._time) - Date.parse(b._time))) {
    const no = Number(r.exp);
    if (!exps.has(no)) exps.set(no, {no, type: r.type, severity: '', cond: '', start: null, end: null, stimuli: []});
    const e = exps.get(no);
    const t = Date.parse(r._time) / 1000;
    if (r.phase === 'start') { e.start = t; e.severity = r.severity || ''; e.cond = r.cond || ''; }
    else if (r.phase === 'stimulus') e.stimuli.push({trial: Number(r.trial), t});
    else if (r.phase === 'end') e.end = t;
  }
  return [...exps.values()];
}

async function loadEvents(cfg, from, to) {
  const rows = await flux(cfg, `from(bucket: "${cfg.bucket}")
    |> range(start: ${Math.floor(from)}, stop: ${Math.ceil(to) + 1})
    |> filter(fn: (r) => r._measurement == "event")
    |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
    |> keep(columns: ["_time", "code", "state", "value", "limit"])`);
  return rows.map((r) => ({t: Date.parse(r._time) / 1000, code: r.code, state: r.state, value: Number(r.value)}))
    .sort((a, b) => a.t - b.t);
}

/* ------------------------------------------------------------------ eslestirme */

function analyse(exp, events) {
  const now = Date.now() / 1000;
  const end = exp.end || now;
  const started = events.filter((e) => e.state === 'BASLADI' && ANOMALY.includes(e.code));
  const trials = [];
  const used = new Set();
  const target = exp.type === 'KESINTI' ? 'A01' : exp.type;   // kesinti deneyinde uyaran darbedir

  if (WINDOW[target]) {
    const w = WINDOW[target];
    exp.stimuli.forEach((s, i) => {
      const next = exp.stimuli[i + 1];
      const until = Math.min(s.t + w.sonra, next ? next.t - w.once : Infinity);
      const hit = started.find((e, k) => !used.has(k) && e.code === target && e.t >= s.t - w.once && e.t <= until);
      if (hit) used.add(started.indexOf(hit));
      trials.push({
        trial: s.trial, expected: s.t, decided: hit ? hit.t : null,
        detected: !!hit, delay: hit ? Math.round(hit.t - s.t) : null, value: hit ? hit.value : null,
      });
    });
  }
  // Deney suresince eslesmeyen her anomali baslangici yanlis alarmdir
  const lastWindow = WINDOW[target] ? WINDOW[target].sonra : 0;
  const falseAlarms = started.filter((e, k) => !used.has(k) && e.t >= exp.start && e.t <= end + (exp.end ? 0 : lastWindow));
  const hours = (end - exp.start) / 3600;
  return {exp, target, trials, falseAlarms, hours};
}

function metrics(tp, fn, fp) {
  const precision = tp + fp ? tp / (tp + fp) : null;
  const recall = tp + fn ? tp / (tp + fn) : null;
  const f1 = precision !== null && recall !== null && precision + recall ? 2 * precision * recall / (precision + recall) : null;
  return {precision, recall, f1};
}

/* ------------------------------------------------------------------ cikti */

const pct = (x) => (x === null ? '-' : (x * 100).toFixed(1).replace('.', ',') + ' %');
const num = (x, d = 1) => (x === null || x === undefined || Number.isNaN(x) ? '-' : x.toFixed(d).replace('.', ','));
const iso = (t) => new Date(t * 1000).toLocaleString('sv-SE', {timeZone: 'Europe/Istanbul'});
const median = (a) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

function writeOutputs(results) {
  fs.mkdirSync(OUT_DIR, {recursive: true});
  const bom = '﻿';

  const trialRows = ['deney_no;tarih;tur;siddet;kosul;deneme;beklenen_zaman;karar_zamani;dogru_tespit;gecikme_s;olcum_degeri'];
  const faRows = ['deney_no;tur;zaman;kod;deger'];
  const byType = {};
  for (const r of results) {
    const {exp} = r;
    for (const t of r.trials) {
      trialRows.push([exp.no, iso(exp.start).slice(0, 10), exp.type, exp.severity, exp.cond, t.trial, iso(t.expected),
        t.decided ? iso(t.decided) : '', t.detected ? 'E' : 'H', t.delay ?? '', t.value === null ? '' : num(t.value, 3)].join(';'));
    }
    for (const e of r.falseAlarms) faRows.push([exp.no, exp.type, iso(e.t), e.code, num(e.value, 3)].join(';'));

    const key = exp.type;
    byType[key] ||= {tp: 0, fn: 0, fp: 0, delays: [], hours: 0, exps: 0};
    const b = byType[key];
    b.tp += r.trials.filter((t) => t.detected).length;
    b.fn += r.trials.filter((t) => !t.detected).length;
    b.fp += r.falseAlarms.length;
    b.delays.push(...r.trials.filter((t) => t.detected).map((t) => t.delay));
    b.hours += r.hours;
    b.exps++;
  }
  fs.writeFileSync(path.join(OUT_DIR, 'denemeler.csv'), bom + trialRows.join('\r\n') + '\r\n');
  fs.writeFileSync(path.join(OUT_DIR, 'yanlis_alarmlar.csv'), bom + faRows.join('\r\n') + '\r\n');

  const md = [`# Deney sonuçları`, '', `Oluşturma: ${iso(Date.now() / 1000)} · Deney sayısı: ${results.length}`, '',
    '| Tür | Deney | Deneme | TP | FN | FP | Precision | Recall | F1 | Gecikme ort. / medyan / en çok (s) | Yanlış alarm / saat |',
    '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |'];
  for (const type of Object.keys(NAMES)) {
    const b = byType[type];
    if (!b) continue;
    const m = metrics(b.tp, b.fn, b.fp);
    const d = b.delays;
    const avg = d.length ? d.reduce((x, y) => x + y, 0) / d.length : null;
    md.push(`| ${type} ${NAMES[type]} | ${b.exps} | ${b.tp + b.fn} | ${b.tp} | ${b.fn} | ${b.fp} | ${pct(type === 'NORMAL' ? null : m.precision)} | ${pct(type === 'NORMAL' ? null : m.recall)} | ${pct(type === 'NORMAL' ? null : m.f1)} | ${num(avg)} / ${num(median(d))} / ${d.length ? Math.max(...d) : '-'} | ${num(b.hours ? b.fp / b.hours : null, 2)} |`);
  }
  md.push('', 'TP: doğru tespit · FN: kaçırılan · FP: yanlış alarm (deney süresince eşleşmeyen anomali başlangıcı).',
    'Precision = TP / (TP + FP) · Recall = TP / (TP + FN) · F1 = 2·P·R / (P + R).',
    'Gecikme: uyaran işaretinden ESP32 kararına (s, ESP32 olay zamanı 1 s çözünürlüklü; dakikalık ortalama kullanan A04 ve sefer sonunda karar veren A03 için doğal gecikme vardır).',
    `Arama pencereleri (s): ${Object.entries(WINDOW).map(([k, w]) => `${k} −${w.once}/+${w.sonra}`).join(', ')}.`);
  fs.writeFileSync(path.join(OUT_DIR, 'ozet.md'), md.join('\n') + '\n');
  return md.join('\n');
}

/* ------------------------------------------------------------------ kesinti raporu */

async function outageReport(cfg, exp) {
  const host = esp32Host();
  const api = process.env.API_URL || 'https://stm32iot-api-fh72.onrender.com';
  const device = process.env.API_DEVICE || 'cihaz01';
  const from = Math.floor(exp.start), to = Math.ceil(exp.end || Date.now() / 1000);

  // 1) ESP32 yerel kaydi (log.csv: "YYYY-MM-DD HH:MM:SS" yerel saat, Turkiye UTC+3)
  const csv = await (await fetch(`http://${host}/log.csv`)).text();
  const local = csv.split(/\r?\n/).slice(1).map((l) => l.split(';')[0]).filter(Boolean)
    .map((z) => Date.parse(z.replace(' ', 'T') + '+03:00') / 1000).filter((t) => t >= from && t <= to);

  // 2) Bulut kayitlari (received_at ile)
  const headers = {'X-API-Key': viewKey()};
  const cloud = await (await fetch(`${api}/api/v1/telemetry?device=${device}&from=${from}&to=${to}&limit=10000`, {headers})).json();
  const cloudTs = cloud.map((r) => r.ts);
  const uniq = new Set(cloudTs);
  const missing = local.filter((t) => !uniq.has(t));
  const delays = cloud.map((r) => r.received_at - r.ts).filter((d) => Number.isFinite(d));
  const lastArrival = cloud.length ? Math.max(...cloud.map((r) => r.received_at)) : null;

  // 3) Olaylar: ESP32 yerel olay kaydi ve buluttaki olaylar
  const localEv = (await (await fetch(`http://${host}/events`)).json())
    .filter((e) => e[0] >= from && e[0] <= to + 600 && ANOMALY.includes(e[1]) && e[2] === 'BASLADI');
  const cloudEv = (await (await fetch(`${api}/api/v1/events?device=${device}&from=${from}&to=${to + 600}&limit=10000`, {headers})).json())
    .filter((e) => ANOMALY.includes(e.code) && e.state === 'BASLADI');

  // 4) Canli MQTT akisi (InfluxDB): QoS0 telemetri kesintide birikmez, bosluk beklenir
  const live = await flux(cfg, `from(bucket: "${cfg.bucket}") |> range(start: ${from}, stop: ${to})
    |> filter(fn: (r) => r._measurement == "telemetry" and r._field == "v") |> count()`);
  const liveCount = live.length ? Number(live[0]._value) : 0;

  const r = analyse(exp, await loadEvents(cfg, from - 60, to + 700));
  const dur = to - from;
  const md = [`# Kesinti deneyi ${exp.no}`, '',
    `Kesinti: ${iso(from)} → ${iso(to)} (${(dur / 60).toFixed(1).replace('.', ',')} dk) · Koşul: ${exp.cond || '-'}`, '',
    '| Ölçüt | Değer |', '| --- | --- |',
    `| Uyaran (kontrollü darbe) sayısı | ${exp.stimuli.length} |`,
    `| Kesintide tespit edilen darbe (A01) | ${r.trials.filter((t) => t.detected).length} / ${r.trials.length} |`,
    `| ESP32 yerel kayıt sayısı (pencerede) | ${local.length} |`,
    `| Buluta ulaşan kayıt | ${uniq.size} (${local.length ? pct(local.filter((t) => uniq.has(t)).length / local.length) : '-'}) |`,
    `| Eksik kayıt | ${missing.length} |`,
    `| Tekrar (çift) kayıt | ${cloudTs.length - uniq.size} |`,
    `| Anomali olayı: yerel / bulut | ${localEv.length} / ${cloudEv.length} |`,
    `| Aktarım gecikmesi (received_at − ölçüm), medyan / en çok | ${num(median(delays), 0)} s / ${delays.length ? Math.max(...delays) : '-'} s |`,
    `| Bağlantı gelince son kaydın buluta ulaşması | ${lastArrival && exp.end ? `${Math.round(lastArrival - exp.end)} s` : '-'} |`,
    `| Canlı MQTT telemetrisi (InfluxDB) | ${liveCount} / ~${dur} s (QoS0 akış kesintide birikmez) |`,
  ];
  if (missing.length) md.push('', `Eksik kayıt zamanları: ${missing.slice(0, 20).map(iso).join(', ')}${missing.length > 20 ? ' …' : ''}`);
  fs.mkdirSync(OUT_DIR, {recursive: true});
  fs.writeFileSync(path.join(OUT_DIR, `kesinti_${exp.no}.md`), md.join('\n') + '\n');
  return md.join('\n');
}

/* ------------------------------------------------------------------ ana */

async function main() {
  const cfg = influxConfig();
  let exps = await loadMarkers(cfg);
  if (!HAS('--test')) exps = exps.filter((e) => e.no < 900);
  const only = arg('--deney') || arg('--kesinti');
  if (only) exps = exps.filter((e) => e.no === Number(only));
  exps = exps.filter((e) => e.start);
  if (!exps.length) {
    console.log('Analiz edilecek deney yok (Node-RED "Deney Kaydı" panelinden deney başlatın; 900 ve üzeri test sayılır, --test ile dahil edilir).');
    return;
  }

  if (arg('--kesinti')) {
    console.log(await outageReport(cfg, exps[0]));
    console.log(`\nKaydedildi: ${path.relative(ROOT, path.join(OUT_DIR, `kesinti_${exps[0].no}.md`))}`);
    return;
  }

  const from = Math.min(...exps.map((e) => e.start)) - 60;
  const to = Math.max(...exps.map((e) => e.end || Date.now() / 1000)) + 700;
  const events = await loadEvents(cfg, from, to);
  const results = exps.map((e) => analyse(e, events));
  for (const r of results) {
    const tp = r.trials.filter((t) => t.detected).length;
    console.log(`Deney ${r.exp.no} (${r.exp.type}${r.exp.end ? '' : ', SÜRÜYOR'}): ${r.trials.length} uyaran, ${tp} tespit, ${r.trials.length - tp} kaçırma, ${r.falseAlarms.length} yanlış alarm`
      + (r.falseAlarms.length ? ` [${r.falseAlarms.map((e) => e.code).join(' ')}]` : ''));
  }
  console.log('\n' + writeOutputs(results));
  console.log(`\nKaydedildi: ${path.relative(ROOT, OUT_DIR)}${path.sep}{denemeler.csv, yanlis_alarmlar.csv, ozet.md}`);
}

if (require.main === module) {
  main().catch((e) => { console.error('HATA:', e.message); process.exit(1); });
}
module.exports = {analyse, metrics, WINDOW};
