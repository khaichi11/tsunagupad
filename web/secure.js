/* Tsunagu-Pad, kanal aman untuk halaman iPad.
 *
 * Pasangan dari src/crypto.c. Halaman ini dibuka lewat http biasa, dan di
 * situ Safari tidak menyediakan crypto.subtle, jadi SHA-256, HMAC, dan
 * ChaCha20 ditulis sendiri di sini. Yang dipakai dari browser hanya
 * crypto.getRandomValues, yang tersedia juga tanpa https.
 *
 * Alurnya: server mengirim nonce, halaman menjawab dengan nonce sendiri dan
 * HMAC-SHA256(kode akses, nonce server, nonce iPad). Kode aksesnya tidak
 * pernah dikirim. Dari situ diturunkan kunci per arah; setiap pesan
 * dienkripsi ChaCha20 dan diberi HMAC atas nomor urut dan isinya.
 * SPDX-License-Identifier: MIT
 */
'use strict';

const TsunaguSecure = (() => {
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);
  const W = new Uint32Array(64);

  function concat(parts) {
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  }

  function sha256(data) {
    const bits = data.length * 8;
    const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
    padded.set(data);
    padded[data.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, Math.floor(bits / 0x100000000));
    view.setUint32(padded.length - 4, bits >>> 0);

    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    for (let off = 0; off < padded.length; off += 64) {
      for (let i = 0; i < 16; i++) W[i] = view.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const a = W[i - 15], b = W[i - 2];
        const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
        const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
        W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
      for (let i = 0; i < 64; i++) {
        const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
        const ch = (e & f) ^ (~e & g);
        const t1 = (h + S1 + ch + K[i] + W[i]) | 0;
        const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0;
        d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
      h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
    }
    const out = new Uint8Array(32);
    const ov = new DataView(out.buffer);
    [h0, h1, h2, h3, h4, h5, h6, h7].forEach((v, i) => ov.setUint32(i * 4, v));
    return out;
  }

  function hmac(key, ...parts) {
    let k = key.length > 64 ? sha256(key) : key;
    const block = new Uint8Array(64);
    block.set(k);
    const ipad = block.map((b) => b ^ 0x36);
    const opad = block.map((b) => b ^ 0x5c);
    return sha256(concat([opad, sha256(concat([ipad, ...parts]))]));
  }

  function rotl(v, n) { return (v << n) | (v >>> (32 - n)); }

  /* ChaCha20 RFC 8439; data diubah di tempat. */
  function chacha20(key, nonce, counter, data) {
    const kv = new DataView(key.buffer, key.byteOffset, 32);
    const nv = new DataView(nonce.buffer, nonce.byteOffset, 12);
    const s = new Uint32Array(16);
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (let i = 0; i < 8; i++) s[4 + i] = kv.getUint32(i * 4, true);
    s[12] = counter;
    for (let i = 0; i < 3; i++) s[13 + i] = nv.getUint32(i * 4, true);
    const x = new Uint32Array(16);
    const block = new Uint8Array(64);
    const bv = new DataView(block.buffer);
    const qr = (a, b, c, d) => {
      x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16);
      x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12);
      x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);
      x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);
    };
    for (let off = 0; off < data.length; off += 64) {
      x.set(s);
      for (let i = 0; i < 10; i++) {
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
      }
      for (let i = 0; i < 16; i++) bv.setUint32(i * 4, (x[i] + s[i]) >>> 0, true);
      s[12]++;
      const n = Math.min(64, data.length - off);
      for (let i = 0; i < n; i++) data[off + i] ^= block[i];
    }
    return data;
  }

  const enc = new TextEncoder();
  const dec = new TextDecoder();

  function b64(bytes) {
    let s = '';
    for (const b of bytes) s += String.fromCharCode(b);
    return btoa(s);
  }

  function unb64(text) {
    const s = atob(text);
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
    return out;
  }

  function random(n) {
    const out = new Uint8Array(n);
    crypto.getRandomValues(out);
    return out;
  }

  function seqBytes(seq) {
    const out = new Uint8Array(8);
    const v = new DataView(out.buffer);
    v.setUint32(0, Math.floor(seq / 0x100000000));
    v.setUint32(4, seq >>> 0);
    return out;
  }

  function equal(a, b) {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
    return diff === 0;
  }

  /* Satu sambungan WebSocket = satu kanal. */
  class Channel {
    constructor(token, serverNonce) {
      const key = enc.encode(token);
      const sn = unb64(serverNonce);
      if (sn.length !== 32) throw new Error('nonce');
      const cn = random(32);
      const derive = (label) => hmac(key, enc.encode(label), new Uint8Array([0]), sn, cn);
      this.hello = { type: 'hello', v: 2, nonce: b64(cn), proof: b64(derive('tsunagupad auth')) };
      this.txEnc = derive('tsunagupad c2s enc');
      this.txMac = derive('tsunagupad c2s mac');
      this.rxEnc = derive('tsunagupad s2c enc');
      this.rxMac = derive('tsunagupad s2c mac');
      this.txSeq = 0;
      this.rxSeq = 0;
    }

    seal(msg) {
      const body = enc.encode(JSON.stringify(msg));
      const seq = seqBytes(++this.txSeq);
      const nonce = new Uint8Array(12);
      nonce.set(seq, 4);
      chacha20(this.txEnc, nonce, 0, body);
      const frame = new Uint8Array(40 + body.length);
      frame.set(seq, 0);
      frame.set(hmac(this.txMac, seq, body), 8);
      frame.set(body, 40);
      return frame.buffer;
    }

    /* Mengembalikan pesan, atau null bila bingkainya palsu atau tidak berurutan. */
    open(buffer) {
      const frame = new Uint8Array(buffer);
      if (frame.length < 40) return null;
      const seq = frame.subarray(0, 8);
      if (!equal(seq, seqBytes(this.rxSeq + 1))) return null;
      const body = frame.slice(40);
      if (!equal(hmac(this.rxMac, seq, body), frame.subarray(8, 40))) return null;
      this.rxSeq++;
      const nonce = new Uint8Array(12);
      nonce.set(seq, 4);
      chacha20(this.rxEnc, nonce, 0, body);
      try { return JSON.parse(dec.decode(body)); } catch { return null; }
    }
  }

  return { Channel, sha256, hmac, chacha20, b64, unb64 };
})();
