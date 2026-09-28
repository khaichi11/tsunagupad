#!/usr/bin/env bash
# Tsunagu-Pad, bagian pemasangan yang butuh root: paket, izin Apple Pencil, hotspot,
# polkit, dan firewall. Dipanggil install.sh lewat sudo:
#   sudo system/install-system.sh NAMA_PENGGUNA
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$(readlink -f "$0")")/.."

say()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }

[[ $EUID -eq 0 ]] || { echo "Jalankan lewat sudo: sudo $0 NAMA_PENGGUNA" >&2; exit 1; }
user=${1:?Pemakaian: install-system.sh NAMA_PENGGUNA}
uid=$(id -u "$user")
group=$(id -gn "$user")

packages=(
    build-essential pkg-config
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsoup-3.0-dev libjson-glib-dev
    libqrencode-dev libx11-dev libxtst-dev
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
    gstreamer1.0-plugins-ugly gstreamer1.0-nice gstreamer1.0-vaapi gstreamer1.0-libav gstreamer1.0-x
    libgstreamer-plugins-bad1.0-0 uxplay avahi-daemon network-manager
    # dnsmasq-base (bukan dnsmasq): tanpa layanan DNS sistem yang bentrok dengan systemd-resolved.
    hostapd dnsmasq-base iw iptables x11-xserver-utils xserver-xorg-core
)
missing=()
for p in "${packages[@]}"; do
    dpkg-query -W -f='${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || missing+=("$p")
done
if ((${#missing[@]})); then
    say "Memasang paket: ${missing[*]}"
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
else
    say "Semua paket yang dibutuhkan sudah terpasang"
fi

say "Mengizinkan tablet virtual untuk tekanan Apple Pencil (/dev/uinput)"
echo 'KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"' \
    >/etc/udev/rules.d/60-tsunagupad-uinput.rules
echo uinput >/etc/modules-load.d/tsunagupad-uinput.conf
modprobe uinput || true
udevadm control --reload-rules || true
if [[ -e /dev/uinput ]]; then
    chgrp input /dev/uinput && chmod 0660 /dev/uinput
fi
if ! id -nG "$user" | tr ' ' '\n' | grep -qx input; then
    usermod -aG input "$user"
    warn "$user baru ditambahkan ke grup 'input'. Log out/login agar tekanan Apple Pencil aktif."
fi

say "Menyiapkan hotspot Tsunagu-Pad"
hotspot_was_active=0
if systemctl is-active --quiet tsunagupad-hotspot.service || systemctl is-active --quiet ichipad-hotspot.service; then
    hotspot_was_active=1
fi
if systemctl is-active --quiet tsunagupad-hotspot.service; then
    systemctl stop tsunagupad-hotspot.service
fi
# Komponen versi lama yang sudah digantikan. Layar kedua (vkms dan monitor
# virtual NVIDIA) dihapus karena membuat layar hitam pada laptop GPU hibrida.
systemctl disable --now tsunagupad-ap0.service 2>/dev/null || true
rm -f /etc/systemd/system/tsunagupad-ap0.service /usr/local/lib/tsunagupad/ap0-interface \
    /etc/polkit-1/rules.d/60-tsunagupad-hotspot.rules /etc/systemd/system/tsunagupad-vkms.service \
    /usr/local/lib/tsunagupad/virtual-monitor /etc/tsunagupad/virtual-monitor.edid
if [[ -e /etc/X11/xorg.conf.d/20-tsunagupad-virtual-monitor.conf ]]; then
    rm -f /etc/X11/xorg.conf.d/20-tsunagupad-virtual-monitor.conf
    warn "Konfigurasi monitor virtual lama dicabut. Logout lalu login agar tampilan kembali normal."
fi

# Hentikan dulu unit dari versi IchiPad sebelum sisa-sisanya dibersihkan.
systemctl disable --now ichipad-hotspot.service ichipad-ap0.service ichipad-vkms.service 2>/dev/null || true

# Pindahkan konfigurasi hotspot dari versi IchiPad supaya nama Wi-Fi dan kata
# sandi yang sudah dipakai iPad tetap sama.
if [[ -f /etc/ichipad/hotspot.conf && ! -f /etc/tsunagupad/hotspot.conf ]]; then
    install -d -m 755 /etc/tsunagupad
    install -m 640 -o root -g "$group" /etc/ichipad/hotspot.conf /etc/tsunagupad/hotspot.conf
    say "Konfigurasi hotspot dipindahkan dari versi lama"
fi

# Sisa-sisa versi lama (IchiPad) yang sudah diganti nama menjadi Tsunagu-Pad.
rm -f /etc/X11/xorg.conf.d/20-ichipad-virtual-monitor.conf /etc/ichipad/virtual-monitor.edid \
    /usr/local/lib/ichipad/virtual-monitor
rm -f /etc/systemd/system/ichipad-*.service /etc/polkit-1/rules.d/60-ichipad*.rules \
    /etc/NetworkManager/conf.d/90-ichipad-unmanaged.conf /etc/udev/rules.d/60-ichipad-uinput.rules \
    /etc/modules-load.d/ichipad-uinput.conf
rm -rf /etc/ichipad /usr/local/lib/ichipad

# hostapd/dnsmasq dari versi lama bisa tertinggal hidup dan memegang radio
# Wi-Fi, sehingga hotspot baru gagal dengan "Could not configure driver mode".
pkill -f '/run/ichipad-hotspot/hostapd.conf' 2>/dev/null || true
pkill -f '/run/ichipad-hotspot/dnsmasq.conf' 2>/dev/null || true
rm -rf /run/ichipad-hotspot
iw dev ap0 del 2>/dev/null || true

install -Dm755 system/tsunagupad-hotspot /usr/local/lib/tsunagupad/hotspot
install -Dm755 system/tsunagupad-hotspot-config /usr/local/lib/tsunagupad/hotspot-config
install -Dm644 system/tsunagupad-hotspot.service /etc/systemd/system/tsunagupad-hotspot.service
install -Dm644 system/60-tsunagupad.rules /etc/polkit-1/rules.d/60-tsunagupad.rules
install -Dm644 system/90-tsunagupad-unmanaged.conf /etc/NetworkManager/conf.d/90-tsunagupad-unmanaged.conf
systemctl daemon-reload
systemctl try-restart polkit.service || true
nmcli general reload conf 2>/dev/null || true

# Konfigurasi hotspot: jangan pernah memakai kata sandi bawaan yang tercantum publik.
conf=/etc/tsunagupad/hotspot.conf
if [[ ! -f $conf ]] || grep -qx 'PASSWORD=IchiPad12345' "$conf"; then
    ssid=$(sed -n 's/^SSID=//p' "$conf" 2>/dev/null | head -n1)
    [[ -n $ssid ]] || ssid="TsunaguPad-$(hostname | tr -dc 'A-Za-z0-9-' | cut -c1-16)"
    password=$(od -An -N9 -tx1 /dev/urandom | tr -d ' \n')
    # Hotspot sedang mati di titik ini, jadi helper tidak ikut me-restart-nya.
    printf '%s\n' "$password" | PKEXEC_UID=$uid /usr/local/lib/tsunagupad/hotspot-config "$ssid"
    say "Hotspot: $ssid, kata sandi baru: $password (bisa diubah dari panel)"
else
    # Berkas versi lama bisa dibaca semua pengguna; batasi ke pemilik.
    chown "root:$group" "$conf"
    chmod 640 "$conf"
fi
if [[ $hotspot_was_active -eq 1 ]]; then
    systemctl start tsunagupad-hotspot.service || warn "Hotspot gagal menyala ulang: journalctl -u tsunagupad-hotspot"
fi

if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    say "Membuka port di firewall (UFW)"
    ufw allow 8765/tcp comment 'Tsunagu-Pad halaman iPad'
    ufw allow 50000:50100/udp comment 'Tsunagu-Pad video WebRTC'
    ufw allow 7000,7001,7100/tcp comment 'Tsunagu-Pad AirPlay'
    ufw allow 6000,6001,7011/udp comment 'Tsunagu-Pad AirPlay'
    ufw allow 5353/udp comment 'Tsunagu-Pad mDNS (AirPlay)'
fi

say "Komponen sistem Tsunagu-Pad terpasang"
