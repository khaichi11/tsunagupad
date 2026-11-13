#!/usr/bin/env bash
# Tsunagu-Pad, hapus program, extension, dan pengaturan sistem yang dibuat install.sh.
# Paket apt (GStreamer, UxPlay, dll.) sengaja tidak dihapus karena bisa dipakai aplikasi lain.
set -euo pipefail
cd "$(dirname "$0")"

gnome-extensions disable tsunagu-pad@local 2>/dev/null || true
sudo system/tsunagupad-login-setup disable || true
make uninstall

sudo systemctl stop tsunagupad-hotspot.service 2>/dev/null || true
sudo systemctl disable --now tsunagupad-ap0.service 2>/dev/null || true
sudo rm -f /etc/systemd/system/tsunagupad-hotspot.service /etc/systemd/system/tsunagupad-vkms.service \
    /etc/systemd/system/tsunagupad-ap0.service
sudo rm -f /etc/polkit-1/rules.d/60-tsunagupad.rules /etc/polkit-1/rules.d/60-tsunagupad-hotspot.rules
sudo rm -f /etc/NetworkManager/conf.d/90-tsunagupad-unmanaged.conf
sudo rm -f /etc/udev/rules.d/60-tsunagupad-uinput.rules /etc/modules-load.d/tsunagupad-uinput.conf
sudo rm -f /etc/X11/xorg.conf.d/20-tsunagupad-virtual-monitor.conf
sudo rm -rf /usr/local/lib/tsunagupad /etc/tsunagupad
sudo systemctl daemon-reload || true
sudo udevadm control --reload-rules || true
sudo nmcli general reload conf 2>/dev/null || true

if command -v ufw >/dev/null && sudo ufw status | grep -q "Status: active"; then
    # Aturan versi lama (terbuka untuk semua alamat) dan versi baru (hanya
    # jaringan lokal dan Tailscale) sama-sama dihapus.
    sudo ufw delete allow 8765/tcp >/dev/null 2>&1 || true
    sudo ufw delete allow 50000:50100/udp >/dev/null 2>&1 || true
    sudo ufw delete allow 7000,7001,7100/tcp >/dev/null 2>&1 || true
    sudo ufw delete allow 6000,6001,7011/udp >/dev/null 2>&1 || true
    sudo ufw delete allow 5353/udp >/dev/null 2>&1 || true
    for net in 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 169.254.0.0/16 100.64.0.0/10 fe80::/10 fc00::/7; do
        sudo ufw delete allow from "$net" to any port 8765 proto tcp >/dev/null 2>&1 || true
        sudo ufw delete allow from "$net" to any port 50000:50100 proto udp >/dev/null 2>&1 || true
        sudo ufw delete allow from "$net" to any port 7000,7001,7100 proto tcp >/dev/null 2>&1 || true
        sudo ufw delete allow from "$net" to any port 6000,6001,7011 proto udp >/dev/null 2>&1 || true
        sudo ufw delete allow from "$net" to any port 5353 proto udp >/dev/null 2>&1 || true
    done
fi

rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/tsunagupad"

# Sisa-sisa versi lama (IchiPad) yang sudah diganti nama menjadi Tsunagu-Pad.
sudo rm -f /etc/X11/xorg.conf.d/20-ichipad-virtual-monitor.conf /etc/ichipad/virtual-monitor.edid \
    /usr/local/lib/ichipad/virtual-monitor
sudo rm -f /etc/systemd/system/ichipad-*.service /etc/polkit-1/rules.d/60-ichipad*.rules \
    /etc/NetworkManager/conf.d/90-ichipad-unmanaged.conf /etc/udev/rules.d/60-ichipad-uinput.rules \
    /etc/modules-load.d/ichipad-uinput.conf
sudo rm -rf /etc/ichipad /usr/local/lib/ichipad
rm -f "$HOME/.local/bin/ichipad"
rm -rf "$HOME/.local/share/ichipad" "$HOME/.local/share/gnome-shell/extensions/ichipad@ichipad.github.io" \
    "$HOME/.config/ichipad"

echo "Tsunagu-Pad sudah dihapus. Modul vkms (bila sempat dimuat) hilang setelah reboot."
