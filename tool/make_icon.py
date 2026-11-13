#!/usr/bin/env python3
"""Menggambar ikon Tsunagu-Pad untuk Windows (.ico) tanpa pustaka tambahan.

Bentuknya sama dengan ikon di area notifikasi yang digambar tray/tray.c:
sebuah tablet dengan bingkai biru dan layar gelap. Ikon dibuat saat build,
jadi repository tidak menyimpan berkas gambar biner.

    python3 tool/make_icon.py build/tsunagupad.ico

SPDX-License-Identifier: MIT
"""
import struct
import sys

ACCENT = (0x0A, 0x84, 0xFF)
SCREEN = (0x1C, 0x1C, 0x1E)
DOT = (0xFF, 0xFF, 0xFF)


def coverage(fx, fy, x0, y0, x1, y1, radius):
    """Bagian piksel (0..1) yang tertutup persegi bersudut bulat, dengan 4x4 sampel."""
    hit = 0
    for sy in range(4):
        for sx in range(4):
            px = fx + (sx + 0.5) / 4
            py = fy + (sy + 0.5) / 4
            if not (x0 <= px < x1 and y0 <= py < y1):
                continue
            cx = min(max(px, x0 + radius), x1 - radius)
            cy = min(max(py, y0 + radius), y1 - radius)
            if (px - cx) ** 2 + (py - cy) ** 2 <= radius ** 2:
                hit += 1
    return hit / 16


def draw(size):
    s = size / 16.0
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = x / s, y / s
            body = coverage(fx, fy, 1.0, 2.0, 15.0, 14.0, 2.2) if s else 0
            screen = coverage(fx, fy, 2.4, 3.4, 13.6, 12.6, 1.0)
            dot = coverage(fx, fy, 6.6, 6.6, 9.4, 9.4, 1.4)
            r, g, b = ACCENT
            if screen:
                r = round(ACCENT[0] * (1 - screen) + SCREEN[0] * screen)
                g = round(ACCENT[1] * (1 - screen) + SCREEN[1] * screen)
                b = round(ACCENT[2] * (1 - screen) + SCREEN[2] * screen)
            if dot:
                r = round(r * (1 - dot) + DOT[0] * dot)
                g = round(g * (1 - dot) + DOT[1] * dot)
                b = round(b * (1 - dot) + DOT[2] * dot)
            a = round(255 * body)
            row.append((b, g, r, a))
        pixels.append(row)
    return pixels


def bmp_entry(size):
    pixels = draw(size)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b''.join(bytes(p) for row in reversed(pixels) for p in row)
    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytes(mask_stride * size)
    return header + xor + and_mask


def main(path):
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [bmp_entry(n) for n in sizes]
    out = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for n, data in zip(sizes, images):
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b''.join(images)
    with open(path, 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'tsunagupad.ico')
