#!/usr/bin/env node
/*
 * Uzun sureli normal calisma raporu (isaretsiz zaman araligi).
 *
 * Kullanim (proje klasorunde):
 *   node deney/surekli.js --bas "2026-11-18 00:20" --bit "2026-11-18 22:00" --ad gece_2026-11-18 [--kosul "masa, sabit"]
 *
 * Kaynaklar:
 *   Bulut API  /api/v1/telemetry  ESP32'nin yerel 10 s kayitlari (depola-ilet; received_at ile)
 *              /api/v1/events     ESP32 olaylari
 *   InfluxDB   telemetry (canli MQTT yolu, 1 s), event
 *
 * Hesaplananlar: kayit butunlugu (beklenen/gelen, bosluklar), canli MQTT yolunun kesintileri,
 * kesinti sirasinda kaydedilip sonradan ulasan kayitlar, anomali olaylari ve yanlis alarm/saat,
 * titresim ve sicaklik istatistikleri. Cikti: deney/sonuclar/surekli_<ad>.md
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const OUT_DIR = path.join(__dirname, 'sonuclar');
const API = process.env.API_URL || 'https://stm32iot-api-fh72.onrender.com';
const DEVICE = process.env.API_DEVICE || 'cihaz01';
const ANOMALY = ['A01', 'A02', 'A03', 'A04'];

const arg = (name) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : null; };
// "YYYY-MM-DD HH:MM[:SS]" Turkiye saati (UTC+3) -> epoch s
const parseLocal = (s) => Date.parse(s.replace(' ', 'T') + (s.length <= 16 ? ':00' : '') + '+03:00') / 1000;
const loc = (t) => new Date((t + 3 * 3600) * 1000).toISOString().slice(0, 19).replace('T', ' ');
const num = (x, d = 3) => Number(x).toFixed(d).replace('.', ',');
const dur = (s) => (s >= 3600 ? `${Math.floor(s / 3600)} sa ${Math.round((s % 3600) / 60)} dk` : `${Math.round(s / 60)} dk`);

function secrets() {
  const notes = fs.readFileSync(path.join(ROOT, 'ESP32', 'node-red', 'mqtt_kullanicilar.txt'), 'utf8');
  const flows = JSON.parse(fs.readFileSync(path.join(os.homedir(), '.node-red', 'flows.json'), 'utf8'));
  const tab = flows.find((n) => n.type === 'tab' && (n.env || []).some((e) => e.name === 'INFLUX_TOKEN'));
  return {
    viewKey: process.env.RENDER_VIEW_KEY || notes.match(/VIEW_KEY\s*:\s*(\S+)/)[1],
    influx: process.env.INFLUX_TOKEN || tab.env.find((e) => e.name === 'INFLUX_TOKEN').value,
  };
}

async function api(key, p) {
  const r = await fetch(`${API}${p}`, {headers: {'X-API-Key': key}, signal: AbortSignal.timeout(120000)});
  if (!r.ok) throw new Error(`API ${r.status}`);
  const j = await r.json();
  return Array.isArray(j) ? j : (j.rows || j.data || []);
}

async function flux(token, query) {
  const r = await fetch('http://localhost:8086/api/v2/query?org=stm32iot', {method: 'POST',
    headers: {Authorization: `Token ${token}`, 'Content-Type': 'application/vnd.flux', Accept: 'application/csv'}, body: query});
  if (!r.ok) throw new Error(`InfluxDB ${r.status}`);
  const rows = []; let h = null;
  for (const l of (await r.text()).split(/\r?\n/)) {
    if (!l.trim()) { h = null; continue; }
    const c = l.split(',');
    if (!h) { h = c; continue; }
    rows.push(Object.fromEntries(h.map((k, i) => [k, c[i]])));
  }
  return rows;
}

async function main() {
  const bas = arg('--bas'), bit = arg('--bit');
  if (!bas || !bit) { console.log('Kullanim: node deney/surekli.js --bas "YYYY-MM-DD HH:MM" --bit "YYYY-MM-DD HH:MM" --ad <ad>'); return; }
  const from = parseLocal(bas), to = parseLocal(bit);
  const name = arg('--ad') || bas.slice(0, 10);
  const kosul = arg('--kosul') || 'belirtilmedi';
  const logInt = Number(arg('--aralik') || 10);
  const gapMin = Number(arg('--bosluk') || 60);
  const hours = (to - from) / 3600;
  const s = secrets();

  // 1) Cihazin yerel kayitlari (bulut uzerinden)
  const rec = (await api(s.viewKey, `/api/v1/telemetry?device=${DEVICE}&from=${from}&to=${to}&limit=200000`))
    .map((r) => ({...r, t: Number(r.ts), rx: r.received_at == null ? null
      : (Number.isFinite(Number(r.received_at)) ? Number(r.received_at) : Date.parse(r.received_at) / 1000)}))
    .filter((r) => r.t >= from && r.t <= to).sort((a, b) => a.t - b.t);
  const recGaps = [];
  let prev = from;
  for (const r of rec) { if (r.t - prev > 2 * logInt + 1) recGaps.push([prev, r.t]); prev = r.t; }
  if (to - prev > 2 * logInt + 1) recGaps.push([prev, to]);
  const expected = Math.floor((to - from) / logInt);

  // 2) Canli MQTT yolu (InfluxDB, 1 s)
  const iso = (t) => new Date(t * 1000).toISOString();
  const live = (await flux(s.influx, `from(bucket:"sensor")|>range(start:${iso(from)},stop:${iso(to)})
    |>filter(fn:(r)=>r._measurement=="telemetry" and r._field=="v")|>keep(columns:["_time"])`))
    .map((r) => Date.parse(r._time) / 1000).sort((a, b) => a - b);
  const liveGaps = [];
  prev = from;
  for (const t of live) { if (t - prev > gapMin) liveGaps.push([prev, t]); prev = t; }
  if (to - prev > gapMin) liveGaps.push([prev, to]);
  const liveDown = liveGaps.reduce((a, [x, y]) => a + (y - x), 0);

  // Kesinti sirasinda kaydedilip sonradan ulasan kayitlar
  const inOutage = (t) => liveGaps.some(([x, y]) => t > x && t < y);
  const outRecs = rec.filter((r) => inOutage(r.t));
  const lateMax = outRecs.reduce((m, r) => (r.rx ? Math.max(m, r.rx - r.t) : m), 0);

  // 3) Olaylar (bulut + canli yol birlesik)
  const ev = new Map();
  for (const e of await api(s.viewKey, `/api/v1/events?device=${DEVICE}&from=${from}&to=${to}&limit=100000`)) {
    if (e.ts >= from && e.ts <= to) ev.set(`${e.ts}|${e.code}|${e.state}`, {t: Number(e.ts), code: e.code, state: e.state, value: e.value});
  }
  const iev = await flux(s.influx, `from(bucket:"sensor")|>range(start:${iso(from)},stop:${iso(to)})
    |>filter(fn:(r)=>r._measurement=="event")|>pivot(rowKey:["_time"],columnKey:["_field"],valueColumn:"_value")
    |>keep(columns:["_time","code","state","value"])`);
  for (const e of iev) {
    const t = Math.round(Date.parse(e._time) / 1000);
    if (!ev.has(`${t}|${e.code}|${e.state}`)) ev.set(`${t}|${e.code}|${e.state}`, {t, code: e.code, state: e.state, value: Number(e.value)});
  }
  const events = [...ev.values()].sort((a, b) => a.t - b.t);
  // 10 s kayitlarin alarm bitlerinden olay baslangiclari (olay listesi bir yolda eksik
  // kalsa bile cihazin kaydi bunlari tasir; or. FW 1.8'de olaylar buluta gitmiyordu)
  const BITS = {E01: 0x01, E02: 0x02, E03: 0x04, E04: 0x08, A01: 0x10, A02: 0x20, A03: 0x40, A04: 0x80};
  let prevFlags = 0, fromFlags = 0;
  for (const r of rec) {
    const f = Number(r.flags) || 0;
    for (const [code, bit] of Object.entries(BITS)) {
      if ((f & bit) && !(prevFlags & bit)
          && !events.some((e) => e.code === code && e.state === 'BASLADI' && Math.abs(e.t - r.t) <= 15)) {
        events.push({t: r.t, code, state: 'BASLADI', value: Number(r.v), kayit: true});
        fromFlags++;
      }
    }
    prevFlags = f;
  }
  events.sort((a, b) => a.t - b.t);
  const fa = events.filter((e) => e.state === 'BASLADI' && ANOMALY.includes(e.code));
  const alarms = events.filter((e) => e.state === 'BASLADI' && !ANOMALY.includes(e.code));

  // 4) Istatistikler (10 s kayitlar: v = pencere icindeki en yuksek 1 s RMS)
  const vs = rec.map((r) => Number(r.v)).filter(Number.isFinite).sort((a, b) => a - b);
  const pct = (p) => vs.length ? vs[Math.min(vs.length - 1, Math.floor(p * vs.length))] : NaN;
  const temps = rec.filter((r) => r.temp != null).map((r) => ({t: r.t, c: Number(r.temp)}));
  let maxRate = 0;   // en hizli sicaklik artisi, 5 dk'lik farkla (C/dk)
  for (let i = 0, j = 0; i < temps.length; i++) {
    while (temps[i].t - temps[j].t > 300) j++;
    if (temps[i].t - temps[j].t >= 240) maxRate = Math.max(maxRate, (temps[i].c - temps[j].c) / ((temps[i].t - temps[j].t) / 60));
  }
  const flagged = rec.filter((r) => Number(r.flags)).length;

  const L = [];
  L.push(`# Uzun süreli normal çalışma: ${name}`, '');
  L.push(`Aralık: ${loc(from)} – ${loc(to)} (${dur(to - from)}) · Koşul: ${kosul} · Oluşturma: ${loc(Date.now() / 1000)}`, '');
  L.push('## Yanlış alarm', '');
  L.push('| Ölçüt | Değer |', '| --- | --- |');
  L.push(`| Anomali başlangıcı (A01–A04) | ${fa.length} |`);
  L.push(`| Yanlış alarm / saat | ${num(fa.length / hours, 2)} |`);
  L.push(`| Eşik alarmı başlangıcı (E01–E04) | ${alarms.length} |`);
  L.push(`| Alarm bayraklı 10 s kayıt | ${flagged} |`, '');
  if (fa.length || alarms.length) {
    L.push("| Zaman | Kod | Değer (g) | Kaynak |", "| --- | --- | --- | --- |");
    for (const e of [...fa, ...alarms].sort((a, b) => a.t - b.t)) L.push(`| ${loc(e.t)} | ${e.code} | ${num(e.value)} | ${e.kayit ? "10 s kaydın alarm bitleri" : "olay listesi"} |`);
    L.push('');
  }
  L.push('## Kayıt bütünlüğü (cihazın yerel kaydı, buluta ulaşan)', '');
  L.push('| Ölçüt | Değer |', '| --- | --- |');
  L.push(`| Beklenen kayıt (${logInt} s aralık) | ${expected} |`);
  L.push(`| Buluta ulaşan kayıt | ${rec.length} (${num(100 * rec.length / expected, 1)} %) |`);
  L.push(`| ${2 * logInt} s üstü boşluk | ${recGaps.length} |`);
  for (const [x, y] of recGaps) L.push(`| · ${loc(x)} – ${loc(y)} | ${dur(y - x)} |`);
  L.push('');
  L.push('## Canlı MQTT yolu (InfluxDB, 1 s)', '');
  L.push('| Ölçüt | Değer |', '| --- | --- |');
  L.push(`| Gelen ölçüm / beklenen | ${live.length} / ${Math.round(to - from)} |`);
  L.push(`| ${gapMin} s üstü kesinti | ${liveGaps.length} (toplam ${dur(liveDown)}) |`);
  for (const [x, y] of liveGaps) L.push(`| · ${loc(x)} – ${loc(y)} | ${dur(y - x)} |`);
  L.push(`| Kesinti sırasında yerelde kaydedilip sonradan buluta ulaşan kayıt | ${outRecs.length} |`);
  L.push(`| Bu kayıtların en geç ulaşma gecikmesi | ${lateMax ? dur(lateMax) : '-'} |`, '');
  L.push('## Ölçüm istatistikleri (10 s kayıtlar)', '');
  L.push('| Ölçüt | Değer |', '| --- | --- |');
  L.push(`| Titreşim (pencere en yükseği, g): medyan / %99 / en yüksek | ${num(pct(0.5))} / ${num(pct(0.99))} / ${num(vs[vs.length - 1])} |`);
  if (temps.length) {
    const cs = temps.map((x) => x.c);
    L.push(`| Sıcaklık (°C): en düşük / en yüksek | ${num(Math.min(...cs), 2)} / ${num(Math.max(...cs), 2)} |`);
    L.push(`| En hızlı sıcaklık artışı (°C/dk, ≥4 dk fark) | ${num(maxRate, 3)} (A04 eşiği 0,5) |`);
  }
  L.push('', 'Yanlış alarm: bu aralıkta uyaran uygulanmadığı için her anomali başlangıcı yanlış alarm sayılır.',
    'Kayıt bütünlüğü cihazın yerel kaydına dayanır; canlı yol kesildiğinde kayıtlar cihazda bekletilip bağlantı gelince gönderilir (depola-ilet).',
    'Olaylar, cihazın olay listesi ve 10 s kayıtlardaki alarm bitleri birleştirilerek sayılır (olay listesinin bir yolda eksik kalması sonucu etkilemez).');

  fs.mkdirSync(OUT_DIR, {recursive: true});
  const out = path.join(OUT_DIR, `surekli_${name}.md`);
  fs.writeFileSync(out, L.join('\n') + '\n');
  console.log(L.join('\n'));
  console.log(`\nKaydedildi: ${path.relative(ROOT, out)}`);
}

main().catch((e) => { console.error('Hata:', e.message); process.exit(1); });
