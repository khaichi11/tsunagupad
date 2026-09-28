# Tsunagu-Pad

Use an iPad as a touch display for Ubuntu, and show the iPad screen on Ubuntu.
Everything runs on your own machine over the local network. Nothing is uploaded
to any server, and no app has to be installed on the iPad.

English | [Bahasa Indonesia](README.id.md)

## What it does

- **Share the Ubuntu screen to the iPad.** A monitor of your choice is encoded
  with hardware H.264 and sent over WebRTC to Safari. Touch, Apple Pencil (with
  pressure), trackpad, and keyboard input are forwarded back to Ubuntu.
- **Show the iPad screen on Ubuntu.** The built in Screen Mirroring of iPadOS is
  received through UxPlay, an open source AirPlay receiver installed from the
  Ubuntu repositories.
- **Direct Wi-Fi hotspot.** A virtual access point lets the iPad connect straight
  to the laptop while the laptop stays connected to its normal Wi-Fi network.
  Useful on campus or office networks that block device to device traffic.
- **One control in GNOME Quick Settings.** Start, stop, pick the monitor, pick
  the quality, and read the connection QR code from the system menu.

## How it works

The `tsunagupad` daemon captures an X11 screen region, encodes it to H.264 with
NVENC, Intel VA-API, or x264, and serves two things on port 8765: a small web
page for the iPad and a WebSocket channel used for WebRTC signalling and input
events. The iPad renders the stream in Safari and sends pointer, pen, and key
events back. Input is injected into X11 through XTest, and Apple Pencil pressure
through a virtual tablet device on `/dev/uinput`.

Latency is kept low in three ways: the encoder runs in a zero latency mode with
one second keyframes, the bitrate adapts to packet loss reported by the iPad,
and the stream asks the receiver to display each frame as soon as it is decoded
instead of buffering it.

## Requirements

- Ubuntu 24.04 or newer with GNOME 45 to 48.
- An **Ubuntu on Xorg** session. Screen capture and input injection use X11.
- An iPad with Safari. No app and no jailbreak needed.
- Optional: an NVIDIA or Intel GPU for hardware encoding. Software encoding
  works as a fallback.

## Install

```bash
git clone https://github.com/khaichi11/tsunagu-pad.git
cd tsunagu-pad
chmod +x install.sh
./install.sh
```

The installer asks for the administrator password once. It installs the packages
it needs, builds the daemon, installs the program and the GNOME extension into
`~/.local`, and sets up the hotspot service, the firewall rules, and the
permission for Apple Pencil pressure.

Reload GNOME Shell afterwards: press `Alt+F2`, type `r`, then Enter. On a Wayland
session, log out and log in again with Ubuntu on Xorg.

To remove everything:

```bash
./uninstall.sh
```

## Usage

Open Quick Settings in the top right corner and use the arrow next to the
Tsunagu-Pad button for the options.

### Share the Ubuntu screen

1. Select the mode for sharing the Ubuntu screen, then pick the monitor and the
   quality.
2. Tap the Tsunagu-Pad button.
3. Scan the QR code with the iPad camera, or type the address into Safari.
4. Optional: in Safari use Share, then Add to Home Screen, so the page opens
   full screen like an app.

On the iPad, a finger acts as a mouse: tap to click, hold or tap with two
fingers to right click, drag two fingers to scroll. Apple Pencil draws with
pressure, which works in apps such as Xournal++ and Krita. A small menu at the
top of the screen holds the keyboard, the touch mode, the language, and the
connection info.

### Show the iPad screen

1. Select the AirPlay mode and tap the button.
2. On the iPad, open Control Center, then Screen Mirroring.
3. Choose **Tsunagu-Pad Ubuntu**.

AirPlay from an iPad is a one way video and audio stream. iPadOS does not allow
a computer to control the iPad remotely.

### Direct hotspot

Turn on the hotspot in the menu and scan the Wi-Fi QR code with the iPad camera.
The hotspot runs on a virtual access point named `ap0`, so the main Wi-Fi
connection stays up and internet access is shared with the iPad.

Because a Wi-Fi radio can only work on one channel at a time, the hotspot always
follows the channel of the main Wi-Fi connection and moves with it. If the main
connection uses a DFS channel, a simultaneous hotspot is not allowed by
regulation and the panel explains what to do. The name and the password are
generated during installation and can be changed from the menu.

### Over Tailscale

If the network blocks device to device traffic and the direct hotspot is not an
option, Tailscale works as the transport. Install Tailscale on Ubuntu and the
Tailscale app on the iPad, sign both into the same tailnet, then open the
`100.x.y.z` address that the panel lists under the other addresses. Tsunagu-Pad
prefers the Tailscale address for the QR code when the tunnel is up.

```bash
sudo apt install tailscale
sudo tailscale up
```

Nothing else needs to change: the access key still applies, and no port has to
be exposed to the internet. Expect more latency than on a local network,
especially when Tailscale cannot reach a direct connection and falls back to a
relay. The AirPlay direction does not work over Tailscale, because iPadOS looks
for the receiver on the local network.

## Quality and latency

Four presets are available, from 960 pixels wide at 30 frames per second up to
the native resolution at 60 frames per second. The bitrate starts around
6 Mbit/s, rises while the network is clear, and drops quickly when packets are
lost, so the sharpest preset needs a few seconds to reach full quality.

Tap the info button in the iPad menu to see the frame rate and the estimated
delay. Expect roughly 40 to 90 milliseconds on a quiet network. A busy shared
Wi-Fi channel adds more.

## Troubleshooting

Check the encoder and the AirPlay receiver:

```bash
~/.local/bin/tsunagupad probe
```

Check the hotspot:

```bash
systemctl status tsunagupad-hotspot.service --no-pager
journalctl -u tsunagupad-hotspot.service -b -n 50 --no-pager -o cat
```

Check the panel:

```bash
journalctl -b /usr/bin/gnome-shell | grep -i tsunagupad | tail -20
```

Old daemons that still hold a port, for example after a GNOME Shell reload, are
stopped automatically when Tsunagu-Pad starts again.

## Limitations

- **No extra desktop.** The iPad mirrors an existing monitor; it does not become
  an additional display. Creating a virtual monitor with the `vkms` kernel module
  froze Xorg on hybrid Intel and NVIDIA laptops, so that path was removed. A
  physical HDMI or USB-C dummy adapter plus the monitor selector is the reliable
  way to get an extra screen.
- **Xorg only.** A Wayland session is not supported yet.
- **One iPad at a time.** A new connection replaces the previous one.

## Security

- The QR code and the address contain an access key. Anyone who has it can see
  and control the Ubuntu screen, so do not share it.
- The hotspot password is stored in `/etc/tsunagupad/hotspot.conf`, readable only
  by root and the user who installed the program.
- The hotspot runs DHCP and NAT on `10.88.0.0/24`. IP forwarding is restored to
  its previous value when the hotspot stops.
- Ports opened in UFW: `8765/tcp` for the page, `50000` to `50100/udp` for video,
  and the AirPlay ports `7000,7001,7100/tcp` with `6000,6001,7011,5353/udp`.

## Disclaimer

This is an experimental hobby project, provided free of charge and without any
warranty. Use it at your own risk.

The installer changes system settings: it installs packages, adds a hotspot
service, opens firewall ports, relaxes the permission on `/dev/uinput` for the
`input` group, and writes a polkit rule. Screen sharing gives anyone holding the
access key full control of your desktop, and the hotspot creates a Wi-Fi network
from your machine. Review the scripts before running them, and make sure this
kind of setup is allowed on the network you use.

The authors and contributors are not liable for any damage, data loss, downtime,
security incident, or policy violation resulting from the use of this software,
as stated in the MIT License.

## Contributing

Issues and pull requests are welcome. Commits follow
[Conventional Commits](https://www.conventionalcommits.org/), for example
`feat(hotspot): follow the uplink channel`. Please keep the style of the
surrounding code and describe how you tested the change.

## License and trademarks

The code in this repository is licensed under the [MIT License](LICENSE).
Third party components stay under their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Apple, iPad, iPadOS, Safari, Apple Pencil, and AirPlay are trademarks of Apple
Inc. This project is not affiliated with, endorsed by, or sponsored by Apple
Inc., and contains no Apple software, artwork, or trademarks in its own files.
Receiving AirPlay is performed by UxPlay, a separate program that you install
from your distribution; this repository does not ship or bundle it.
