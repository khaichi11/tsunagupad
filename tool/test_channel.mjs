#!/usr/bin/env node
/* Uji ujung ke ujung kanal aman iPad <-> komputer terhadap daemon yang
 * sedang berjalan (biasanya `tsunagupad share --test-source --encoder cpu`).
 *
 *   node tool/test_channel.mjs PORT FILE_KODE_AKSES
 *
 * Memakai web/secure.js yang sama persis dengan halaman iPad, lalu memeriksa:
 * kode benar diterima dan offer WebRTC datang terenkripsi, kode salah dan
 * login gaya lama ditolak, pesan yang diputar ulang atau diubah ditolak, dan
 * alamat dikunci setelah terlalu banyak percobaan.
 * SPDX-License-Identifier: MIT
 */
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const [, , port, tokenFile] = process.argv;
const token = fs.readFileSync(tokenFile, 'utf8').trim();

const sandbox = { TextEncoder, TextDecoder, btoa, atob, crypto, Uint8Array, Uint32Array, DataView, Math, JSON, Error };
vm.createContext(sandbox);
vm.runInContext(`${fs.readFileSync(path.join(here, '..', 'web', 'secure.js'), 'utf8')}\nthis.T = TsunaguSecure;`, sandbox);
const T = sandbox.T;

function session(mode) {
  return new Promise((resolve) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);
    ws.binaryType = 'arraybuffer';
    const seen = [];
    let ch = null;
    const finish = () => { try { ws.close(); } catch { /* sudah tertutup */ } resolve(seen); };
    const timer = setTimeout(finish, 20000);
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        const m = JSON.parse(ev.data);
        seen.push(`text:${m.type}`);
        if (m.type === 'challenge') {
          ch = new T.Channel(mode === 'bad' ? `${token}x` : token, m.nonce);
          ws.send(JSON.stringify(mode === 'legacy' ? { type: 'hello', token } : ch.hello));
        }
        return;
      }
      if (mode === 'tamper') {
        const b = new Uint8Array(ev.data.slice(0));
        b[b.length - 1] ^= 1;
        seen.push(ch.open(b.buffer) === null ? 'tamper-rejected' : 'TAMPER-ACCEPTED');
        clearTimeout(timer);
        finish();
        return;
      }
      const m = ch.open(ev.data);
      seen.push(`bin:${m ? m.type : 'BAD'}`);
      if (m && m.type === 'welcome') {
        ws.send(ch.seal({ type: 'ping', t: 1 }));
        if (mode === 'replay') {
          const frame = ch.seal({ type: 'ping', t: 2 });
          ws.send(frame);
          ws.send(frame);
        }
      }
      if (m && m.type === 'offer' && mode === 'good') {
        clearTimeout(timer);
        finish();
      }
    };
    ws.onclose = (ev) => { seen.push(`close:${ev.code}`); clearTimeout(timer); finish(); };
    ws.onerror = () => {};
  });
}

const checks = [];
function expect(name, ok, detail) {
  checks.push(ok);
  console.log(`${ok ? 'OK  ' : 'FAIL'} ${name}${ok ? '' : `: ${JSON.stringify(detail)}`}`);
}

const good = await session('good');
expect('kode benar: welcome dan offer WebRTC terenkripsi', good.includes('bin:welcome') && good.includes('bin:offer'), good);
const bad = await session('bad');
expect('kode salah ditolak', bad.includes('close:4001') && !bad.some((s) => s.startsWith('bin:')), bad);
const legacy = await session('legacy');
expect('login lama (kode terkirim apa adanya) ditolak', legacy.includes('close:4001'), legacy);
const replay = await session('replay');
expect('pesan yang diputar ulang memutus sambungan', replay.includes('close:4002'), replay);
const tamper = await session('tamper');
expect('pesan dari server yang diubah ditolak halaman', tamper.includes('tamper-rejected'), tamper);
for (let i = 0; i < 5; i++) await session('bad');
const locked = await session('good');
expect('alamat dikunci setelah lima kode salah', locked.includes('close:4004'), locked);

const res = await fetch(`http://127.0.0.1:${port}/`);
const csp = res.headers.get('content-security-policy') || '';
expect('header keamanan HTTP', csp.includes("frame-ancestors 'none'") &&
  res.headers.get('x-content-type-options') === 'nosniff' && res.headers.get('referrer-policy') === 'no-referrer',
  Object.fromEntries(res.headers));
const page = await res.text();
expect('halaman memuat secure.js', page.includes('secure.js'), page.slice(0, 200));

process.exit(checks.every(Boolean) ? 0 : 1);
