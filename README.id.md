# Tsunagu-Pad

Pakai iPad sebagai layar sentuh untuk Ubuntu atau Windows, dan tampilkan layar
iPad di komputer. Semuanya berjalan di komputer sendiri melalui jaringan lokal.
Tidak ada data yang dikirim ke server mana pun, dan tidak ada aplikasi yang
perlu dipasang di iPad.

Pengguna Windows: lihat [Windows](#windows) untuk pemasang dan panel di area
notifikasi.

[English](README.md) | Bahasa Indonesia

## Demo

![Layar Ubuntu dibagikan ke iPad](docs/media/demo-share-id.gif)

Layar Ubuntu tampil di iPad, dan sentuhan, Apple Pencil, mouse, serta keyboard
dari iPad mengendalikan Ubuntu. Menggambar di iPad langsung masuk ke aplikasi
yang berjalan di Ubuntu.

![iPad dipakai sebagai layar kedua](docs/media/demo-second-id.gif)

Di Wayland, iPad bisa jadi layar kedua, bukan salinan layar pertama: satu
monitor tanpa kabel, jendela bisa diseret ke sana, dan penunjuk menyeberang
seperti di layar biasa.

![Layar iPad tampil di Ubuntu lewat AirPlay](docs/media/demo-airplay-id.gif)

Arah sebaliknya: layar iPad tampil di Ubuntu lewat AirPlay, memakai tombol
Pencerminan Layar yang memang sudah ada di iPadOS.

Animasinya digambar oleh [docs/make_demo.py](docs/make_demo.py), bukan
rekaman layar sungguhan, jadi tidak ada isi layar pribadi yang ikut
dipublikasikan. Jalankan `python3 docs/make_demo.py` untuk membuatnya ulang.

## Fungsi

- **Bagikan layar Ubuntu ke iPad.** Monitor pilihan Anda di-encode dengan H.264
  perangkat keras lalu dikirim lewat WebRTC ke Safari. Sentuhan, Apple Pencil
  (dengan tekanan), trackpad, dan keyboard iPad mengendalikan Ubuntu.
- **Tampilkan layar iPad di Ubuntu.** Fitur Pencerminan Layar bawaan iPadOS
  diterima oleh UxPlay, penerima AirPlay sumber terbuka yang dipasang dari
  repositori Ubuntu.
- **Hotspot Wi-Fi langsung.** Titik akses virtual membuat iPad tersambung
  langsung ke laptop, sementara laptop tetap tersambung ke Wi-Fi biasa. Berguna
  di jaringan kampus atau kantor yang memblokir lalu lintas antar perangkat.
- **Satu tombol di GNOME Quick Settings.** Menyalakan, mematikan, memilih
  monitor, memilih kualitas, dan menampilkan QR sambungan dari menu sistem.

## Cara kerja

Program `tsunagupad` menangkap layar, meng-encode-nya menjadi H.264 dengan
NVENC, Intel VA-API, atau x264, lalu menyajikan dua hal di port 8765: halaman
web kecil untuk iPad dan kanal WebSocket untuk negosiasi WebRTC serta event
masukan. iPad menampilkan video di Safari dan mengirim balik event penunjuk,
pena, dan tombol.

- **Sesi Xorg:** area layar ditangkap lewat X11 dan masukan disuntikkan lewat
  XTest.
- **Sesi Wayland:** layar berasal dari aliran PipeWire portal ScreenCast
  xdg-desktop-portal, dan masukan penunjuk, gulir, serta keyboard lewat portal
  RemoteDesktop. GNOME meminta izin satu kali lalu mengingatnya.

Tekanan dan kemiringan Apple Pencil lewat perangkat tablet virtual di
`/dev/uinput`. Di Wayland tablet itu dipetakan ke monitor yang dibagikan.

Di Wayland penangkapan meminta GNOME paling banyak sebesar laju frame preset,
dan encoder Intel VA-API membaca frame sebagai DMA-BUF tanpa salinan lewat CPU.
Selama iPad tersambung, penguncian layar otomatis ditahan, karena GNOME menolak
sesi berbagi layar baru selama layar terkunci.

Latensi ditekan dengan tiga cara: encoder berjalan pada mode tanpa penundaan dan
hanya mengirim keyframe sepuluh detik sekali atau saat iPad memintanya, bitrate
menyesuaikan diri terhadap paket yang hilang menurut laporan iPad, dan penerima
diminta menampilkan setiap frame begitu selesai didekode alih-alih menyimpannya
di buffer.

## Kebutuhan

- Ubuntu 24.04 atau lebih baru dengan GNOME 45 sampai 50.
- Sesi Ubuntu on Xorg atau Wayland. Keduanya bisa; GNOME 50 (Ubuntu 26.04) hanya
  Wayland.
- iPad dengan Safari. Tidak perlu aplikasi dan tidak perlu jailbreak.
- Opsional: GPU NVIDIA atau Intel untuk encoding perangkat keras. Di NVIDIA
  Jetson dipakai encoder Tegra. Encoding perangkat lunak tersedia sebagai
  cadangan.

## Pemasangan

Di Windows, lihat [Windows](#windows). Di Ubuntu:

Unduh `tsunagupad_<versi>_amd64.deb` dari
[halaman rilis](https://github.com/khaichi11/tsunagupad/releases) lalu buka
berkasnya, atau pasang lewat terminal:

```bash
sudo apt install ./tsunagupad_0.2.0_amd64.deb
```

Sudah, itu saja pemasangannya. Semua yang dibutuhkan programnya terdaftar
sebagai ketergantungan paket, jadi yang belum ada diunduhkan sendiri, lengkap
dengan bilah kemajuan di App Center atau daftar biasa di apt. Hotspotnya
mendapat kata sandi acak sendiri saat pertama dipasang, dan kata sandinya
dicetak di terminal.

Setelah itu keluar lalu masuk lagi sekali, kemudian nyalakan sakelarnya:

```bash
gnome-extensions enable tsunagu-pad@local
```

### Atau bangun sendiri

```bash
git clone https://github.com/khaichi11/tsunagupad.git
cd tsunagupad
chmod +x install.sh
./install.sh            # membangun dan memasang ke ~/.local
tool/build_deb.sh       # atau membuat berkas .deb-nya
```

Pemasang meminta kata sandi administrator satu kali. Ia memasang paket yang
dibutuhkan, membangun program, memasang program dan extension GNOME ke
`~/.local`, lalu menyiapkan layanan hotspot, aturan firewall, dan izin untuk
tekanan Apple Pencil.

Setelah itu muat ulang GNOME Shell: tekan `Alt+F2`, ketik `r`, lalu Enter. Pada
sesi Wayland, log out dan login kembali.

Untuk menghapus semuanya:

```bash
./uninstall.sh
```

## Pemakaian

Buka Quick Settings di pojok kanan atas, lalu pakai panah di sebelah tombol
Tsunagu-Pad untuk melihat pilihan.

### Bagikan layar Ubuntu

1. Pilih mode berbagi layar Ubuntu, lalu tentukan monitor dan kualitas. Di
   Wayland monitor dipilih di jendela izin GNOME.
2. Ketuk tombol Tsunagu-Pad. Di Wayland, GNOME menampilkan jendela izin pada
   pertama kali: pilih layar, nyalakan **Allow Remote Interaction** agar iPad
   bisa mengontrol Ubuntu, dan biarkan **Remember This Selection** menyala.
   Pilihan layarnya ikut diingat, itu sebabnya jendela izinnya tidak muncul
   lagi dan layar yang dibagikan selalu sama. Untuk memilih layar lain, pakai
   **Pilih layar lagi** di panel, atau jalankan sekali
   `tsunagupad share --pick-screen`.
3. Pindai QR dengan kamera iPad, atau ketik alamatnya di Safari.
4. Opsional: di Safari pilih Bagikan, lalu Tambahkan ke Layar Utama, supaya
   halaman terbuka layar penuh seperti aplikasi.

Di iPad, jari berfungsi sebagai mouse: ketuk untuk klik, tahan atau ketuk dua
jari untuk klik kanan, geser dua jari untuk menggulir. Apple Pencil menggambar
dengan tekanan, yang bekerja di aplikasi seperti Xournal++ dan Krita. Menu kecil
di bagian atas layar berisi keyboard, mode sentuh, pilihan bahasa, dan informasi
sambungan.

### Pakai iPad sebagai layar kedua

Hanya di Wayland. Layarnya datang dari monitor virtual kompositor, dan Xorg
tidak punya itu, jadi di sesi Xorg perintahnya berhenti dengan pesan, bukan
jalan setengah-setengah; panelnya pun menyembunyikan sakelar itu di sana. Kalau
sedang di Xorg, keluar lalu masuk lagi dan pilih Ubuntu on Wayland, atau
bagikan saja layar yang sudah ada.

```bash
tsunagupad share --second 1280x800
```

GNOME mendapat satu monitor tambahan yang tidak tercolok kabel apa pun. Jendela
bisa diseret ke sana, dan penunjuk dari laptop ikut menyeberang lalu digambar ke
dalam videonya, jadi rasanya seperti layar biasa. Atur sekali letaknya di
Pengaturan, Tampilan, dan sesi berikutnya menaruhnya di situ lagi: GNOME memberi
monitor ini nomor seri baru tiap sesi sehingga susunan simpanannya sendiri tidak
pernah cocok, jadi letaknya dicatat di `~/.config/tsunagupad/second-screen`.

Ukuran yang diminta itulah luas desktop yang didapat iPad. `--scale` mengatur
besar tampilannya: bawaannya `1`, jadi layar 1280x800 benar-benar seluas 1280
kali 800 titik; `1.5` membuat tulisan lebih besar dengan ruang dua pertiganya;
`ikut` menyamakan dengan skala monitor utama. Kalau ada aplikasi yang tidak mau
muat, beri layar yang lebih luas (`--second 1640x1024`), bukan skala yang lebih
besar.

### Tampilkan layar iPad

1. Pilih mode AirPlay lalu ketuk tombolnya.
2. Di iPad, buka Pusat Kontrol, lalu Pencerminan Layar.
3. Pilih **Tsunagu-Pad Ubuntu**.

AirPlay dari iPad adalah aliran video dan audio satu arah. iPadOS tidak
mengizinkan komputer mengendalikan iPad dari jarak jauh.

### Hotspot langsung

Nyalakan hotspot di menu, lalu pindai QR Wi-Fi dengan kamera iPad. Hotspot
berjalan di titik akses virtual bernama `ap0`, sehingga sambungan Wi-Fi utama
tetap hidup dan internetnya ikut dibagikan ke iPad.

Karena radio Wi-Fi hanya bisa bekerja pada satu kanal, hotspot selalu mengikuti
kanal Wi-Fi utama dan berpindah bersamanya. Bila Wi-Fi utama memakai kanal DFS,
hotspot bersamaan tidak diizinkan oleh regulasi, dan panel menjelaskan apa yang
perlu dilakukan. Nama dan kata sandinya dibuat saat pemasangan dan bisa diubah
dari menu.

### Lewat Tailscale

Bila jaringan memblokir lalu lintas antar perangkat dan hotspot langsung tidak
memungkinkan, Tailscale bisa dipakai sebagai jalurnya. Pasang Tailscale di
Ubuntu dan aplikasi Tailscale di iPad, masuk ke tailnet yang sama, lalu buka
alamat `100.x.y.z` yang ditampilkan panel pada daftar alamat lain. Tsunagu-Pad
mengutamakan alamat Tailscale untuk QR bila tunnel-nya hidup.

```bash
sudo apt install tailscale
sudo tailscale up
```

Tidak ada yang perlu diubah lagi: kode akses tetap berlaku dan tidak ada port
yang perlu dibuka ke internet. Latensinya lebih tinggi daripada jaringan lokal,
terutama bila Tailscale tidak mendapat sambungan langsung dan memakai relay.
Arah AirPlay tidak bisa lewat Tailscale, karena iPadOS mencari penerima di
jaringan lokal.

### Di Jetson atau mesin lain tanpa layar

Layar kedua bekerja sama saja di NVIDIA Jetson: monitor virtualnya datang dari
kompositor, bukan dari keluaran kartu grafis, jadi tidak perlu ada layar yang
tercolok. Encoder Tegra dipilih otomatis kalau tersedia, dan monitor virtual itu
menjadi monitor utama bila memang tidak ada layar lain.

Yang tetap dibutuhkan adalah sesi Wayland GNOME yang berjalan di Jetson-nya.
Tanpa layar tercolok, sesi biasanya tidak mau jalan sendiri. Pilihannya: nyalakan
login otomatis lalu biarkan dummy HDMI murah tertancap di portnya, atau jalankan
sesi GNOME headless lewat GNOME Remote Desktop, kemudian jalankan perintahnya di
dalam sesi itu:

```bash
tsunagupad share --second 1280x800
```

Jalur ini belum diuji di perangkat Jetson sungguhan. Kalau encoder-nya tidak
ketemu, `--encoder cpu` tetap bisa dipakai, dan `TSUNAGUPAD_DEBUG=1` menampilkan
encoder apa saja yang sudah dicoba.

Kalau iPad dipakai sebagai pengganti monitor, sesi Xorg juga cukup, dan dummy
plug tidak diperlukan. Saat monitor masih tercolok, jalankan `make login-setup`
dari terminal di desktop: Xorg akan selalu punya layar 1080p memakai EDID monitor
itu, dan layar login GDM ikut tampil di iPad sehingga password diketik dari sana.
QR tidak akan terlihat tanpa layar, jadi simpan dulu alamatnya di Layar Utama
iPad. Sakelar **iPad aktif di sesi berikutnya** di panel menentukan apakah ini
menyala saat login berikutnya, dan `make login-undo` mengembalikan semuanya.

Input dari iPad, termasuk password itu, dikirim terenkripsi dan terautentikasi
(lihat [Keamanan](#keamanan)). Tetap pakai jaringan milik sendiri dengan sandi
Wi-Fi yang kuat.

Pilihan yang tidak bisa dipakai disembunyikan otomatis: `tsunagupad capabilities`
menampilkan model mesin dan hasil deteksinya. Hotspot hanya muncul kalau chip
Wi-Fi bisa menjadi titik akses sambil tetap tersambung, dan encoder dipilih
dengan mencobanya satu per satu di mesin itu.

## Windows

Program yang sama berjalan di Windows 10 (versi 1809 atau lebih baru) dan
Windows 11, 64-bit. Setiap fitur versi Ubuntu punya padanannya di Windows:

| Ubuntu | Windows |
|---|---|
| Tombol GNOME Quick Settings | Ikon di samping jam, selalu tampil di sebelah Quick Settings, membuka panel yang dibuat seperti Quick Settings Windows 11 (juga `Ctrl+Alt+I`) |
| Tangkap layar lewat X11 atau PipeWire | DXGI Desktop Duplication, di GPU |
| NVENC, Intel VA-API, x264 | NVENC, Intel Quick Sync, AMD AMF, Media Foundation, x264 |
| Pena sebagai tablet uinput | Pena Windows Ink, dengan tekanan (1024 tingkat) dan kemiringan |
| Layar kedua dari monitor virtual Mutter | Layar kedua dari Virtual Display Driver, monitor tanpa kabel |
| UxPlay dari repositori Ubuntu | UxPlay ikut dibangun dan dibawa pemasang, dengan mDNS sendiri, jadi Bonjour Apple tidak perlu |
| Hotspot di `ap0` virtual | Mobile Hotspot Windows, Wi-Fi utama tetap tersambung |
| `make login-setup`, service systemd pengguna | Layanan Tsunagu-Pad: layar login dan sesi berikutnya di iPad |
| Aturan UFW | Aturan Windows Firewall untuk subnet lokal dan Tailscale saja |

### Pasang di Windows

Unduh `Tsunagu-Pad-<versi>-setup.exe` dari
[halaman rilis](https://github.com/khaichi11/tsunagupad/releases) lalu jalankan.
Pemasang membawa semua yang dibutuhkan, jadi tidak ada yang diunduh saat
memasang. Kata sandi administrator diminta sekali, dan ada empat bagian:

- **Tsunagu-Pad** (wajib): program, panel di area notifikasi, halaman iPad,
  AirPlay, dan aturan Windows Firewall yang hanya menerima subnet lokal dan
  Tailscale (`100.64.0.0/10`).
- **Layar kedua**: memasang Virtual Display Driver (lisensi MIT, ditandatangani
  SignPath Foundation untuk proyek Virtual Display Driver). Windows mungkin
  bertanya sekali apakah perangkat lunak perangkat ini boleh dipasang.
- **Layanan layar login dan sesi berikutnya**: layanan Windows yang tidak
  melakukan apa pun sampai sakelarnya dinyalakan di panel.
- **`tsunagupad` di PATH**, untuk dipakai dari terminal atau lewat SSH. Yang
  masuk PATH hanya folder kecil berisi peluncur, bukan folder pustaka
  programnya, supaya program lain tidak ikut memuatnya.

Tersedia juga arsip `.zip` portabel untuk mesin yang tidak boleh dipasangi
program; isinya program dan AirPlay, tanpa driver, layanan, dan aturan firewall.

### Pemakaian di Windows

Windows tidak mengizinkan aplikasi lain menambah tombol ke panel Quick
Settings-nya, jadi Tsunagu-Pad membawa panel yang cara kerjanya sama. Ikonnya
ada di samping jam, di sebelah ikon Quick Settings, dan dibuat selalu tampil,
tidak tersembunyi di balik tanda `^`. Klik ikonnya, atau tekan `Ctrl+Alt+I` dari
mana saja. Semua pilihan ada di panel itu saja, yang bisa digulir bila
layarnya pendek:

- sakelar di atas menyalakan dan mematikan Tsunagu-Pad, dengan statusnya di
  bawah nama;
- **Windows ke iPad** atau **iPad ke Windows** memilih arah;
- saat berbagi layar, QR untuk dipindai kamera iPad, alamatnya, dan tombol
  untuk menyalin alamat atau membuat kode akses baru;
- **Layar**: sakelar layar kedua dengan ukuran dan skalanya, monitor yang
  dibagikan, dan preset kualitas;
- **Hotspot**: sakelar hotspot, QR Wi-Fi-nya, serta nama dan kata sandinya,
  diubah langsung di panel;
- **Lainnya**: sesi berikutnya dan layar login, mulai otomatis saat masuk
  Windows, dan keluar.

Klik kanan pada ikonnya membuka panel yang sama; tidak ada menu terpisah.
Panelnya mengikuti tema gelap atau terang Windows. Di menu Start juga ada
*Tsunagu-Pad - Nyalakan atau matikan*, yang bisa di-pin ke Start atau taskbar
untuk menyalakan dan mematikan dengan sekali klik. Kotak *Mobile hotspot* yang
sudah ada di Quick Settings Windows mengendalikan hotspot yang sama dengan yang
dipakai Tsunagu-Pad.

- **Layar kedua.** Nyalakan *Jadikan iPad layar kedua*. Windows mendapat satu
  monitor selama iPad tersambung dan kehilangannya lagi saat Tsunagu-Pad
  berhenti; jendela yang tertinggal di sana dipindah ke layar lain. Seret
  letaknya sekali di Pengaturan, Sistem, Layar, dan sesi berikutnya monitor itu
  kembali ke tempat yang sama. Dari terminal:
  `tsunagupad share --second 1280x800 --scale 1.5`. Ukuran yang belum dikenal
  driver ditambahkan oleh layanan saat pertama dipakai.
- **AirPlay.** Pilih *iPad ke Windows*, lalu di iPad buka Pusat Kontrol,
  Pencerminan Layar, dan pilih **Tsunagu-Pad Windows**. Selama hotspot
  menyala, penerimanya diumumkan di hotspot, tempat iPad tersambung.
- **Jaringan kampus.** eduroam dan jaringan sejenis memisahkan perangkat satu
  sama lain, jadi iPad tidak bisa menyambung ke laptop walau di Wi-Fi yang
  sama. Panel memberi tahu bila laptop ada di jaringan seperti itu: nyalakan
  hotspot dan sambungkan iPad ke sana.
- **Hotspot.** *Hotspot langsung* menyalakan Mobile Hotspot Windows dan panel
  menampilkan QR Wi-Fi-nya. Nama dan kata sandinya diubah langsung di panel.
  Windows butuh sambungan Wi-Fi atau Ethernet untuk dibagikan sebelum
  hotspotnya bisa menyala.
- **Layar login dan sesi berikutnya.** *iPad aktif di sesi berikutnya* meminta
  kata sandi administrator, lalu layanan membagikan layar di layar login, layar
  kunci, dan jendela UAC, dan tetap berjalan setelah Anda masuk. Layanan memakai
  salinan kode akses Anda, jadi shortcut di iPad tetap berlaku. Lewat jalur ini
  pula `tsunagupad on` bekerja dari SSH.
- **Apple Pencil** sampai ke aplikasi sebagai pena Windows Ink dengan tekanan
  dan kemiringan, jadi Krita, Photoshop, OneNote, dan Xournal++ menggambar
  seperti dengan tablet. Masukan ke jendela yang berjalan sebagai administrator
  (Task Manager, jendela pemasang) diblokir Windows, kecuali sesinya berasal dari
  layanan.

Baris perintahnya sama seperti di Ubuntu, ditambah beberapa perintah Windows:

```
tsunagupad share [--monitor N|primary|\\.\DISPLAY2] [--second WxH] [--scale S] [--quality Q]
tsunagupad receive [--fullscreen]
tsunagupad on | off | status
tsunagupad hotspot on|off|status|config NAMA   (kata sandi dari input standar)
tsunagupad monitors
tsunagupad service status
tsunagupad probe
```

### Bangun sendiri untuk Windows

Pasang [MSYS2](https://www.msys2.org), buka terminal **UCRT64**, pasang paket yang
tercantum di bagian atas [windows/build.sh](windows/build.sh), lalu:

```bash
git clone https://github.com/khaichi11/tsunagupad.git
cd tsunagupad
windows/build.sh        # dist/windows/Tsunagu-Pad-<versi>-setup.exe
```

Skrip itu membangun program dan UxPlay, mengumpulkan plugin GStreamer dan
DLL-nya, memeriksa checksum rilis Virtual Display Driver, lalu menjalankan NSIS.
GitHub Actions menjalankan skrip yang sama di setiap push, memasang hasilnya
tanpa tampilan di Windows yang bersih, lalu menguji kanal terenkripsi pada
program yang terpasang.

### Penyelesaian masalah di Windows

- `tsunagupad probe` menampilkan encoder yang jalan. `TSUNAGUPAD_DEBUG=1`
  mencetak setiap encoder yang dicoba dan pipeline GStreamer-nya.
- Gambar tidak muncul di iPad: pastikan jaringannya sama, dan aturan Windows
  Firewall bernama *Tsunagu-Pad* ada (pasang ulang untuk memulihkannya).
  Jaringan berlabel *Public* tetap diizinkan, tetapi hanya dari subnet lokal.
- `tsunagupad service status` menunjukkan apakah layanannya terpasang dan menyala.
- Untuk menghapus semuanya: Pengaturan, Aplikasi, Tsunagu-Pad, Uninstall.
  Layanan, aturan firewall, entri PATH, dan driver layar virtual (bila dulu
  dipasang oleh Tsunagu-Pad) ikut dihapus.

## Kenapa ini dibuat

Ada tiga hal yang harus benar sekaligus: layar yang benar-benar dimiliki
kompositor di sesi Wayland, tidak ada yang perlu dipasang di iPad, dan Apple
Pencil yang sampai ke aplikasi sebagai tablet dengan tekanan serta kemiringan,
bukan sebagai mouse. Untuk sampai ke sana ada beberapa hal yang harus
dipecahkan dulu: kompositor yang hanya menggambar ulang monitor virtual saat
isinya berubah, kursor yang tidak pernah ikut terkirim, dan monitor yang nomor
serinya berganti tiap sesi sehingga letaknya selalu dilupakan.

## Kualitas dan latensi

Tersedia lima pilihan, dari lebar 960 piksel pada 30 frame per detik sampai
resolusi asli pada 60 frame per detik. Pilihan `monitor` (sampai 1920 piksel, 30 frame
per detik) ditujukan untuk mesin tanpa encoder perangkat keras. Bitrate dimulai sekitar 6 Mbit/s, naik
selama jaringan lega, dan turun cepat saat ada paket hilang, sehingga pilihan
paling tajam butuh beberapa detik untuk mencapai kualitas penuh.

Ketuk tombol info di menu iPad untuk melihat frame rate dan perkiraan jeda. Pada
jaringan yang lapang, jedanya sekitar 40 sampai 90 milidetik. Kanal Wi-Fi yang
ramai menambah jeda itu.

## Penyelesaian masalah

Memeriksa encoder dan penerima AirPlay:

```bash
~/.local/bin/tsunagupad probe
```

Memeriksa hotspot:

```bash
systemctl status tsunagupad-hotspot.service --no-pager
journalctl -u tsunagupad-hotspot.service -b -n 50 --no-pager -o cat
```

Memeriksa panel:

```bash
journalctl -b /usr/bin/gnome-shell | grep -i tsunagupad | tail -20
```

Program lama yang masih memegang port, misalnya setelah GNOME Shell dimuat
ulang, dihentikan otomatis saat Tsunagu-Pad dinyalakan kembali.

## Batasan

- **Bukan layar tambahan.** iPad menampilkan cermin dari monitor yang sudah ada,
  bukan menjadi monitor tambahan. Membuat monitor virtual dengan modul kernel
  `vkms` membuat Xorg membeku pada laptop dengan GPU hibrida Intel dan NVIDIA,
  sehingga jalur itu dihapus. Cara yang andal untuk layar tambahan adalah
  adaptor dummy HDMI atau USB-C, lalu pilih monitornya di menu.
- **Wayland butuh jendela izin GNOME satu kali.** Tanpa **Allow Remote
  Interaction** iPad hanya bisa menonton. Tekanan Apple Pencil butuh tablet
  virtual yang dipetakan ke monitor yang dibagikan lewat pengaturan GNOME; bila
  gagal, Pencil bekerja sebagai penunjuk biasa.
- **Satu iPad sekaligus.** Sambungan baru menggantikan yang lama.

## Keamanan

Perlindungan yang sama berlaku di Ubuntu dan Windows.

- **Kode akses tidak pernah lewat jaringan.** QR membawanya setelah tanda `#`
  (`http://…:8765/#k=…`), bagian alamat yang tidak pernah dikirim browser ke
  server. Saat iPad menyambung, komputer mengirim tantangan acak dan iPad
  menjawab dengan HMAC-SHA256 dari kode akses dan kedua nilai acak itu. Shortcut
  lama dengan `?k=` tetap berlaku; halamannya langsung memindahkan kode ke
  belakang `#`.
- **Semua isi sambungan dienkripsi dan diautentikasi.** Kunci khusus untuk satu
  sambungan diturunkan dari kode akses dan kedua nilai acak. Setiap pesan, dua
  arah, dienkripsi ChaCha20 dan membawa HMAC-SHA256 atas nomor urut dan isinya.
  Pesan yang diubah, diputar ulang, atau tertukar urutannya memutus sambungan.
  Ketikan, termasuk kata sandi di layar login, tidak bisa dibaca orang lain di
  jaringan, perintah tidak bisa disisipkan, dan sidik jari DTLS video WebRTC ikut
  terautentikasi sehingga videonya tidak bisa dibelokkan. Implementasinya ada di
  [src/crypto.c](src/crypto.c) dan [web/secure.js](web/secure.js), dicek dengan
  vektor uji RFC dan oleh [tool/test_channel.mjs](tool/test_channel.mjs) di CI.
  Yang tidak bisa dicegah: orang yang mampu mengubah halaman itu sendiri dalam
  perjalanannya ke iPad. Karena itu jaringan milik sendiri tetap penting.
- **Hanya jaringan lokal.** Yang dilayani hanya alamat loopback, privat
  (`10/8`, `172.16/12`, `192.168/16`), link-local, dan Tailscale (`100.64/10`,
  `fc00::/7`). `--allow-public` membuka batas ini bila memang perlu.
- **Batasan.** Setelah lima kode salah dalam satu menit, alamat itu harus
  menunggu satu menit. Sambungan WebSocket dari halaman situs lain (`Origin`
  berbeda) ditolak, ukuran pesan dibatasi, dan halamannya dikirim dengan Content
  Security Policy, `nosniff`, `no-referrer`, dan larangan dibingkai situs lain.
- **Anda melihat siapa yang menyambung.** Panel (GNOME maupun Windows) memberi
  notifikasi berisi alamat setiap perangkat baru yang memegang kendali, dan
  **Buat kode akses baru** langsung mencabut kode lama.
- Kata sandi hotspot di Ubuntu disimpan di `/etc/tsunagupad/hotspot.conf`, hanya
  bisa dibaca root dan pengguna yang memasang program. Di Windows itu adalah
  pengaturan Mobile Hotspot milik Windows sendiri.
- Hotspot Ubuntu menjalankan DHCP dan NAT pada `10.88.0.0/24`. Nilai IP
  forwarding dikembalikan seperti semula saat hotspot dimatikan.
- Firewall: UFW di Ubuntu dan Windows Firewall di Windows menerima halaman
  (`8765/tcp`), video (`50000` sampai `50100/udp`), dan AirPlay
  (`7000,7001,7100/tcp`, `6000,6001,7011,5353/udp`) hanya dari alamat privat,
  link-local, dan Tailscale.
- Layanan Windows: salinan kode akses dan log sesinya ada di
  `%ProgramData%\TsunaguPad\session`, hanya bisa dibaca SYSTEM dan
  administrator. Menyalakan atau mematikan sakelarnya butuh hak administrator.
  Layanan hanya menerima perintah dari komputer ini sendiri, dan hanya
  sekumpulan opsi tertentu (misalnya `--web-dir` ditolak, sehingga ia tidak bisa
  disuruh menyajikan berkas sembarangan).

## Penafian

Ini proyek eksperimental yang dibuat untuk kesenangan sendiri, diberikan gratis
dan tanpa jaminan apa pun. Pemakaian sepenuhnya menjadi tanggung jawab Anda.

Pemasang mengubah pengaturan sistem: memasang paket, menambah layanan hotspot,
membuka port firewall, melonggarkan izin `/dev/uinput` untuk grup `input`, dan
menulis aturan polkit. Di Windows pemasang menambah aturan firewall, dan bila
dipilih juga driver layar virtual, sebuah layanan, dan entri PATH. Berbagi layar memberi kendali penuh atas desktop kepada
siapa pun yang memegang kode akses, dan hotspot membuat jaringan Wi-Fi dari
komputer Anda. Periksa dulu isi skripnya sebelum dijalankan, dan pastikan
pemakaian seperti ini diizinkan di jaringan yang Anda pakai.

Penulis dan kontributor tidak bertanggung jawab atas kerusakan, kehilangan data,
gangguan layanan, insiden keamanan, atau pelanggaran kebijakan yang timbul dari
pemakaian perangkat lunak ini, sebagaimana dinyatakan dalam Lisensi MIT.

## Kontribusi

Issue dan pull request dipersilakan. Commit mengikuti
[Conventional Commits](https://www.conventionalcommits.org/), misalnya
`feat(hotspot): follow the uplink channel`. Pertahankan gaya kode di sekitarnya
dan jelaskan cara Anda mengujinya.

## Lisensi dan merek

Kode di repository ini memakai [Lisensi MIT](LICENSE). Komponen pihak ketiga
tetap memakai lisensinya masing-masing; lihat
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), yang juga menyebut terus
terang bagian mana yang berupa standar buatan orang lain dan bagian mana yang
memang pekerjaan di repository ini.

Apple, iPad, iPadOS, Safari, Apple Pencil, dan AirPlay adalah merek Apple Inc.
Windows adalah merek Microsoft Corporation.
Proyek ini tidak berafiliasi, tidak didukung, dan tidak disponsori oleh Apple
Inc., serta tidak memuat perangkat lunak, gambar, maupun merek Apple di
berkasnya sendiri. Penerimaan AirPlay dikerjakan UxPlay, program terpisah: di
Ubuntu Anda memasangnya dari distribusi, dan pemasang Windows membawa hasil
build-nya lengkap dengan lisensi dan alamat kode sumbernya. Repository ini
tidak memuatnya.
