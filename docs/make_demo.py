#!/usr/bin/env python3
"""Membuat motion graphic dokumentasi Tsunagu-Pad.

Semua gambar digambar dari nol, bukan rekaman layar, jadi tidak ada isi layar
pribadi yang ikut terbawa ke repository. Jalankan dari akar proyek:

    python3 docs/make_demo.py            # semuanya
    python3 docs/make_demo.py share      # satu adegan saja

Hasil di docs/media: demo-share.gif, demo-second.gif, demo-airplay.gif, dan
versi berbahasa Indonesia dengan akhiran -id.

Gayanya minimalis seperti iklan produk: latar gelap bergaris tipis, bentuk
bergaris halus, satu warna aksen. Tidak ada yang memantul; semuanya masuk
dengan percepatan lalu perlambatan dan disingkap lewat topeng yang bergerak.
Tiap frame dirender beberapa kali lalu dirata-ratakan supaya ada jejak geraknya,
dan semuanya digambar dua kali lipat lalu dikecilkan agar tepinya halus.

Butuh python3-pil dan ffmpeg. Teks memakai Poppins bila tersedia; fontnya
berlisensi SIL Open Font License dan tidak disalin ke repository ini. Ambil
sekali dengan:

    mkdir -p ~/.cache/tsunagupad-fonts && cd ~/.cache/tsunagupad-fonts
    curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-SemiBold.ttf
    curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-Medium.ttf
    curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-Regular.ttf

SPDX-License-Identifier: MIT
"""

import math
import pathlib
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT_W, OUT_H = 880, 495
SS = 2                      # digambar sebesar ini, lalu dikecilkan
W, H = OUT_W * SS, OUT_H * SS
FPS = 25
SUB = 2                     # cuplikan per frame, dirata-ratakan jadi jejak gerak

BG = (14, 17, 22)           # latar gelap
RULE = (22, 26, 33)         # garis latar
PANEL = (18, 22, 29)        # badan perangkat dan kartu
HAIR = (58, 68, 83)         # garis bentuk
INK = (233, 237, 244)       # teks utama, pena, penunjuk
GREY = (138, 149, 166)      # teks pendamping
GLASS = (28, 35, 48)        # isi layar perangkat
TINT = (40, 49, 63)         # isi bentuk di dalam layar
ACCENT = (167, 139, 250)   # ungu terang

STROKE = 1.6                # tebal garis dalam satuan tayang

FONT_DIRS = [pathlib.Path.home() / ".cache/tsunagupad-fonts",
             pathlib.Path("/usr/share/fonts/truetype")]
FALLBACK = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def find_font(*names):
    for base in FONT_DIRS:
        for name in names:
            for path in base.rglob(name):
                return str(path)
    return FALLBACK


SEMI = find_font("Poppins-SemiBold.ttf", "Poppins-Medium.ttf")
MED = find_font("Poppins-Medium.ttf", "Poppins-Regular.ttf", "Inter-Medium.ttf")
REG = find_font("Poppins-Regular.ttf", "Inter-Regular.ttf", "Poppins-Medium.ttf")

_fonts = {}


def font(path, size):
    key = (path, int(size * SS))
    if key not in _fonts:
        try:
            _fonts[key] = ImageFont.truetype(path, int(size * SS))
        except OSError:
            _fonts[key] = ImageFont.load_default()
    return _fonts[key]


# ---------- waktu dan percepatan ----------

def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def span(t, start, end):
    return clamp((t - start) / max(1e-6, end - start))


def ease(t):
    """Percepatan lalu perlambatan; dipakai hampir di semua gerakan."""
    t = clamp(t)
    return 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


def ease_soft(t):
    t = clamp(t)
    return t * t * (3 - 2 * t)


def out_quint(t):
    t = clamp(t)
    return 1 - (1 - t) ** 5


def mix(a, b, k):
    return a + (b - a) * k


def mixp(a, b, k):
    return (mix(a[0], b[0], k), mix(a[1], b[1], k))


# ---------- kuas dasar ----------

def px(*v):
    return tuple(int(round(x * SS)) for x in v)


def layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def over(img, lay):
    return Image.alpha_composite(img, lay)


def rr(d, box, radius, fill=None, line_color=HAIR, width=STROKE, alpha=255):
    d.rounded_rectangle(px(*box), radius * SS,
                        fill=(tuple(fill) + (alpha,)) if fill else None,
                        outline=(tuple(line_color) + (alpha,)) if line_color else None,
                        width=max(1, int(width * SS)) if line_color else 0)


def circle(d, x, y, r, fill=None, line_color=HAIR, width=STROKE, alpha=255):
    d.ellipse(px(x - r, y - r, x + r, y + r),
              fill=(tuple(fill) + (alpha,)) if fill else None,
              outline=(tuple(line_color) + (alpha,)) if line_color else None,
              width=max(1, int(width * SS)) if line_color else 0)


def seg(d, a, b, color=HAIR, width=STROKE, alpha=255):
    d.line(px(a[0], a[1], b[0], b[1]), fill=tuple(color) + (alpha,),
           width=max(1, int(width * SS)))


def text(d, s, xy, size=14, color=INK, path=None, center=False, alpha=255, track=0.0):
    """Teks biasa; track merenggangkan huruf sedikit untuk judul."""
    f = font(path or REG, size)
    x, y = px(*xy)
    if track <= 0:
        if center:
            x -= d.textlength(s, font=f) / 2
        d.text((x, y), s, font=f, fill=tuple(color) + (int(alpha),))
        return
    gap = track * SS
    total = sum(d.textlength(c, font=f) + gap for c in s) - gap
    if center:
        x -= total / 2
    for c in s:
        d.text((x, y), c, font=f, fill=tuple(color) + (int(alpha),))
        x += d.textlength(c, font=f) + gap


def text_w(d, s, size, path=None):
    return d.textlength(s, font=font(path or REG, size)) / SS


def reveal(lay, box, k, feather=3):
    """Menyingkap isi layer dari bawah ke atas di dalam kotak."""
    if k >= 1:
        return lay
    if k <= 0:
        return layer()
    x0, y0, x1, y1 = box
    m = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(m)
    cut = mix(y1, y0, k)
    md.rectangle(px(x0 - 60, cut, x1 + 60, y1 + 80), fill=255)
    if feather:
        m = m.filter(ImageFilter.GaussianBlur(feather * SS))
    out = lay.copy()
    out.putalpha(Image.composite(out.getchannel("A"), Image.new("L", (W, H), 0), m))
    return out


# ---------- panggung ----------

def stage(t):
    """Gelap dengan garis tegak tipis dan satu garis alas. Latarnya diam supaya
    yang bergerak hanya bendanya, dan supaya GIF-nya tetap kecil."""
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    step = 40
    for i in range(1, OUT_W // step + 1):
        d.line(px(i * step, 0, i * step, OUT_H), fill=RULE, width=max(1, int(SS)))
    d.line(px(0, 436, OUT_W, 436), fill=(25, 29, 37), width=max(1, int(SS)))
    return img.convert("RGBA")


# ---------- isi layar ----------

def picture(img, box, k=1.0, framed=False, alpha=255):
    """Isi layar: foto tangga yang sedang dilabeli, digambar dengan garis saja."""
    if k <= 0.02:
        return img
    if framed:
        sh = layer()
        ImageDraw.Draw(sh).rounded_rectangle(px(box[0], box[1] + 7, box[2], box[3] + 7),
                                             5 * SS, fill=(0, 0, 0, 120))
        img = over(img, sh.filter(ImageFilter.GaussianBlur(5 * SS)))
    lay = layer()
    d = ImageDraw.Draw(lay)
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    a = int(alpha * clamp(k))
    rr(d, box, 5, GLASS, HAIR if framed else None, STROKE, a)
    seg(d, (x0 + w * 0.08, y0 + h * 0.72), (x1 - w * 0.08, y0 + h * 0.72), TINT, 1.2, a)
    steps = 4
    for i in range(steps):
        sw = w * (0.48 - i * 0.03)
        sh = h * 0.1
        left = x0 + w * 0.18 + i * w * 0.02
        top = y1 - h * 0.13 - (i + 1) * sh
        rr(d, (left, top, left + sw, top + sh), 2, TINT, HAIR, 1.2, a)
    circle(d, x1 - w * 0.17, y0 + h * 0.23, min(w, h) * 0.07, None, HAIR, 1.2, a)
    return over(img, lay)


def window_card(d, box, alpha=255, lines=3, accent=False):
    x0, y0, x1, y1 = box
    col = ACCENT if accent else HAIR
    rr(d, box, 5, PANEL, col, STROKE, alpha)
    seg(d, (x0, y0 + 15), (x1, y0 + 15), col, 1.2, alpha)
    for i in range(3):
        circle(d, x0 + 10 + i * 8, y0 + 7.5, 2.1, None, col, 1.2, alpha)
    for i in range(lines):
        wdt = (x1 - x0 - 24) * (0.9 - i * 0.22)
        yy = y0 + 26 + i * 11
        if yy > y1 - 8:
            break
        rr(d, (x0 + 12, yy, x0 + 12 + wdt, yy + 4), 2, TINT, None, 0, alpha)


# ---------- perangkat ----------

def laptop_art(x, y, w, h):
    """Laptop sebagai layer tersendiri, supaya bisa disingkap dengan topeng."""
    lay = layer()
    d = ImageDraw.Draw(lay)
    rr(d, (x, y, x + w, y + h), 11, PANEL, HAIR, STROKE)
    screen = (x + 10, y + 10, x + w - 10, y + h - 16)
    rr(d, screen, 6, GLASS, HAIR, 1.2)
    rr(d, (x - 18, y + h + 3, x + w + 18, y + h + 13), 5, PANEL, HAIR, STROKE)
    seg(d, (x + w / 2 - 20, y + h + 8), (x + w / 2 + 20, y + h + 8), HAIR, 1.2)
    return lay, screen, (x - 20, y, x + w + 20, y + h + 15)


def tablet_art(x, y, w, h):
    lay = layer()
    d = ImageDraw.Draw(lay)
    rr(d, (x, y, x + w, y + h), 14, PANEL, HAIR, STROKE)
    screen = (x + 9, y + 9, x + w - 9, y + h - 9)
    rr(d, screen, 8, GLASS, HAIR, 1.2)
    return lay, screen, (x, y, x + w, y + h)


def enter(img, t, start, art, dur=0.95, rise=20):
    """Benda naik sedikit sambil disingkap dari bawah. Tidak memantul."""
    k = ease(span(t, start, start + dur))
    lay, screen, bounds = art(0.0 if k >= 1 else (1 - k) * rise)
    if k <= 0.002:
        return img, screen
    return over(img, reveal(lay, bounds, ease_soft(k))), screen


# ---------- benda kecil ----------

def pen(lay, x, y, lift=0.0):
    """Pena tipis. Ujungnya di titik (x, y); saat terangkat ia menjauh sedikit
    dan miring lebih tegak, seperti tangan yang mengangkat pena."""
    d = ImageDraw.Draw(lay)
    angle = math.radians(-52 - lift * 10)
    ux, uy = math.cos(angle), math.sin(angle)
    nx, ny = -uy, ux
    tipx, tipy = x + lift * 5, y - lift * 7

    def at(along, side=0.0):
        return (tipx + ux * along + nx * side, tipy + uy * along + ny * side)

    d.polygon([px(*at(0)), px(*at(11, 3.2)), px(*at(11, -3.2))], fill=tuple(INK) + (255,))
    seg(d, at(11), at(50), INK, 4.6)
    seg(d, at(50), at(57), ACCENT, 4.6)
    if lift < 0.25:                     # bayangan kecil saat menyentuh layar
        circle(d, x, y, 2.2, None, ACCENT, 1.2, int(180 * (1 - lift * 4)))


def pointer(lay, x, y, alpha=255):
    d = ImageDraw.Draw(lay)
    body = [(x, y), (x, y + 16), (x + 4.2, y + 12.4), (x + 7, y + 18.4),
            (x + 9.6, y + 17.2), (x + 6.8, y + 11.4), (x + 11.6, y + 11.2)]
    d.polygon([px(*p) for p in body], fill=tuple(INK) + (alpha,))


def tick(lay, x, y, k):
    """Denyut tipis saat satu titik ditandai."""
    if k <= 0 or k >= 1:
        return
    circle(ImageDraw.Draw(lay), x, y, mix(4, 22, out_quint(k)), None, ACCENT, 1.2,
           int(150 * (1 - k) ** 2))


def dashed(d, a, b, phase, color=HAIR, width=1.3, dash=7, gap=7, alpha=255):
    (ax, ay), (bx, by) = a, b
    total = math.hypot(bx - ax, by - ay)
    if total < 1:
        return
    ux, uy = (bx - ax) / total, (by - ay) / total
    pos = -((phase * (dash + gap)) % (dash + gap))
    while pos < total:
        s, e = max(0.0, pos), min(total, pos + dash)
        if e > s:
            seg(d, (ax + ux * s, ay + uy * s), (ax + ux * e, ay + uy * e), color, width, alpha)
        pos += dash + gap


def chip(d, cx, cy, s, alpha=255):
    w = text_w(d, s, 11.5, MED)
    rr(d, (cx - w / 2 - 10, cy - 10, cx + w / 2 + 10, cy + 10), 10, PANEL, HAIR, 1.2, alpha)
    text(d, s, (cx, cy - 7), 11.5, GREY, MED, center=True, alpha=alpha)


# ---------- kerangka adegan ----------

def wordmark(img, t, subtitle):
    lay = layer()
    text(ImageDraw.Draw(lay), "Tsunagu-Pad", (54, 34), 27, INK, SEMI, track=0.4)
    img = over(img, reveal(lay, (46, 26, 430, 74), ease(span(t, 0.1, 0.95)), 2))

    sub = layer()
    text(ImageDraw.Draw(sub), subtitle, (56, 78), 13.5, GREY, REG)
    img = over(img, reveal(sub, (48, 72, 600, 98), ease(span(t, 0.35, 1.2)), 2))

    rule = layer()
    g = ease(span(t, 0.22, 1.1))
    if g > 0:
        seg(ImageDraw.Draw(rule), (56, 106), (56 + 92 * g, 106), ACCENT, 2.2)
    return over(img, rule)


def caption(img, t, start, note):
    k = ease(span(t, start, start + 0.7))
    if k <= 0.01:
        return img
    lay = layer()
    text(ImageDraw.Draw(lay), note, (OUT_W / 2, OUT_H - 44), 13.5, GREY, REG, center=True)
    return over(img, reveal(lay, (0, OUT_H - 54, OUT_W, OUT_H - 18), k, 2))


def tag(img, t, start, xy, s):
    k = ease(span(t, start, start + 0.6))
    if k <= 0.02:
        return img
    lay = layer()
    chip(ImageDraw.Draw(lay), xy[0], xy[1] + (1 - k) * 5, s, int(255 * k))
    return over(img, lay)


TEXTS = {
    "en": {
        "share_sub": "Your Ubuntu screen on the iPad, and the iPad drives it",
        "share_foot": "Mark a point on the iPad; Ubuntu has it a moment later.",
        "second_sub": "The iPad as a second screen, not a copy of the first",
        "second_foot": "Drag a window to the right and it keeps running over there.",
        "airplay_sub": "The iPad screen on Ubuntu, over AirPlay",
        "airplay_foot": "Control Centre, Screen Mirroring, then Tsunagu-Pad Ubuntu",
        "ubuntu": "Ubuntu", "ipad": "iPad", "second": "second screen",
    },
    "id": {
        "share_sub": "Layar Ubuntu di iPad, dan iPad yang mengendalikannya",
        "share_foot": "Tandai satu titik di iPad, Ubuntu langsung ikut menandai.",
        "second_sub": "iPad jadi layar kedua, bukan salinan layar pertama",
        "second_foot": "Seret jendela ke kanan, jalannya terus di sana.",
        "airplay_sub": "Layar iPad tampil di Ubuntu lewat AirPlay",
        "airplay_foot": "Pusat Kontrol, Pencerminan Layar, lalu Tsunagu-Pad Ubuntu",
        "ubuntu": "Ubuntu", "ipad": "iPad", "second": "layar kedua",
    },
}

MARKS = [(0.27, 0.66), (0.38, 0.33), (0.62, 0.29), (0.75, 0.58), (0.52, 0.78)]
SHARE_LEN = 7.2


def scene_share(t, s):
    img = stage(t)

    img, lap = enter(img, t, 0.5, lambda lift: laptop_art(64, 150 + lift, 338, 218))
    img = picture(img, lap, ease(span(t, 1.2, 1.8)))
    img, tab = enter(img, t, 1.3, lambda lift: tablet_art(534, 158 + lift, 288, 202))

    lay = layer()
    d = ImageDraw.Draw(lay)
    link = ease(span(t, 2.0, 2.7))
    if link > 0:
        dashed(d, (lap[2] + 14, 262), (mix(lap[2] + 14, tab[0] - 14, link), 262), t * 2.0)
    img = over(img, lay)

    # Isi layar menyeberang: satu kartu tipis meluncur dari laptop ke iPad.
    fly = span(t, 2.4, 3.2)
    if fly < 1:
        k = ease(fly)
        img = picture(img, tuple(mix(lap[i], tab[i], k) for i in range(4)), 1.0, framed=True)
    else:
        img = picture(img, tab, 1.0)

    lay = layer()
    d = ImageDraw.Draw(lay)
    first, gap = 3.4, 0.5
    on_tab, on_lap = [], []
    for i, (nx, ny) in enumerate(MARKS):
        tap = first + i * gap
        tx, ty = tab[0] + nx * (tab[2] - tab[0]), tab[1] + ny * (tab[3] - tab[1])
        lx, ly = lap[0] + nx * (lap[2] - lap[0]), lap[1] + ny * (lap[3] - lap[1])
        if t >= tap:
            on_tab.append((tx, ty))
            tick(lay, tx, ty, span(t, tap, tap + 0.5))
        if t >= tap + 0.13:
            on_lap.append((lx, ly))
            tick(lay, lx, ly, span(t, tap + 0.13, tap + 0.6))

    done = first + (len(MARKS) - 1) * gap
    closed = len(on_tab) == len(MARKS) and t > done + 0.3
    for pts in (on_tab, on_lap):
        if len(pts) > 1:
            path = pts + ([pts[0]] if closed and len(pts) == len(MARKS) else [])
            d.line([px(*p) for p in path], fill=tuple(ACCENT) + (255,),
                   width=max(1, int(1.8 * SS)), joint="curve")
        for p in pts:
            circle(d, p[0], p[1], 3.4, PANEL, ACCENT, 1.6)

    # Gerak pena: masuk dari luar bingkai, melayang ke titik berikutnya, menukik
    # saat menandai, lalu diangkat lagi. Satu tahap, satu ketukan.
    enter_at = first - 0.75
    if t >= enter_at:
        n = len(MARKS)
        prog = (t - first) / gap
        i = max(0, min(n - 1, math.floor(prog) + 1))
        if t < first:
            local = span(t, enter_at, first)
            away = (MARKS[0][0] + 0.26, MARKS[0][1] + 0.42)
            p = mixp(away, MARKS[0], ease(local))
            lift = 1 - ease(local) * 0.65
        else:
            local = clamp(prog - (i - 1)) if i > 0 else 1.0
            travel = ease(clamp(local / 0.7))
            press = clamp((local - 0.7) / 0.3)
            p = mixp(MARKS[max(0, i - 1)], MARKS[i], travel)
            lift = max(1 - press, math.sin(math.pi * clamp(local / 0.7)) * 0.85)
            if t > done + 0.25:         # selesai: pena diangkat keluar
                out = ease(span(t, done + 0.25, done + 0.9))
                p = mixp(MARKS[-1], (MARKS[-1][0] + 0.2, MARKS[-1][1] + 0.34), out)
                lift = max(lift, out)
        if t <= done + 0.95:
            pen(lay, tab[0] + p[0] * (tab[2] - tab[0]),
                tab[1] + p[1] * (tab[3] - tab[1]), lift)
    img = over(img, lay)

    img = wordmark(img, t, s["share_sub"])
    img = tag(img, t, 2.1, (233, 398), s["ubuntu"])
    img = tag(img, t, 2.3, (678, 380), s["ipad"])
    img = caption(img, t, 6.0, s["share_foot"])
    return img


SECOND_LEN = 7.2


def scene_second(t, s):
    img = stage(t)

    img, lap = enter(img, t, 0.5, lambda lift: laptop_art(56, 156 + lift, 342, 220))
    img, tab = enter(img, t, 1.25, lambda lift: tablet_art(546, 168 + lift, 280, 196))

    lay = layer()
    d = ImageDraw.Draw(lay)
    y = 268
    link = ease(span(t, 4.6, 5.3))
    if link > 0:
        dashed(d, (lap[2] + 14, y), (mix(lap[2] + 14, tab[0] - 14, link), y), t * 2.0)
        if link > 0.85:
            chip(d, (lap[2] + tab[0]) / 2, y, s["second"], int(255 * span(t, 5.2, 5.6)))

    stay = int(255 * ease(span(t, 1.6, 2.1)))
    if stay > 3:
        window_card(d, (lap[0] + 16, lap[1] + 18, lap[0] + 124, lap[1] + 98), stay)

    move = span(t, 3.2, 4.5)
    k = ease(move)
    src = (lap[0] + 138, lap[1] + 52, lap[0] + 250, lap[1] + 142)
    dst = (tab[0] + 22, tab[1] + 32, tab[0] + 164, tab[1] + 146)
    box = tuple(mix(src[i], dst[i], k) for i in range(4))
    if t > 2.0:
        drag = int(255 * ease(span(t, 2.0, 2.5)))
        window_card(d, box, drag, 4, accent=0.02 < move < 1)
        if move < 1:
            pointer(lay, box[0] + 20, box[1] + 7)
        else:
            pk = ease(span(t, 4.5, 5.2))
            pointer(lay, mix(box[0] + 20, box[0] + 98, pk), mix(box[1] + 7, box[1] + 66, pk))

    img = over(img, lay)
    img = wordmark(img, t, s["second_sub"])
    img = tag(img, t, 1.9, (227, 402), s["ubuntu"])
    img = tag(img, t, 2.1, (686, 382), s["ipad"])
    img = caption(img, t, 5.9, s["second_foot"])
    return img


AIRPLAY_LEN = 6.4


def scene_airplay(t, s):
    img = stage(t)

    img, tab = enter(img, t, 0.5, lambda lift: tablet_art(86, 170 + lift, 252, 188))
    img = picture(img, tab, ease(span(t, 1.15, 1.75)))
    img, lap = enter(img, t, 1.25, lambda lift: laptop_art(470, 150 + lift, 348, 224))

    lay = layer()
    d = ImageDraw.Draw(lay)
    win = (lap[0] + 20, lap[1] + 22, lap[2] - 20, lap[3] - 16)
    win_a = int(255 * ease(span(t, 2.3, 2.9)))
    if win_a > 3:
        window_card(d, win, win_a, lines=0)
    link = ease(span(t, 2.0, 2.8))
    if link > 0:
        dashed(d, (tab[2] + 14, 264), (mix(tab[2] + 14, win[0] - 14, link), 264), t * 2.0)
    img = over(img, lay)

    inner = (win[0] + 9, win[1] + 22, win[2] - 9, win[3] - 9)
    fly = span(t, 2.8, 3.7)
    if fly > 0:
        box = tuple(mix(tab[i], inner[i], ease(fly)) for i in range(4))
        img = picture(img, box, 1.0, framed=fly < 1)

    img = wordmark(img, t, s["airplay_sub"])
    img = tag(img, t, 1.7, (212, 386), s["ipad"])
    img = tag(img, t, 2.3, (644, 400), s["ubuntu"])
    img = caption(img, t, 4.8, s["airplay_foot"])
    return img


# ---------- render ----------

def render(scene, length, lang, tmp):
    s = TEXTS[lang]
    frames = int(length * FPS)
    for i in range(frames):
        acc = None
        for j in range(SUB):
            t = (i + j / SUB) / FPS
            frame = scene(t, s).convert("RGB")
            acc = frame if acc is None else Image.blend(acc, frame, 1 / (j + 1))
        acc.resize((OUT_W, OUT_H), Image.LANCZOS).save(tmp / f"f{i:04d}.png")
    return frames


def to_gif(tmp, out):
    """Palet dihitung dari isinya sendiri supaya garis tipis tidak pecah."""
    pal = tmp / "palette.png"
    src = ["-framerate", str(FPS), "-i", str(tmp / "f%04d.png")]
    subprocess.run(["ffmpeg", "-y", "-v", "error", *src, "-vf",
                    "palettegen=max_colors=96:stats_mode=diff", str(pal)], check=True)
    subprocess.run(["ffmpeg", "-y", "-v", "error", *src, "-i", str(pal), "-lavfi",
                    "paletteuse=dither=none:diff_mode=rectangle", "-loop", "0",
                    str(out)], check=True)


def build(name, scene, length, lang, outdir):
    out = outdir / (f"demo-{name}.gif" if lang == "en" else f"demo-{name}-{lang}.gif")
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        render(scene, length, lang, tmp)
        to_gif(tmp, out)
    print(f"{out.name}: {out.stat().st_size / 1024:.0f} KB")


def main():
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg belum terpasang: sudo apt install ffmpeg")
    outdir = pathlib.Path("docs/media")
    outdir.mkdir(parents=True, exist_ok=True)
    jobs = [("share", scene_share, SHARE_LEN),
            ("second", scene_second, SECOND_LEN),
            ("airplay", scene_airplay, AIRPLAY_LEN)]
    only = sys.argv[1:] or None
    for name, scene, length in jobs:
        if only and name not in only:
            continue
        for lang in ("en", "id"):
            build(name, scene, length, lang, outdir)


if __name__ == "__main__":
    main()
