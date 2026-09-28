#!/usr/bin/env bash
# Tsunagu-Pad, pemasang untuk Ubuntu 24.04+ (GNOME).
# Bagian root (paket, hotspot, izin) ada di system/install-system.sh
# dan hanya meminta sudo sekali; sisanya dipasang ke ~/.local tanpa root.
set -euo pipefail
cd "$(dirname "$0")"

say()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }

if [[ $EUID -eq 0 ]]; then
    echo "Jalankan tanpa sudo: ./install.sh (sudo akan diminta saat perlu)." >&2
    exit 1
fi

say "Komponen sistem (perlu sudo)"
sudo "$PWD/system/install-system.sh" "$USER"

say "Build Tsunagu-Pad"
make clean >/dev/null
make

say "Memasang ke ~/.local"
make install

say "Cek encoder video"
"$HOME/.local/bin/tsunagupad" probe || warn "Encoder H.264 tidak ditemukan, cek paket GStreamer."

say "Mengaktifkan tombol Tsunagu-Pad di Quick Settings"
gnome-extensions enable tsunagu-pad@local 2>/dev/null || true
if [[ "${XDG_SESSION_TYPE:-}" == "x11" ]]; then
    warn "Muat ulang GNOME Shell agar panel versi baru terpakai: Alt+F2, ketik r, lalu Enter."
else
    warn "Log out lalu login kembali agar panel versi baru terpakai."
    warn "Mode Ubuntu ke iPad saat ini butuh sesi \"Ubuntu on Xorg\" (pilih di layar login)."
fi

say "Selesai! Buka Quick Settings (pojok kanan atas) dan ketuk tombol Tsunagu-Pad."
