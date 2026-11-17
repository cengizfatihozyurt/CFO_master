#!/usr/bin/env node
/*
 * Veritabani tasima: kaynak Postgres (Render) -> hedef Postgres (Neon)
 *
 * Baglanti adresleri api/.env.tasima dosyasindan okunur (GitHub'a gonderilmez):
 *   KAYNAK_URL=postgresql://...render.com/...      (Render -> veritabani -> External Database URL)
 *   HEDEF_URL=postgresql://...neon.tech/...?sslmode=require
 *
 * Kullanim (api klasorunde):
 *   node tasi.js            tablolari olusturur, tum kayitlari kopyalar (tekrarlar atlanir)
 *   node tasi.js --say      yalnizca iki taraftaki kayit sayilarini karsilastirir
 *
 * Tekrar calistirmak guvenlidir (ON CONFLICT DO NOTHING): gecis sirasinda gelen
 * kayitlari almak icin API yeni veritabanina gectikten sonra bir kez daha calistirilir.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { Pool } = require('pg');

function env() {
  const file = path.join(__dirname, '.env.tasima');
  if (!fs.existsSync(file)) throw new Error('.env.tasima dosyasi yok');
  const out = {};
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z_]+)\s*=\s*(.+?)\s*$/);
    if (m) out[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
  for (const k of ['KAYNAK_URL', 'HEDEF_URL']) {
    if (!out[k] || !/^postgres(ql)?:\/\//.test(out[k])) throw new Error(`${k} eksik ya da gecersiz`);
  }
  return out;
}

// node-postgres "channel_binding" parametresini tanimadigi icin adresten cikarilir
const clean = (url) => url.replace(/([?&])channel_binding=[^&]*&?/, '$1').replace(/[?&]$/, '');
const pool = (url) => new Pool({ connectionString: clean(url), ssl: { rejectUnauthorized: false }, max: 2 });

// api/server.js ile ayni sema
const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS telemetry (
     device TEXT NOT NULL, ts BIGINT NOT NULL, temp REAL, vx REAL, vy REAL, vz REAL, v REAL,
     flags INTEGER NOT NULL DEFAULT 0, received_at BIGINT NOT NULL, PRIMARY KEY (device, ts))`,
  `CREATE TABLE IF NOT EXISTS events (
     device TEXT NOT NULL, ts BIGINT NOT NULL, code TEXT NOT NULL, state TEXT NOT NULL, value REAL,
     lim REAL, descr TEXT, received_at BIGINT NOT NULL, PRIMARY KEY (device, ts, code, state))`,
];
const TABLES = {
  telemetry: ['device', 'ts', 'temp', 'vx', 'vy', 'vz', 'v', 'flags', 'received_at'],
  events: ['device', 'ts', 'code', 'state', 'value', 'lim', 'descr', 'received_at'],
};
const BATCH = 2000;

async function counts(db) {
  const out = {};
  for (const t of Object.keys(TABLES)) {
    const r = await db.query(`SELECT COUNT(*) AS n, MIN(ts) AS a, MAX(ts) AS b FROM ${t}`);
    out[t] = { n: Number(r.rows[0].n), from: Number(r.rows[0].a), to: Number(r.rows[0].b) };
  }
  return out;
}

const day = (t) => (t ? new Date(t * 1000).toLocaleString('tr-TR', { timeZone: 'Europe/Istanbul' }) : '-');

function report(src, dst) {
  for (const t of Object.keys(TABLES)) {
    const ok = src[t].n === dst[t].n ? 'ESIT' : `FARK ${src[t].n - dst[t].n}`;
    console.log(`${t.padEnd(10)} kaynak ${String(src[t].n).padStart(7)}  hedef ${String(dst[t].n).padStart(7)}  ${ok}`
      + `   (${day(src[t].from)} → ${day(src[t].to)})`);
  }
}

async function copyTable(src, dst, table) {
  const cols = TABLES[table];
  const key = table === 'telemetry' ? 'device, ts' : 'device, ts, code, state';
  let last = null, total = 0, inserted = 0;
  for (;;) {
    // Anahtar sirasina gore sayfalama (OFFSET yerine; buyuk tabloda hizli)
    const r = last
      ? await src.query(`SELECT ${cols.join(', ')} FROM ${table} WHERE (${key}) > (${last.map((_, i) => `$${i + 1}`).join(', ')})
                         ORDER BY ${key} LIMIT ${BATCH}`, last)
      : await src.query(`SELECT ${cols.join(', ')} FROM ${table} ORDER BY ${key} LIMIT ${BATCH}`);
    if (!r.rows.length) break;
    const values = [];
    const rowsSql = r.rows.map((row, i) => {
      cols.forEach((c) => values.push(row[c]));
      return `(${cols.map((_, j) => `$${i * cols.length + j + 1}`).join(', ')})`;
    });
    const ins = await dst.query(`INSERT INTO ${table} (${cols.join(', ')}) VALUES ${rowsSql.join(', ')}
                                 ON CONFLICT DO NOTHING RETURNING ts`, values);
    total += r.rows.length;
    inserted += ins.rowCount;
    const lr = r.rows[r.rows.length - 1];
    last = table === 'telemetry' ? [lr.device, lr.ts] : [lr.device, lr.ts, lr.code, lr.state];
    process.stdout.write(`\r${table}: ${total} okundu, ${inserted} yeni yazildi`);
  }
  process.stdout.write('\n');
  return { total, inserted };
}

async function main() {
  const e = env();
  const src = pool(e.KAYNAK_URL), dst = pool(e.HEDEF_URL);
  try {
    for (const s of SCHEMA) await dst.query(s);
    if (!process.argv.includes('--say')) {
      for (const t of Object.keys(TABLES)) await copyTable(src, dst, t);
    }
    report(await counts(src), await counts(dst));
  } finally {
    await src.end();
    await dst.end();
  }
}

main().catch((err) => { console.error('HATA:', err.message); process.exit(1); });
