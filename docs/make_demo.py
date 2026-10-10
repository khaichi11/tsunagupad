#!/usr/bin/env python3
"""Membuat motion graphic dokumentasi Tsunagu-Pad.

Semua gambar digambar dari nol, bukan rekaman layar, jadi tidak ada isi layar
pribadi yang ikut terbawa ke repository. Jalankan dari akar proyek:

    python3 docs/make_demo.py

Hasil di docs/media: demo-share.gif, demo-share-id.gif, demo-airplay.gif,
demo-airplay-id.gif

Butuh python3-pil dan ffmpeg. Judul memakai Poppins dan teks memakai Inter bila
tersedia; keduanya berlisensi SIL Open Font License dan tidak disalin ke
repository ini. Ambil sekali dengan:

    mkdir -p ~/.cache/tsunagupad-fonts && cd ~/.cache/tsunagupad-fonts
    curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-SemiBold.ttf
    curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-Medium.ttf
    sudo apt install fonts-inter

SPDX-License-Identifier: MIT
"""

import math
import pathlib
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 880, 495
FPS = 20

INK = (9, 12, 18)
INK_SOFT = (16, 21, 30)
PANEL = (23, 30, 41)
BEZEL = (13, 17, 24)
EDGE = (44, 55, 71)
TEXT = (232, 237, 244)
MUTED = (143, 158, 177)
ACCENT = (110, 231, 200)
MARK = (240, 118, 118)

FONT_DIRS = [pathlib.Path.home() / ".cache/tsunagupad-fonts",
             pathlib.Path("/usr/share/fonts/truetype")]
FALLBACK = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def find_font(*names):
    for base in FONT_DIRS:
        for name in names:
            for path in base.rglob(name):
                return str(path)
    return FALLBACK


DISPLAY = find_font("Poppins-SemiBold.ttf")
DISPLAY_MED = find_font("Poppins-Medium.ttf", "Poppins-SemiBold.ttf")
BODY = find_font("InterDisplay-Regular.ttf", "Inter-Regular.ttf", "Inter[opsz,wght].ttf")
BODY_MED = find_font("InterDisplay-Medium.ttf", "Inter-Medium.ttf", "Inter[opsz,wght].ttf")

_font_cache = {}


def font(path, size):
    key = (path, size)
    if key not in _font_cache:
        try:
            _font_cache[key] = ImageFont.truetype(path, size)
        except OSError:
            _font_cache[key] = ImageFont.load_default()
    return _font_cache[key]


def text(d, s, xy, size=15, color=TEXT, path=None, center=False):
    f = font(path or BODY, size)
    x, y = xy
    if center:
        x -= d.textlength(s, font=f) / 2
    d.text((x, y), s, font=f, fill=color)


def ease(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def backdrop():
    """Latar gelap dengan pendar lembut dan garis kisi tipis."""
    img = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(img)
    for y in range(H):
        k = y / H
        d.line([(0, y), (W, y)], fill=(
            int(INK[0] + (INK_SOFT[0] - INK[0]) * k),
            int(INK[1] + (INK_SOFT[1] - INK[1]) * k),
            int(INK[2] + (INK_SOFT[2] - INK[2]) * k)))
    grid = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(grid)
    for x in range(0, W, 44):
        g.line([(x, 0), (x, H)], fill=(255, 255, 255, 7))
    for y in range(0, H, 44):
        g.line([(0, y), (W, y)], fill=(255, 255, 255, 7))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([W * 0.26, -170, W * 0.78, 250], fill=ACCENT + (26,))
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    return Image.alpha_composite(Image.alpha_composite(img.convert("RGBA"), glow), grid)


def shadow(img, box, radius, blur=22, alpha=130, drop=14):
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    x0, y0, x1, y1 = box
    ImageDraw.Draw(layer).rounded_rectangle([x0, y0 + drop, x1, y1 + drop], radius,
                                            fill=(0, 0, 0, alpha))
    return Image.alpha_composite(img, layer.filter(ImageFilter.GaussianBlur(blur)))


def gradient_screen(d, box, top, bottom):
    x0, y0, x1, y1 = box
    span = max(1, int(y1 - y0))
    for i in range(span):
        k = i / span
        d.line([(x0, y0 + i), (x1, y0 + i)], fill=(
            int(top[0] + (bottom[0] - top[0]) * k),
            int(top[1] + (bottom[1] - top[1]) * k),
            int(top[2] + (bottom[2] - top[2]) * k)))


def laptop(img, x, y, w, h):
    img = shadow(img, (x, y, x + w, y + h), 16)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w, y + h], 16, fill=BEZEL, outline=EDGE, width=2)
    screen = (x + 12, y + 12, x + w - 12, y + h - 18)
    gradient_screen(d, screen, (26, 34, 46), (17, 23, 32))
    d.rounded_rectangle([x - 26, y + h + 6, x + w + 26, y + h + 18], 6,
                        fill=PANEL, outline=EDGE, width=2)
    d.line([x + w / 2 - 26, y + h + 12, x + w / 2 + 26, y + h + 12], fill=EDGE, width=3)
    return img, screen


def tablet(img, x, y, w, h):
    img = shadow(img, (x, y, x + w, y + h), 22)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w, y + h], 22, fill=BEZEL, outline=EDGE, width=2)
    screen = (x + 11, y + 11, x + w - 11, y + h - 11)
    gradient_screen(d, screen, (26, 34, 46), (17, 23, 32))
    d.rounded_rectangle([x + w / 2 - 26, y + h - 7, x + w / 2 + 26, y + h - 5], 2, fill=EDGE)
    return img, screen


def stairs(d, box):
    """Contoh foto tangga, bahan yang dilabeli."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    d.rectangle(box, fill=(31, 40, 52))
    d.polygon([(x0, y0 + h * 0.18), (x1, y0 + h * 0.02), (x1, y0), (x0, y0)], fill=(38, 49, 63))
    steps = 5
    for i in range(steps):
        k = i / steps
        sw = w * (0.78 - k * 0.1)
        sh = h * 0.58 / steps
        top = y1 - h * 0.14 - (i + 1) * sh
        left = x0 + w * 0.11 + k * w * 0.03
        d.rectangle([left, top, left + sw, top + sh * 0.88], fill=(56 + i * 5, 68 + i * 5, 83 + i * 5))
        d.line([left, top, left + sw, top], fill=(96, 112, 131), width=1)


def polygon_layer(box, points, count, pop):
    """Poligon label dengan pendar lembut."""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = box
    pts = [(x0 + px * (x1 - x0), y0 + py * (y1 - y0)) for px, py in points[:count]]
    if len(pts) > 2:
        d.polygon(pts, fill=MARK + (38,))
    if len(pts) > 1:
        d.line(pts + ([pts[0]] if count == len(points) else []), fill=MARK + (235,), width=3,
               joint="curve")
    for i, p in enumerate(pts):
        r = 5 if i < count - 1 else 5 + 4 * (1 - ease(pop))
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=(252, 236, 236, 255),
                  outline=MARK + (255,), width=2)
    return layer


def pen_cursor(layer, x, y, pressed):
    d = ImageDraw.Draw(layer)
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([x - 26, y - 26, x + 26, y + 26], fill=ACCENT + (70 if pressed else 40,))
    layer.alpha_composite(glow.filter(ImageFilter.GaussianBlur(12)))
    r = 11 if pressed else 13
    d.ellipse([x - r, y - r, x + r, y + r], outline=ACCENT + (255,), width=2)
    d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=ACCENT + (255,))


def stream(layer, a, b, phase, color, label, lift=70):
    """Paket data mengalir di jalur melengkung dari a ke b."""
    d = ImageDraw.Draw(layer)
    (ax, ay), (bx, by) = a, b
    cx, cy = (ax + bx) / 2, min(ay, by) - lift

    def point(t):
        u = 1 - t
        return (u * u * ax + 2 * u * t * cx + t * t * bx,
                u * u * ay + 2 * u * t * cy + t * t * by)

    guide = [point(i / 40) for i in range(41)]
    d.line(guide, fill=color + (40,), width=2)
    for i in range(5):
        t = ((i / 5) + phase) % 1.0
        px, py = point(t)
        fade = int(255 * (0.35 + 0.65 * math.sin(math.pi * t)))
        d.ellipse([px - 4, py - 4, px + 4, py + 4], fill=color + (fade,))
    mx, my = point(0.5)
    f = font(BODY_MED, 14)
    tw = d.textlength(label, font=f)
    d.rounded_rectangle([mx - tw / 2 - 10, my - 14, mx + tw / 2 + 10, my + 12], 13,
                        fill=(16, 22, 31, 225), outline=color + (120,), width=1)
    d.text((mx - tw / 2, my - 10), label, font=f, fill=color + (255,))


TEXTS = {
    "en": {
        "share_sub": "Your Ubuntu screen on the iPad, controlled from the iPad",
        "video": "video", "input": "input",
        "chips": ["touch", "Apple Pencil", "mouse", "keyboard"],
        "share_foot": "One picture, two screens. Point on the iPad, Ubuntu answers right away.",
        "airplay_sub": "The iPad screen shown on Ubuntu through AirPlay",
        "airplay": "AirPlay",
        "airplay_foot": "Control Center, then Screen Mirroring, then pick Tsunagu-Pad Ubuntu",
    },
    "id": {
        "share_sub": "Layar Ubuntu kamu di iPad, dikendalikan dari iPad",
        "video": "video", "input": "kendali",
        "chips": ["sentuh", "Apple Pencil", "mouse", "keyboard"],
        "share_foot": "Satu gambar, dua layar. Disentuh di iPad, Ubuntu langsung menjawab.",
        "airplay_sub": "Layar iPad tampil di Ubuntu lewat AirPlay",
        "airplay": "AirPlay",
        "airplay_foot": "Pusat Kontrol, lalu Pencerminan Layar, lalu pilih Tsunagu-Pad Ubuntu",
    },
}


def chips(img, labels, active, y):
    """Deretan label jenis masukan; satu menyala bergantian."""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    f = font(BODY_MED, 13)
    widths = [d.textlength(s, font=f) + 26 for s in labels]
    total = sum(widths) + 10 * (len(labels) - 1)
    x = (W - total) / 2
    for i, label in enumerate(labels):
        on = i == active
        d.rounded_rectangle([x, y, x + widths[i], y + 28], 14,
                            fill=(ACCENT + (38,)) if on else (255, 255, 255, 10),
                            outline=(ACCENT + (190,)) if on else (255, 255, 255, 28), width=1)
        d.text((x + 13, y + 6), label, font=f, fill=(ACCENT if on else MUTED) + (255,))
        x += widths[i] + 10
    return Image.alpha_composite(img, layer)

PATH = [(0.24, 0.72), (0.34, 0.34), (0.60, 0.29), (0.74, 0.64), (0.49, 0.83)]


def header(img, subtitle):
    d = ImageDraw.Draw(img)
    text(d, "Tsunagu-Pad", (56, 36), 31, TEXT, DISPLAY)
    text(d, subtitle, (58, 78), 15, MUTED, BODY)
    d.line([(58, 106), (58 + 120, 106)], fill=ACCENT + (255,), width=3)
    return img


def footer(img, note):
    d = ImageDraw.Draw(img)
    text(d, note, (W / 2, H - 46), 14, MUTED, BODY, center=True)
    return img


def frame_share(i, total, lang="en"):
    s = TEXTS[lang]
    img = header(backdrop(), s["share_sub"])

    intro = ease(i / (total * 0.18))
    img, lap = laptop(img, 60, 150, 348, 226)
    img, tab = tablet(img, 520, 152, 300, 214)
    d = ImageDraw.Draw(img)
    stairs(d, lap)
    stairs(d, tab)

    hold = total * 0.16
    span = max(1.0, total - hold)
    walk = max(0.0, (i - hold)) / span * (len(PATH) - 0.001)
    count = min(len(PATH), int(walk) + 1)
    seg = walk - int(walk)
    a = PATH[max(0, int(walk) - 1)] if int(walk) else PATH[0]
    b = PATH[min(len(PATH) - 1, int(walk))]
    k = ease(seg)
    cur = (a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k)

    img = Image.alpha_composite(img, polygon_layer(lap, PATH, count, seg))
    img = Image.alpha_composite(img, polygon_layer(tab, PATH, count, seg))

    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = tab[0] + cur[0] * (tab[2] - tab[0])
    py = tab[1] + cur[1] * (tab[3] - tab[1])
    pen_cursor(layer, px, py, seg > 0.82 or seg < 0.12)
    phase = (i / total) * 2.2 % 1.0
    stream(layer, (412, 230), (512, 230), phase, ACCENT, s["video"], 58)
    stream(layer, (512, 320), (412, 320), phase, MARK, s["input"], -52)
    img = Image.alpha_composite(img, layer)

    d = ImageDraw.Draw(img)
    text(d, "Ubuntu", (234, 404), 14, MUTED, BODY_MED, center=True)
    text(d, "iPad", (670, 382), 14, MUTED, BODY_MED, center=True)
    img = chips(img, s["chips"], int(i / total * len(s["chips"])) % len(s["chips"]), 112)
    if intro < 1:
        veil = Image.new("RGBA", (W, H), (9, 12, 18, int(255 * (1 - intro))))
        img = Image.alpha_composite(img, veil)
    return footer(img, s["share_foot"]).convert("RGB")


def card(d, box, grow):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    d.rectangle(box, fill=(24, 31, 42))
    d.rounded_rectangle([x0 + w * 0.08, y0 + h * 0.12, x1 - w * 0.08, y0 + h * 0.34], 10,
                        fill=(33, 43, 57))
    d.rounded_rectangle([x0 + w * 0.08, y0 + h * 0.12, x0 + w * 0.08 + w * 0.3, y0 + h * 0.34], 10,
                        fill=ACCENT)
    base = y1 - h * 0.16
    bars = 6
    bw = w * 0.085
    gap = w * 0.045
    start = x0 + w * 0.12
    for b in range(bars):
        hgt = h * (0.1 + 0.26 * (0.5 + 0.5 * math.sin((b + grow) * 0.9)))
        bx = start + b * (bw + gap)
        d.rounded_rectangle([bx, base - hgt, bx + bw, base], 4,
                            fill=ACCENT if b % 2 else MARK)
    d.line([x0 + w * 0.08, base + 7, x1 - w * 0.08, base + 7], fill=(44, 55, 71), width=2)


def frame_airplay(i, total, lang="en"):
    s = TEXTS[lang]
    img = header(backdrop(), s["airplay_sub"])
    img, tab = tablet(img, 70, 152, 300, 214)
    img, lap = laptop(img, 470, 150, 348, 226)
    d = ImageDraw.Draw(img)
    grow = i * 0.22
    card(d, tab, grow)
    card(d, lap, grow)

    # Garis pindai tipis di layar Ubuntu: tanda gambar baru saja tiba.
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sweep = (i / total * 1.6) % 1.3
    if sweep <= 1:
        sy = lap[1] + (lap[3] - lap[1]) * sweep
        ImageDraw.Draw(layer).rectangle([lap[0], sy - 10, lap[2], sy + 10], fill=ACCENT + (34,))
    phase = (i / total) * 2.2 % 1.0
    stream(layer, (374, 250), (466, 250), phase, ACCENT, s["airplay"], 62)
    img = Image.alpha_composite(img, layer.filter(ImageFilter.GaussianBlur(0.4)))

    d = ImageDraw.Draw(img)
    text(d, "iPad", (220, 382), 14, MUTED, BODY_MED, center=True)
    text(d, "Ubuntu", (644, 404), 14, MUTED, BODY_MED, center=True)
    return footer(img, s["airplay_foot"]).convert("RGB")


def render(name, maker, frames, lang):
    out = pathlib.Path("docs/media") / name
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for i in range(frames):
            maker(i, frames, lang).save(tmp / f"f{i:04d}.png")
        palette = tmp / "palette.png"
        base = ["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS),
                "-i", str(tmp / "f%04d.png")]
        subprocess.run(base + ["-vf", "palettegen=max_colors=96:stats_mode=diff", str(palette)],
                       check=True)
        subprocess.run(base + ["-i", str(palette),
                               "-lavfi", "paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle",
                               "-loop", "0", str(out)], check=True)
    print(f"{out}  {out.stat().st_size // 1024} KB")


if __name__ == "__main__":
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg tidak ditemukan")
    render("demo-share.gif", frame_share, 96, "en")
    render("demo-share-id.gif", frame_share, 96, "id")
    render("demo-airplay.gif", frame_airplay, 56, "en")
    render("demo-airplay-id.gif", frame_airplay, 56, "id")
