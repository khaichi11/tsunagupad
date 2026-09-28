/* QR code alamat sambungan, dikirim ke panel sebagai baris "0101,1010,...".
 * SPDX-License-Identifier: MIT
 */
#include <qrencode.h>

#include "tsunagupad.h"

char *qr_rows(const char *text)
{
    QRcode *q = QRcode_encodeString(text, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!q)
        return NULL;

    GString *s = g_string_sized_new(q->width * (q->width + 1));
    for (int y = 0; y < q->width; y++) {
        if (y)
            g_string_append_c(s, ',');
        for (int x = 0; x < q->width; x++)
            g_string_append_c(s, (q->data[y * q->width + x] & 1) ? '1' : '0');
    }
    QRcode_free(q);
    return g_string_free(s, FALSE);
}
