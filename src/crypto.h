/* Kanal aman WebSocket: lihat src/crypto.c.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>

#define NONCE_LEN 32
/* Bingkai biner: nomor urut (8 byte, big-endian), HMAC-SHA256 (32 byte), isi terenkripsi. */
#define FRAME_HEADER 40

typedef struct {
    guint8 rx_enc[32], rx_mac[32];  /* iPad -> komputer */
    guint8 tx_enc[32], tx_mac[32];  /* komputer -> iPad */
    guint64 rx_seq, tx_seq;
} Channel;

gboolean secure_random(guint8 *buf, gsize len);
gboolean equal_time(const guint8 *a, const guint8 *b, gsize len);
void hmac_sha256(const guint8 *key, gsize key_len, const guint8 *const *parts, const gsize *lens,
                 guint n, guint8 out[32]);
void chacha20_xor(const guint8 key[32], const guint8 nonce[12], guint32 counter,
                  guint8 *data, gsize len);

void channel_proof(const char *token, const guint8 sn[NONCE_LEN], const guint8 cn[NONCE_LEN],
                   guint8 out[32]);
void channel_init(Channel *c, const char *token, const guint8 sn[NONCE_LEN], const guint8 cn[NONCE_LEN]);
GBytes *channel_seal(Channel *c, const char *text, gsize len);
/* Mengembalikan teks JSON (diakhiri NUL) atau NULL bila bingkainya palsu atau tidak berurutan. */
char *channel_open(Channel *c, const guint8 *frame, gsize len);
