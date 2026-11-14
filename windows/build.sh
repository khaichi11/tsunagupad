#!/usr/bin/env bash
# Membangun Tsunagu-Pad untuk Windows beserta pemasangnya (.exe).
#
# Dijalankan di terminal MSYS2 UCRT64 (juga dipakai GitHub Actions):
#
#   pacman -S --needed git make zip unzip mingw-w64-ucrt-x86_64-{gcc,pkgconf,cmake,ninja,python,ntldd,nsis} \
#       mingw-w64-ucrt-x86_64-{gstreamer,gst-plugins-base,gst-plugins-good,gst-plugins-bad,gst-plugins-ugly} \
#       mingw-w64-ucrt-x86_64-{gst-libav,libnice,libsoup3,json-glib,qrencode,libplist,openssl,glib-networking}
#   windows/build.sh
#
# Hasilnya dist/windows/Tsunagu-Pad-<versi>-setup.exe. Pemasang itu membawa
# semua yang dibutuhkan: program, plugin GStreamer, UxPlay untuk AirPlay, dan
# Virtual Display Driver untuk layar kedua. Tidak ada yang diunduh saat
# pemasangan.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."

say() { printf '\033[1;36m==\033[0m %s\n' "$*"; }

version=$(sed -n 's/^VERSION *:= *//p' Makefile | head -n1)
prefix=${MINGW_PREFIX:-/ucrt64}
root=$PWD
work=$root/dist/windows
stage=$work/stage
mkdir -p "$work"

# Versi yang dikunci, supaya pemasang yang sama selalu berisi hal yang sama.
UXPLAY_REPO=https://github.com/FDH2/UxPlay.git
UXPLAY_COMMIT=05b0275cb1b726592700e47a6e0307544b3d3a52   # 1.74, dengan mDNS bawaan (tanpa Bonjour)
VDD_URL=https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip
VDD_SHA256=e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a
VDD_LICENSE_URL=https://raw.githubusercontent.com/VirtualDrivers/Virtual-Display-Driver/master/LICENSE

say "Membangun Tsunagu-Pad $version"
make clean >/dev/null
make -j"$(nproc)"

say "Membangun UxPlay (penerima AirPlay)"
# Tambalan: mDNS bawaan UxPlay di Windows hanya mengumumkan di satu adaptor
# dan selalu melewati adaptor Mobile Hotspot. Tambalan ini membolehkan
# Tsunagu-Pad memilih adaptornya (UXPLAY_MDNS_IPV4).
uxplay_stamp="$UXPLAY_COMMIT $(sha256sum windows/uxplay-mdns-interface.patch | cut -d' ' -f1)"
if [[ ! -x $work/uxplay/build/uxplay.exe || "$(cat "$work/uxplay/.tsunagupad-stamp" 2>/dev/null)" != "$uxplay_stamp" ]]; then
    rm -rf "$work/uxplay"
    git init -q "$work/uxplay"
    git -C "$work/uxplay" config core.autocrlf false
    git -C "$work/uxplay" fetch -q --depth 1 "$UXPLAY_REPO" "$UXPLAY_COMMIT"
    git -C "$work/uxplay" checkout -q FETCH_HEAD
    # Git for Windows bisa memberi CRLF pada berkas tambalan; baris LF dipakai.
    tr -d '\r' < windows/uxplay-mdns-interface.patch | git -C "$work/uxplay" apply --ignore-whitespace -
    cmake -S "$work/uxplay" -B "$work/uxplay/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C "$work/uxplay/build"
    echo "$uxplay_stamp" > "$work/uxplay/.tsunagupad-stamp"
fi

say "Mengambil Virtual Display Driver"
if [[ ! -f $work/vdd.zip ]] || ! echo "$VDD_SHA256  $work/vdd.zip" | sha256sum -c --quiet; then
    curl -fsSL -o "$work/vdd.zip" "$VDD_URL"
fi
echo "$VDD_SHA256  $work/vdd.zip" | sha256sum -c --quiet
curl -fsSL -o "$work/vdd-LICENSE" "$VDD_LICENSE_URL" || cp LICENSE "$work/vdd-LICENSE.missing"

say "Menyusun isi pemasang"
rm -rf "$stage"
mkdir -p "$stage"/{bin,lib/gstreamer-1.0,lib/gio/modules,libexec/gstreamer-1.0,share/tsunagupad/web,drivers/vdd,licenses}
cp build/tsunagupad.exe build/tsunagupad-tray.exe "$work/uxplay/build/uxplay.exe" "$stage/bin/"
cp web/* "$stage/share/tsunagupad/web/"
cp windows/hotspot.ps1 "$stage/share/tsunagupad/"
unzip -q -j -o "$work/vdd.zip" 'VirtualDisplayDriver/MttVDD.inf' 'VirtualDisplayDriver/MttVDD.dll' \
    'VirtualDisplayDriver/mttvdd.cat' -d "$stage/drivers/vdd"
cp "$prefix/libexec/gstreamer-1.0/gst-plugin-scanner.exe" "$stage/libexec/gstreamer-1.0/"

# Hanya plugin yang dipakai: tangkap layar dan encoder (D3D11, NVENC, Quick
# Sync, AMF, Media Foundation, x264), WebRTC, serta dekoder dan pemutar untuk
# UxPlay. Plugin lain (OpenCV dan sebagainya) menarik ratusan MB pustaka.
plugins=(
    coreelements
    app audioconvert audioresample playback typefindfunctions videoconvertscale videorate
    videotestsrc volume pbtypes rawparse
    autodetect rtp rtpmanager videofilter isomp4 audioparsers jpeg directsound adaptivedemux2 soup
    webrtc dtls srtp sctp d3d11 d3d12 nvcodec qsv amfcodec mediafoundation videoparsersbad
    wasapi wasapi2 codectimestamper hls
    x264 libav nice
    level imagefreeze   # UxPlay: "level" ada di pipa suaranya, tanpanya UxPlay langsung berhenti
)
for p in "${plugins[@]}"; do
    dll=$prefix/lib/gstreamer-1.0/libgst$p.dll
    if [[ -f $dll ]]; then
        cp "$dll" "$stage/lib/gstreamer-1.0/"
    else
        echo "!! plugin $p tidak ada di MSYS2, dilewati"
    fi
done
for m in libgioopenssl.dll; do
    [[ -f $prefix/lib/gio/modules/$m ]] && cp "$prefix/lib/gio/modules/$m" "$stage/lib/gio/modules/"
done

# Semua DLL yang dibutuhkan program dan plugin di atas, diambil dari MSYS2.
say "Mengumpulkan DLL"
collect() {
    ntldd -R "$@" 2>/dev/null | tr '\\' '/' | grep -i "$(cygpath -m "$prefix")/bin/" |
        sed -E 's/.*=> (.*\.dll).*/\1/I' | sort -u
}
targets=("$stage"/bin/*.exe "$stage"/lib/gstreamer-1.0/*.dll "$stage"/libexec/gstreamer-1.0/*.exe
         "$stage"/lib/gio/modules/*.dll)
for round in 1 2 3; do
    mapfile -t dlls < <(collect "${targets[@]}")
    for d in "${dlls[@]}"; do
        cp -n "$(cygpath -u "$d")" "$stage/bin/" 2>/dev/null || true
    done
    targets=("$stage"/bin/*.exe "$stage"/bin/*.dll "$stage"/lib/gstreamer-1.0/*.dll)
done
ls "$stage/bin" | wc -l

say "Lisensi dan sumber"
cp LICENSE "$stage/licenses/Tsunagu-Pad-LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$stage/licenses/THIRD_PARTY_NOTICES.md"
cp "$work/uxplay/LICENSE" "$stage/licenses/UxPlay-LICENSE.txt"
cp windows/uxplay-mdns-interface.patch "$stage/licenses/UxPlay-mdns-interface.patch"
[[ -f $work/uxplay/lib/llhttp/LICENSE-MIT ]] && cp "$work/uxplay/lib/llhttp/LICENSE-MIT" "$stage/licenses/UxPlay-llhttp-LICENSE.txt"
[[ -f $work/vdd-LICENSE ]] && cp "$work/vdd-LICENSE" "$stage/licenses/VirtualDisplayDriver-LICENSE.txt"
{
    echo "Sumber komponen pihak ketiga dalam pemasang Tsunagu-Pad $version untuk Windows"
    echo
    echo "UxPlay (GPL-3.0-or-later): $UXPLAY_REPO, commit $UXPLAY_COMMIT"
    echo "  https://github.com/FDH2/UxPlay/archive/$UXPLAY_COMMIT.tar.gz"
    echo "  Dimodifikasi oleh Tsunagu-Pad dengan UxPlay-mdns-interface.patch (di folder ini):"
    echo "  variabel UXPLAY_MDNS_IPV4 memilih adaptor jaringan untuk pengumuman mDNS."
    echo "Virtual Display Driver (MIT): $VDD_URL (sha256 $VDD_SHA256)"
    echo "  sumber: https://github.com/VirtualDrivers/Virtual-Display-Driver"
    echo
    echo "Pustaka berikut dibangun oleh proyek MSYS2. Kode sumber persis untuk setiap versi"
    echo "tersedia di https://repo.msys2.org/mingw/sources/ dan resep build-nya di"
    echo "https://github.com/msys2/MINGW-packages:"
    echo
    for f in "$stage"/bin/*.dll "$stage"/lib/gstreamer-1.0/*.dll "$stage"/lib/gio/modules/*.dll \
             "$stage"/libexec/gstreamer-1.0/*.exe; do
        pacman -Qqo "$prefix/${f#"$stage"/}" 2>/dev/null || true
    done | sort -u | while read -r pkg; do
        echo "  $(pacman -Q "$pkg")"
        lic=$prefix/share/licenses/${pkg#mingw-w64-ucrt-x86_64-}
        [[ -d $lic ]] && cp -r "$lic" "$stage/licenses/msys2-${pkg#mingw-w64-ucrt-x86_64-}"
    done
} > "$stage/licenses/SOURCES.txt"

say "Uji cepat program yang sudah disusun"
"$stage/bin/tsunagupad.exe" --version
"$stage/bin/tsunagupad.exe" qr "uji" >/dev/null
"$stage/bin/tsunagupad.exe" monitors || true
"$stage/bin/tsunagupad.exe" probe || echo "!! tidak ada encoder yang lolos uji di mesin build ini"

say "Membuat pemasang"
makensis=$(command -v makensis || true)
[[ -z $makensis && -x "/c/Program Files (x86)/NSIS/makensis.exe" ]] && makensis="/c/Program Files (x86)/NSIS/makensis.exe"
"$makensis" -V2 -DVERSION="$version" -DSTAGE="$(cygpath -w "$stage")" -DROOT="$(cygpath -w "$root")" \
    -DOUTFILE="$(cygpath -w "$work/Tsunagu-Pad-$version-setup.exe")" windows/installer.nsi
ls -lh "$work/Tsunagu-Pad-$version-setup.exe"

say "Arsip portabel"
rm -f "$work/Tsunagu-Pad-$version-windows-x64.zip"
(cd "$stage" && zip -qr "$work/Tsunagu-Pad-$version-windows-x64.zip" .)
ls -lh "$work/Tsunagu-Pad-$version-windows-x64.zip"
