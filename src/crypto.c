/* Kanal aman antara iPad dan komputer, tanpa TLS.
 *
 * Halaman iPad dibuka lewat http biasa (Safari tidak mau sertifikat buatan
 * sendiri untuk WebSocket), jadi isi kanal WebSocket dilindungi di sini:
 *
 *   1. Kode akses hanya ada di QR (bagian #k=… yang tidak pernah dikirim
 *      browser ke server). Saat menyambung, server mengirim nonce acak; iPad
 *      menjawab HMAC-SHA256(kode, nonce server, nonce iPad). Kode aksesnya
 *      sendiri tidak pernah lewat jaringan.
 *   2. Dari kedua nonce dan kode akses diturunkan empat kunci sesi, satu
 *      pasang per arah. Setiap pesan dienkripsi ChaCha20 (RFC 8439) lalu
 *      diautentikasi HMAC-SHA256 atas nomor urut dan isinya
 *      (encrypt-then-MAC). Nomor urut harus naik satu per satu, jadi pesan
 *      lama tidak bisa diputar ulang dan pesan palsu ditolak.
 *
 * Akibatnya ketikan dari iPad (termasuk kata sandi di layar login) tidak bisa
 * dibaca orang lain di jaringan yang sama, perintah tidak bisa disisipkan,
 * dan sidik jari DTLS di SDP ikut terautentikasi sehingga video WebRTC tidak
 * bisa dibelokkan. Implementasi yang sama ada di web/secure.js.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

#include "crypto.h"

gboolean secure_random(guint8 *buf, gsize len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    gsize done = 0;
    while (done < len) {
        ssize_t n = getrandom(buf + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        done += (gsize) n;
    }
    return TRUE;
#endif
}

/* ---------- HMAC-SHA256 ---------- */

void hmac_sha256(const guint8 *key, gsize key_len, const guint8 *const *parts, const gsize *lens,
                 guint n, guint8 out[32])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, key_len);
    for (guint i = 0; i < n; i++)
        g_hmac_update(h, parts[i], lens[i]);
    gsize size = 32;
    g_hmac_get_digest(h, out, &size);
    g_hmac_unref(h);
}

gboolean equal_time(const guint8 *a, const guint8 *b, gsize len)
{
    guint8 diff = 0;
    for (gsize i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

/* ---------- ChaCha20 (RFC 8439) ---------- */

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); \
    c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  \
    c += d; b ^= c; b = ROTL(b, 7)

static guint32 load32(const guint8 *p)
{
    return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

static void chacha20_block(const guint32 in[16], guint8 out[64])
{
    guint32 x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        guint32 v = x[i] + in[i];
        out[i * 4] = (guint8) v;
        out[i * 4 + 1] = (guint8) (v >> 8);
        out[i * 4 + 2] = (guint8) (v >> 16);
        out[i * 4 + 3] = (guint8) (v >> 24);
    }
}

void chacha20_xor(const guint8 key[32], const guint8 nonce[12], guint32 counter,
                  guint8 *data, gsize len)
{
    guint32 state[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    for (int i = 0; i < 8; i++)
        state[4 + i] = load32(key + i * 4);
    state[12] = counter;
    for (int i = 0; i < 3; i++)
        state[13 + i] = load32(nonce + i * 4);

    guint8 block[64];
    for (gsize off = 0; off < len; off += 64) {
        chacha20_block(state, block);
        state[12]++;
        gsize n = MIN(64, len - off);
        for (gsize i = 0; i < n; i++)
            data[off + i] ^= block[i];
    }
}

/* ---------- kanal ---------- */

static void derive(const char *token, const char *label, const guint8 sn[NONCE_LEN],
                   const guint8 cn[NONCE_LEN], guint8 out[32])
{
    static const guint8 zero = 0;
    const guint8 *parts[] = { (const guint8 *) label, &zero, sn, cn };
    const gsize lens[] = { strlen(label), 1, NONCE_LEN, NONCE_LEN };
    hmac_sha256((const guint8 *) token, strlen(token), parts, lens, G_N_ELEMENTS(parts), out);
}

void channel_proof(const char *token, const guint8 sn[NONCE_LEN], const guint8 cn[NONCE_LEN],
                   guint8 out[32])
{
    derive(token, "tsunagupad auth", sn, cn, out);
}

void channel_init(Channel *c, const char *token, const guint8 sn[NONCE_LEN], const guint8 cn[NONCE_LEN])
{
    derive(token, "tsunagupad c2s enc", sn, cn, c->rx_enc);
    derive(token, "tsunagupad c2s mac", sn, cn, c->rx_mac);
    derive(token, "tsunagupad s2c enc", sn, cn, c->tx_enc);
    derive(token, "tsunagupad s2c mac", sn, cn, c->tx_mac);
    c->rx_seq = 0;
    c->tx_seq = 0;
}

static void put_seq(guint8 out[8], guint64 seq)
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (guint8) seq;
        seq >>= 8;
    }
}

static void frame_mac(const guint8 key[32], const guint8 seq[8], const guint8 *data, gsize len,
                      guint8 out[32])
{
    const guint8 *parts[] = { seq, data };
    const gsize lens[] = { 8, len };
    hmac_sha256(key, 32, parts, lens, 2, out);
}

GBytes *channel_seal(Channel *c, const char *text, gsize len)
{
    guint8 *frame = g_malloc(FRAME_HEADER + len);
    c->tx_seq++;
    put_seq(frame, c->tx_seq);
    guint8 nonce[12] = { 0 };
    memcpy(nonce + 4, frame, 8);
    memcpy(frame + FRAME_HEADER, text, len);
    chacha20_xor(c->tx_enc, nonce, 0, frame + FRAME_HEADER, len);
    frame_mac(c->tx_mac, frame, frame + FRAME_HEADER, len, frame + 8);
    return g_bytes_new_take(frame, FRAME_HEADER + len);
}

char *channel_open(Channel *c, const guint8 *frame, gsize len)
{
    if (len < FRAME_HEADER)
        return NULL;
    guint8 expect[8], mac[32];
    put_seq(expect, c->rx_seq + 1);
    if (memcmp(frame, expect, 8) != 0)
        return NULL;    /* hilang, terulang, atau tertukar urutannya */
    frame_mac(c->rx_mac, frame, frame + FRAME_HEADER, len - FRAME_HEADER, mac);
    if (!equal_time(mac, frame + 8, 32))
        return NULL;
    c->rx_seq++;

    gsize n = len - FRAME_HEADER;
    char *text = g_malloc(n + 1);
    memcpy(text, frame + FRAME_HEADER, n);
    guint8 nonce[12] = { 0 };
    memcpy(nonce + 4, frame, 8);
    chacha20_xor(c->rx_enc, nonce, 0, (guint8 *) text, n);
    text[n] = '\0';
    return text;
}
