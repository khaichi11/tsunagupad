# Third party notices

Tsunagu-Pad is licensed under the MIT License (see `LICENSE`). This repository
contains no third party source code, binaries, artwork, fonts, or trademarks. On
Ubuntu the components below are installed from your Linux distribution. The
Windows installer, which `windows/build.sh` builds, does carry binaries of
third party components; they are listed in [Windows installer](#windows-installer)
and stay under their own licenses.

Tsunagu-Pad memakai Lisensi MIT (lihat `LICENSE`). Repository ini tidak
menyertakan kode, binary, gambar, font, maupun merek pihak ketiga. Di Ubuntu,
komponen di bawah ini dipasang dari distribusi Linux Anda. Pemasang Windows yang
dibuat `windows/build.sh` memang membawa binary komponen pihak ketiga; daftarnya
ada di bagian [Windows installer](#windows-installer), dan semuanya tetap memakai
lisensinya masing-masing.

## Linked libraries (dynamic linking)

| Component | License | Use |
|---|---|---|
| GStreamer and its plugins | LGPL-2.1-or-later | Screen capture, H.264 encoding, WebRTC transport |
| GLib, GObject, GIO | LGPL-2.1-or-later | Main loop, utilities |
| libsoup 3 | LGPL-2.0-or-later | HTTP server and WebSocket for the iPad page |
| JSON-GLib | LGPL-2.1-or-later | Message parsing |
| libqrencode | LGPL-2.1-or-later | QR code generation |
| libX11, libXtst | MIT | Screen capture region and input injection |
| libpipewire 0.3 | MIT | Reading the virtual monitor's stream and its cursor metadata |

These libraries are used through their public interfaces as shared libraries.
No code from them is copied into this repository, which is why the MIT license
applies to the files here. Their license terms continue to apply to the
libraries themselves.

## Standards, and what is written here

WebRTC, H.264, RTP, SDP, ICE and DTLS are open standards published by the IETF,
the W3C and the ITU. Standards can be implemented by anyone; this project does
not implement them from scratch and does not claim to. The implementations come
from GStreamer and its plugins, from the browser on the iPad, and from the
graphics driver that encodes H.264 on the GPU.

What is written here, and what the MIT license covers, is the program that puts
those pieces together and the behaviour that comes out of it:

- the daemon in `src/`: the pipelines and their timing, the PipeWire reader that
  takes the cursor metadata the GStreamer element drops, the virtual monitor
  created and placed through Mutter's D-Bus interface, the pen, touch and
  keyboard path into virtual input devices, the bitrate and keyframe control,
  and the small web server and signalling;
- the page in `web/`: the client the iPad actually runs in Safari;
- the GNOME Shell extension in `extension/`, the hotspot and installer scripts
  in `system/`, and the animations in `docs/`;
- the Windows port in `src/win/`, `tray/`, and `windows/`: DXGI capture and the
  Windows encoders, input through `SendInput` and Windows Ink synthetic pens,
  the second screen driven through the Windows display configuration API, the
  service for the sign-in screen, the tray panel, and the installer;
- the secure channel in `src/crypto.c` and `web/secure.js`. SHA-256 (FIPS
  180-4), HMAC (RFC 2104), and ChaCha20 (RFC 8439) are published standards; the
  C side uses GLib's HMAC-SHA256 and the ChaCha20 written here, and the page
  side implements all three itself because Safari offers no `crypto.subtle` on
  a plain `http` page. The protocol around them, challenge-response login, per
  direction keys, and sequence numbers, is the design claimed here.

Using a library is not the same as claiming it. A program that speaks HTTP did
not invent HTTP; this one did not invent WebRTC. The work claimed here is the
design and the code in this repository.

## A note on the software encoders

H.264 is covered by patents in some countries, and the encoders are installed
from your distribution rather than shipped here: `vah264lpenc` and `vaapih264enc`
(Intel, LGPL), `nvautogpuh264enc` (NVIDIA, LGPL plugin over a proprietary
driver), `nvv4l2h264enc` on Jetson boards, and `x264enc` as a fallback. The x264
plugin is GPL-2.0-or-later; it is loaded at runtime from the distribution and no
part of it is copied or redistributed here. Check the rules of your own
jurisdiction if you redistribute binaries.

## Separate programs invoked at runtime

| Component | License | Use |
|---|---|---|
| UxPlay | GPL-3.0-or-later | Receives AirPlay screen mirroring from the iPad |
| hostapd | BSD-3-Clause | Runs the virtual Wi-Fi access point |
| dnsmasq | GPL-2.0-or-later or GPL-3.0-or-later | DHCP for hotspot clients |
| iw, iptables, NetworkManager, systemd, xrandr | GPL-2.0-or-later and others | Network and display configuration |

Tsunagu-Pad starts these as separate processes and never links against them, so
their copyleft terms do not extend to the code in this repository. They are not
redistributed here; the installer takes them from the Ubuntu repositories.

## GNOME Shell extension

The extension imports the public GNOME Shell and GJS modules at runtime
(GPL-2.0-or-later for GNOME Shell). Only our own JavaScript is distributed here.
The panel button uses icon names from the system icon theme rather than bundled
artwork, so no icon files are included.

## Windows installer

The Windows installer is built from the same MIT licensed code, and in addition
carries the following programs and libraries so that nothing has to be
downloaded during installation. Their license texts, and where to obtain the
exact corresponding source code, are installed in the `licenses` folder
(`licenses/SOURCES.txt` lists every package and version).

| Component | License | Use |
|---|---|---|
| GStreamer, its plugins, GLib, libsoup, JSON-GLib, libqrencode, libnice, and their dependencies, as built by the MSYS2 project | LGPL-2.1-or-later and others | Screen capture, encoding, WebRTC, the web server |
| x264 | GPL-2.0-or-later | Software H.264 encoder used when no hardware encoder works |
| FFmpeg (gst-libav) | LGPL-2.1-or-later, GPL-2.0-or-later as built by MSYS2 | Audio and video decoding for AirPlay |
| OpenSSL, libsrtp, usrsctp | Apache-2.0, BSD-3-Clause | DTLS and SRTP for WebRTC |
| UxPlay | GPL-3.0-or-later | Receives AirPlay screen mirroring from the iPad; runs as a separate program |
| Virtual Display Driver | MIT | The monitor with no cable used for the second screen |

These are separate files (DLLs, executables, and a driver package) used through
their public interfaces, or separate programs started as processes; no code
from them is copied into the files of this repository. Each can be replaced by
another build of the same version. The GPL applies to x264, FFmpeg as built,
and UxPlay as distributed in the installer; the source locations for the exact
versions are listed in `licenses/SOURCES.txt`, as those licenses require.

The Virtual Display Driver is signed by SignPath Foundation on behalf of the
Virtual Display Driver project. The installer verifies the SHA-256 checksum of
the released archive before including it.

Pemasang Windows dibuat dari kode MIT yang sama, ditambah program dan pustaka di
atas supaya tidak ada yang perlu diunduh saat memasang. Teks lisensinya dan
alamat kode sumber persisnya ikut terpasang di folder `licenses`.

## Trademarks

Apple, iPad, iPadOS, Safari, Apple Pencil, and AirPlay are trademarks of Apple
Inc. Ubuntu is a trademark of Canonical Ltd. Windows is a trademark of
Microsoft Corporation. GNOME is a trademark of the GNOME
Foundation. NVIDIA is a trademark of NVIDIA Corporation. Intel is a trademark of
Intel Corporation. These names are used only to describe compatibility. This
project is not affiliated with, endorsed by, or sponsored by any of them.

## Note on AirPlay

The AirPlay protocol is not publicly documented by Apple. Receiving it is done
entirely by UxPlay, a third party open source project. This repository contains
no AirPlay protocol implementation and no Apple keys or certificates. Check the
rules of your own jurisdiction and network before using it.

Protokol AirPlay tidak didokumentasikan secara publik oleh Apple. Penerimaannya
sepenuhnya dikerjakan UxPlay, proyek sumber terbuka pihak ketiga. Repository ini
tidak memuat implementasi protokol AirPlay maupun kunci atau sertifikat Apple.
Periksa aturan di wilayah dan jaringan Anda sebelum memakainya.
