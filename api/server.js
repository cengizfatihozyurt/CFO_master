/*
 * STM32 IoT veri API'si
 *
 * ESP32 olcumleri ve olaylari toplu (batch) olarak POST eder; sunucu Postgres'e
 * yazar. Ayni kayit tekrar gelirse (ESP32 baglanti kopunca yeniden gonderir)
 * veritabani benzersiz anahtarla tekrar kaydi engeller.
 *
 * Ortam degiskenleri:
 *   API_KEY       ESP32'nin X-API-Key basliginda gonderecegi anahtar (zorunlu)
 *   VIEW_KEY      Istege bagli, sadece okuma anahtari (panel sifresi). Tanimliysa
 *                 okuma uclari API_KEY'e ek olarak bunu da kabul eder; yazma kabul etmez.
 *   DATABASE_URL  Postgres baglanti adresi. Yoksa yerel test icin SQLite dosyasi kullanilir.
 *   PORT          Dinlenecek port (Render kendisi verir), varsayilan 8080
 *
 * Uclar:
 *   GET  /                            goruntuleme paneli (public/index.html)
 *   GET  /health                      saglik kontrolu (anahtarsiz)
 *   GET  /api/v1/series               ?device=&from=&to=&points=  grafik icin gruplanmis veri
 *   POST /api/v1/telemetry            {"device","records":[{"t","temp","vx","vy","vz","v","flags"}]}
 *   POST /api/v1/events               {"device","events":[{"t","code","state","value","limit","desc"}]}
 *   GET  /api/v1/telemetry            ?device=&from=&to=&limit=   (epoch saniye)
 *   GET  /api/v1/events               ?device=&from=&to=&limit=
 *   GET  /api/v1/latest               her cihazin son kaydi ve toplam sayilar
 */
'use strict';

const crypto = require('crypto');
const path = require('path');
const express = require('express');

const API_KEY = process.env.API_KEY || '';
const VIEW_KEY = process.env.VIEW_KEY || '';
const PORT = Number(process.env.PORT) || 8080;
const MAX_BATCH = 500;

if (!API_KEY) {
  console.error('HATA: API_KEY ortam degiskeni tanimli degil');
  process.exit(1);
}

/* ------------------------------------------------------------------ depolama */

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS telemetry (
     device TEXT NOT NULL,
     ts BIGINT NOT NULL,
     temp REAL,
     vx REAL, vy REAL, vz REAL, v REAL,
     flags INTEGER NOT NULL DEFAULT 0,
     received_at BIGINT NOT NULL,
     PRIMARY KEY (device, ts))`,
  `CREATE TABLE IF NOT EXISTS events (
     device TEXT NOT NULL,
     ts BIGINT NOT NULL,
     code TEXT NOT NULL,
     state TEXT NOT NULL,
     value REAL,
     lim REAL,
     descr TEXT,
     received_at BIGINT NOT NULL,
     PRIMARY KEY (device, ts, code, state))`,
];

// Postgres ve SQLite icin ortak arayuz: query(sql, params) -> rows
// SQL'ler $1, $2 ... parametre bicimiyle yazilir.
async function openStore() {
  if (process.env.DATABASE_URL) {
    const { Pool } = require('pg');
    const local = /localhost|127\.0\.0\.1/.test(process.env.DATABASE_URL);
    const pool = new Pool({
      connectionString: process.env.DATABASE_URL,
      ssl: local ? false : { rejectUnauthorized: false },
      max: 5,
    });
    for (const s of SCHEMA) await pool.query(s);
    console.log('Depolama: Postgres');
    return {
      kind: 'postgres',
      query: async (sql, params = []) => (await pool.query(sql, params)).rows,
    };
  }

  const { DatabaseSync } = require('node:sqlite');
  const file = process.env.SQLITE_FILE || 'local.db';
  const db = new DatabaseSync(file);
  for (const s of SCHEMA) db.exec(s);
  console.log(`Depolama: SQLite (${file}) - sadece yerel test icin`);
  return {
    kind: 'sqlite',
    query: async (sql, params = []) => {
      // $1 -> ?1 (numarali parametre; ayni parametre birden cok kez kullanilabilir)
      const st = db.prepare(sql.replace(/\$(\d+)/g, '?$1').replace(/\s+RETURNING\s+\w+\s*$/i, ''));
      if (/^\s*select/i.test(sql)) return st.all(...params);
      // Postgres'teki RETURNING ile ayni sonuc: eklenen satir sayisi kadar eleman
      return new Array(Number(st.run(...params).changes)).fill({});
    },
  };
}

/* ------------------------------------------------------------------ yardimcilar */

function keyMatches(req, key) {
  if (!key) return false;
  const got = Buffer.from(String(req.get('x-api-key') || ''));
  const want = Buffer.from(key);
  return got.length === want.length && crypto.timingSafeEqual(got, want);
}

// Yazma: sadece API_KEY
function requireKey(req, res, next) {
  if (!keyMatches(req, API_KEY)) return res.status(401).json({ ok: false, error: 'gecersiz API anahtari' });
  next();
}

// Okuma: API_KEY ya da VIEW_KEY
function requireRead(req, res, next) {
  if (!keyMatches(req, API_KEY) && !keyMatches(req, VIEW_KEY)) {
    return res.status(401).json({ ok: false, error: 'gecersiz anahtar' });
  }
  next();
}

const numOrNull = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const validDevice = (d) => typeof d === 'string' && /^[A-Za-z0-9_.-]{1,64}$/.test(d);
const validTs = (t) => Number.isInteger(t) && t > 1600000000 && t < 4102444800;

function rangeArgs(q) {
  const from = Number.isFinite(Number(q.from)) ? Number(q.from) : 0;
  const to = Number.isFinite(Number(q.to)) && q.to !== undefined ? Number(q.to) : 4102444800;
  const limit = Math.min(Math.max(parseInt(q.limit, 10) || 1000, 1), 10000);
  return { from, to, limit };
}

/* ------------------------------------------------------------------ uygulama */

async function main() {
  const store = await openStore();
  const app = express();
  app.disable('x-powered-by');
  app.use(express.json({ limit: '1mb' }));
  app.use(express.static(path.join(__dirname, 'public')));

  app.get('/health', async (req, res) => {
    try {
      await store.query('SELECT 1 AS ok');
      res.json({ ok: true, db: store.kind, time: Math.floor(Date.now() / 1000) });
    } catch (e) {
      res.status(503).json({ ok: false, error: 'veritabani erisilemiyor' });
    }
  });

  app.post('/api/v1/telemetry', requireKey, async (req, res) => {
    const { device, records } = req.body || {};
    if (!validDevice(device) || !Array.isArray(records)) {
      return res.status(400).json({ ok: false, error: 'device ve records zorunlu' });
    }
    if (records.length > MAX_BATCH) {
      return res.status(413).json({ ok: false, error: `en fazla ${MAX_BATCH} kayit` });
    }
    const now = Math.floor(Date.now() / 1000);
    let inserted = 0, skipped = 0;
    for (const r of records) {
      if (!r || !validTs(r.t)) { skipped++; continue; }
      const rows = await store.query(
        `INSERT INTO telemetry (device, ts, temp, vx, vy, vz, v, flags, received_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
         ON CONFLICT DO NOTHING RETURNING ts`,
        [device, r.t, numOrNull(r.temp), numOrNull(r.vx), numOrNull(r.vy), numOrNull(r.vz),
         numOrNull(r.v), Number.isInteger(r.flags) ? r.flags : 0, now]);
      inserted += rows.length;
    }
    res.json({ ok: true, received: records.length, inserted, skipped });
  });

  app.post('/api/v1/events', requireKey, async (req, res) => {
    const { device, events } = req.body || {};
    if (!validDevice(device) || !Array.isArray(events)) {
      return res.status(400).json({ ok: false, error: 'device ve events zorunlu' });
    }
    if (events.length > MAX_BATCH) {
      return res.status(413).json({ ok: false, error: `en fazla ${MAX_BATCH} olay` });
    }
    const now = Math.floor(Date.now() / 1000);
    let skipped = 0;
    for (const e of events) {
      if (!e || !validTs(e.t) || typeof e.code !== 'string' || typeof e.state !== 'string') {
        skipped++;
        continue;
      }
      await store.query(
        `INSERT INTO events (device, ts, code, state, value, lim, descr, received_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
         ON CONFLICT DO NOTHING`,
        [device, e.t, e.code.slice(0, 8), e.state.slice(0, 16), numOrNull(e.value), numOrNull(e.limit),
         String(e.desc || '').slice(0, 300), now]);
    }
    res.json({ ok: true, received: events.length, skipped });
  });

  app.get('/api/v1/telemetry', requireRead, async (req, res) => {
    const { from, to, limit } = rangeArgs(req.query);
    const device = req.query.device;
    // received_at: kaydin sunucuya ulastigi an (kesinti deneyinde aktarim gecikmesi icin)
    const rows = validDevice(device)
      ? await store.query(
        `SELECT device, ts, temp, vx, vy, vz, v, flags, received_at FROM telemetry
         WHERE device = $1 AND ts >= $2 AND ts <= $3 ORDER BY ts DESC LIMIT $4`, [device, from, to, limit])
      : await store.query(
        `SELECT device, ts, temp, vx, vy, vz, v, flags, received_at FROM telemetry
         WHERE ts >= $1 AND ts <= $2 ORDER BY ts DESC LIMIT $3`, [from, to, limit]);
    res.json(rows.map((r) => ({ ...r, ts: Number(r.ts), received_at: Number(r.received_at) })));
  });

  app.get('/api/v1/events', requireRead, async (req, res) => {
    const { from, to, limit } = rangeArgs(req.query);
    const device = req.query.device;
    const sel = 'SELECT device, ts, code, state, value, lim AS "limit", descr AS "desc", received_at FROM events';
    const rows = validDevice(device)
      ? await store.query(`${sel} WHERE device = $1 AND ts >= $2 AND ts <= $3 ORDER BY ts DESC LIMIT $4`,
        [device, from, to, limit])
      : await store.query(`${sel} WHERE ts >= $1 AND ts <= $2 ORDER BY ts DESC LIMIT $3`, [from, to, limit]);
    res.json(rows.map((r) => ({ ...r, ts: Number(r.ts), received_at: Number(r.received_at) })));
  });

  // Grafik verisi: aralik en fazla `points` gruba bolunur. Sicaklik ve eksenler
  // ortalama, toplam titresim en yuksek, alarm bitleri birlesik (OR).
  app.get('/api/v1/series', requireRead, async (req, res) => {
    const device = req.query.device;
    if (!validDevice(device)) return res.status(400).json({ ok: false, error: 'device zorunlu' });
    const now = Math.floor(Date.now() / 1000);
    const to = Number.isFinite(Number(req.query.to)) && req.query.to !== undefined ? Number(req.query.to) : now;
    let from = Number.isFinite(Number(req.query.from)) ? Number(req.query.from) : to - 3600;
    if (from <= 0) {
      const first = await store.query('SELECT MIN(ts) AS t FROM telemetry WHERE device = $1', [device]);
      from = Number(first[0] && first[0].t) || to - 3600;
    }
    const points = Math.min(Math.max(parseInt(req.query.points, 10) || 600, 10), 2000);
    const bucket = Math.max(1, Math.ceil((to - from) / points));
    const flagsAgg = store.kind === 'postgres' ? 'BIT_OR(flags)' : 'MAX(flags)';
    const rows = await store.query(
      `SELECT ts - (ts % $1) AS t, AVG(temp) AS temp, AVG(vx) AS vx, AVG(vy) AS vy, AVG(vz) AS vz,
              MAX(v) AS v, ${flagsAgg} AS flags, COUNT(*) AS n
       FROM telemetry WHERE device = $2 AND ts >= $3 AND ts <= $4
       GROUP BY 1 ORDER BY 1`, [bucket, device, from, to]);
    const num = (x) => (x === null || x === undefined ? null : Number(x));
    res.json({
      from, to, bucket,
      rows: rows.map((r) => [Number(r.t), num(r.temp), num(r.vx), num(r.vy), num(r.vz), num(r.v), Number(r.flags) || 0]),
    });
  });

  app.get('/api/v1/latest', requireRead, async (req, res) => {
    const devices = await store.query(
      `SELECT device, COUNT(*) AS records, MIN(ts) AS first_ts, MAX(ts) AS last_ts
       FROM telemetry GROUP BY device ORDER BY device`);
    const out = [];
    for (const d of devices) {
      const last = await store.query(
        'SELECT ts, temp, vx, vy, vz, v, flags FROM telemetry WHERE device = $1 ORDER BY ts DESC LIMIT 1',
        [d.device]);
      const ev = await store.query('SELECT COUNT(*) AS n FROM events WHERE device = $1', [d.device]);
      out.push({
        device: d.device, records: Number(d.records), events: Number(ev[0].n),
        first_ts: Number(d.first_ts), last_ts: Number(d.last_ts),
        last: last[0] ? { ...last[0], ts: Number(last[0].ts) } : null,
      });
    }
    res.json(out);
  });

  app.use((err, req, res, next) => {
    if (err.type === 'entity.parse.failed') return res.status(400).json({ ok: false, error: 'gecersiz JSON' });
    console.error(err);
    res.status(500).json({ ok: false, error: 'sunucu hatasi' });
  });

  app.listen(PORT, () => console.log(`API dinliyor: ${PORT}`));
}

main().catch((e) => {
  console.error('Baslatma hatasi:', e);
  process.exit(1);
});
