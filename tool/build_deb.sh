#!/usr/bin/env bash
# Membuat paket .deb supaya pemasangan cukup satu perintah atau satu klik, dan
# pustaka yang belum ada ditarik sendiri oleh apt dengan tampilan unduhannya.
#
#   tool/build_deb.sh            # hasilnya dist/tsunagupad_<versi>_<arsitektur>.deb
#
# Memasangnya:
#
#   sudo apt install ./dist/tsunagupad_*.deb
#
# atau klik dua kali berkasnya; App Center menampilkan kemajuan unduhannya.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."

version=$(sed -n 's/^VERSION *:= *//p' Makefile | head -n1)
arch=$(dpkg --print-architecture)
pkg=tsunagupad
root=dist/${pkg}_${version}_${arch}
deb=dist/${pkg}_${version}_${arch}.deb

say() { printf '\033[1;36m==\033[0m %s\n' "$*"; }

say "Membangun program versi $version untuk $arch"
make clean >/dev/null
make PREFIX=/usr

rm -rf "$root"
install -d "$root/DEBIAN"

say "Menyusun isi paket"
install -Dm755 build/tsunagupad "$root/usr/bin/tsunagupad"
install -d "$root/usr/share/tsunagupad/web"
install -m644 web/* "$root/usr/share/tsunagupad/web/"
install -d "$root/usr/share/gnome-shell/extensions/tsunagu-pad@local"
cp -r extension/tsunagu-pad@local/. "$root/usr/share/gnome-shell/extensions/tsunagu-pad@local/"

# Pembantu hotspot. Di dalam paket letaknya /usr/lib, bukan /usr/local/lib yang
# memang bukan tempat berkas milik paket, jadi jalurnya ikut disesuaikan.
install -Dm755 system/tsunagupad-hotspot "$root/usr/lib/tsunagupad/hotspot"
install -Dm755 system/tsunagupad-hotspot-config "$root/usr/lib/tsunagupad/hotspot-config"
install -Dm644 system/tsunagupad-hotspot.service "$root/lib/systemd/system/tsunagupad-hotspot.service"
install -Dm644 system/60-tsunagupad.rules "$root/etc/polkit-1/rules.d/60-tsunagupad.rules"
install -Dm644 system/90-tsunagupad-unmanaged.conf \
    "$root/etc/NetworkManager/conf.d/90-tsunagupad-unmanaged.conf"
sed -i 's#/usr/local/lib/tsunagupad#/usr/lib/tsunagupad#g' \
    "$root/lib/systemd/system/tsunagupad-hotspot.service" \
    "$root/etc/polkit-1/rules.d/60-tsunagupad.rules" \
    "$root/usr/lib/tsunagupad/hotspot" \
    "$root/usr/lib/tsunagupad/hotspot-config"

# Tablet virtual untuk Apple Pencil butuh /dev/uinput yang bisa ditulis.
install -d "$root/lib/udev/rules.d"
cat > "$root/lib/udev/rules.d/60-tsunagupad-uinput.rules" <<'RULES'
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
RULES
chmod 644 "$root/lib/udev/rules.d/60-tsunagupad-uinput.rules"

install -Dm644 README.md "$root/usr/share/doc/$pkg/README.md"
install -Dm644 README.id.md "$root/usr/share/doc/$pkg/README.id.md"
install -Dm644 THIRD_PARTY_NOTICES.md "$root/usr/share/doc/$pkg/THIRD_PARTY_NOTICES.md"
install -Dm644 LICENSE "$root/usr/share/doc/$pkg/copyright"

size=$(du -ks "$root" | cut -f1)

cat > "$root/DEBIAN/control" <<CONTROL
Package: $pkg
Version: $version
Architecture: $arch
Maintainer: khaichi11 <khaichi11@users.noreply.github.com>
Installed-Size: $size
Depends: libc6, libglib2.0-0t64 | libglib2.0-0, libgstreamer1.0-0,
 libgstreamer-plugins-base1.0-0, libgstreamer-plugins-bad1.0-0,
 libsoup-3.0-0, libjson-glib-1.0-0, libqrencode4, libx11-6, libxtst6,
 libpipewire-0.3-0t64 | libpipewire-0.3-0,
 gstreamer1.0-plugins-base, gstreamer1.0-plugins-good,
 gstreamer1.0-plugins-bad, gstreamer1.0-nice, gstreamer1.0-pipewire,
 pipewire, xdg-desktop-portal, xdg-desktop-portal-gnome
Recommends: gstreamer1.0-vaapi, gstreamer1.0-plugins-ugly, gstreamer1.0-libav,
 network-manager, iw, hostapd, dnsmasq, uxplay, avahi-daemon
Section: utils
Priority: optional
Homepage: https://github.com/khaichi11/tsunagupad
Description: Use an iPad as a second screen for Ubuntu over Wi-Fi
 Tsunagu-Pad gives a GNOME Wayland session a monitor that no cable is attached
 to: windows can be dragged onto it, the pointer crosses the edge, and the
 laptop's own cursor is drawn into the picture. The iPad installs nothing and
 opens the screen in Safari.
 .
 Touch and Apple Pencil travel back as a virtual tablet with pressure and tilt,
 so applications cannot tell them from a real one. The screen is captured
 through PipeWire, encoded as H.264 on the GPU, and sent as a WebRTC stream.
CONTROL

cat > "$root/DEBIAN/conffiles" <<'CONF'
/etc/polkit-1/rules.d/60-tsunagupad.rules
/etc/NetworkManager/conf.d/90-tsunagupad-unmanaged.conf
CONF

cat > "$root/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger --subsystem-match=misc --action=change 2>/dev/null || true
    [ -e /dev/uinput ] && { chgrp input /dev/uinput 2>/dev/null || true; \
        chmod 0660 /dev/uinput 2>/dev/null || true; }
    systemctl daemon-reload 2>/dev/null || true
    systemctl try-restart polkit.service 2>/dev/null || true
    nmcli general reload conf 2>/dev/null || true

    # Kata sandi hotspot dibuat acak saat pemasangan pertama; tidak pernah ada
    # kata sandi bawaan yang sama untuk semua orang.
    conf=/etc/tsunagupad/hotspot.conf
    if [ ! -f "$conf" ]; then
        install -d -m 755 /etc/tsunagupad
        ssid="TsunaguPad-$(hostname | tr -dc 'A-Za-z0-9-' | cut -c1-16)"
        pass=$(od -An -N9 -tx1 /dev/urandom | tr -d ' \n')
        printf 'SSID=%s\nPASSWORD=%s\n' "$ssid" "$pass" > "$conf"
        chmod 640 "$conf"
        chgrp input "$conf" 2>/dev/null || true
        echo "Tsunagu-Pad hotspot: $ssid, kata sandi $pass (bisa diubah dari panel)"
    fi

    for u in $(getent group sudo | cut -d: -f4 | tr ',' ' '); do
        id -nG "$u" 2>/dev/null | grep -qw input || usermod -aG input "$u" || true
    done

    cat <<'MSG'

Tsunagu-Pad terpasang.

  1. Keluar lalu masuk lagi (logout) sekali, supaya panel GNOME dan izin
     perangkat masukannya ikut termuat.
  2. Nyalakan ekstensinya:  gnome-extensions enable tsunagu-pad@local
  3. Atau jalankan langsung:  tsunagupad share --second 1280x800

MSG
fi
exit 0
POST
chmod 755 "$root/DEBIAN/postinst"

cat > "$root/DEBIAN/prerm" <<'PRE'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    systemctl stop tsunagupad-hotspot.service 2>/dev/null || true
    systemctl disable tsunagupad-hotspot.service 2>/dev/null || true
fi
exit 0
PRE
chmod 755 "$root/DEBIAN/prerm"

say "Mengemas"
dpkg-deb --build --root-owner-group "$root" "$deb" >/dev/null
rm -rf "$root"
ls -lh "$deb" | awk '{print $9, $5}'
dpkg-deb --info "$deb" | sed -n '/Depends/,/Description/p' | head -12
