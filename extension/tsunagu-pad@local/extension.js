/* Tsunagu-Pad, tombol Quick Settings GNOME untuk menyatukan layar Ubuntu dan iPad.
 * Extension ini hanya antarmuka; kerja beratnya dilakukan program `tsunagupad` (C)
 * serta layanan sistem hotspot.
 * SPDX-License-Identifier: MIT
 */
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as ModalDialog from 'resource:///org/gnome/shell/ui/modalDialog.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

Gio._promisify(Gio.Subprocess.prototype, 'communicate_utf8_async');

const RECEIVER_NAME = 'Tsunagu-Pad Ubuntu';
const HOTSPOT_UNIT = 'tsunagupad-hotspot.service';
const HOTSPOT_CONFIG_PATH = '/etc/tsunagupad/hotspot.conf';
const HOTSPOT_HELPER = '/usr/local/lib/tsunagupad/hotspot-config';
/* Batas sisi QR dalam piksel. Ukuran modul dihitung dari batas ini supaya QR
 * yang panjang (mis. kata sandi Wi-Fi) tidak melebihi lebar menu Quick Settings. */
const QR_MAX_PX = 168;
const QUALITIES = [
    ['hemat', 'Hemat (jaringan lemah)'],
    ['lancar', 'Lancar · 720p'],
    ['seimbang', 'Seimbang · 1600px'],
    ['tajam', 'Tajam · resolusi asli'],
];
const CONFIG_PATH = GLib.build_filenamev([GLib.get_user_config_dir(), 'tsunagupad', 'panel.json']);

function loadConfig() {
    const defaults = {
        mode: 'share',
        monitorName: '',
        quality: 'lancar',
        fullscreen: false,
        tuning: 2,
    };
    let config;
    try {
        const [, bytes] = GLib.file_get_contents(CONFIG_PATH);
        config = {...defaults, ...JSON.parse(new TextDecoder().decode(bytes))};
    } catch {
        return defaults;
    }
    /* Kunci versi lama: layar kedua (dihapus karena monitor virtual membuat layar
     * hitam di laptop GPU hibrida), indeks monitor (berubah saat kabel dicolok),
     * dan kata sandi hotspot NetworkManager. */
    for (const key of ['extend', 'extendRegion', 'extendSize', 'monitor', 'hotspotPassword'])
        delete config[key];
    if (!config.tuning) {
        config.quality = 'lancar';
        config.tuning = 2;
    }
    return config;
}

function saveConfig(config) {
    try {
        GLib.mkdir_with_parents(GLib.path_get_dirname(CONFIG_PATH), 0o700);
        GLib.file_set_contents(CONFIG_PATH, JSON.stringify(config, null, 2));
    } catch (e) {
        console.error(`Tsunagu-Pad: gagal menyimpan pengaturan: ${e.message}`);
    }
}

/* Berkas polos KUNCI=nilai yang ditulis helper root (tanpa kutip/escape). */
function readHotspotConfig() {
    try {
        const [, bytes] = GLib.file_get_contents(HOTSPOT_CONFIG_PATH);
        const values = {ssid: '', password: ''};
        for (const line of new TextDecoder().decode(bytes).split('\n')) {
            const m = line.match(/^(SSID|PASSWORD)=(.*)$/);
            if (m)
                values[m[1].toLowerCase()] = m[2];
        }
        return values;
    } catch {
        return null;
    }
}

/* Format QR Wi-Fi: karakter \ ; , : " wajib di-escape. */
function wifiQrText(ssid, password) {
    const escape = s => s.replace(/([\\;,:"])/g, '\\$1');
    return `WIFI:T:WPA;S:${escape(ssid)};P:${escape(password)};;`;
}

function findBinary() {
    const candidates = [
        GLib.find_program_in_path('tsunagupad'),
        GLib.build_filenamev([GLib.get_home_dir(), '.local', 'bin', 'tsunagupad']),
        '/usr/local/bin/tsunagupad',
        '/usr/bin/tsunagupad',
    ];
    return candidates.find(p => p && GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE)) ?? null;
}

async function run(argv, input = null) {
    let flags = Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE;
    if (input !== null)
        flags |= Gio.SubprocessFlags.STDIN_PIPE;
    try {
        const proc = Gio.Subprocess.new(argv, flags);
        const [stdout, stderr] = await proc.communicate_utf8_async(input, null);
        return {ok: proc.get_successful(), stdout: stdout ?? '', stderr: stderr ?? ''};
    } catch (e) {
        return {ok: false, stdout: '', stderr: e.message};
    }
}

function lastLine(res, fallback) {
    return `${res.stderr}\n${res.stdout}`.split('\n').map(l => l.trim()).filter(Boolean).pop() ?? fallback;
}

/* Output X11 yang sedang menyala: {name, primary, x, y, width, height}. */
async function xrandrOutputs() {
    const res = await run(['xrandr', '--query']);
    if (!res.ok)
        throw new Error(lastLine(res, 'xrandr tidak tersedia'));
    const outputs = [];
    for (const line of res.stdout.split('\n')) {
        const m = line.match(/^(\S+) connected (primary )?(\d+)x(\d+)\+(\d+)\+(\d+)/);
        if (m) {
            outputs.push({
                name: m[1], primary: !!m[2],
                width: Number(m[3]), height: Number(m[4]), x: Number(m[5]), y: Number(m[6]),
            });
        }
    }
    return {outputs, text: res.stdout};
}

/* Proses `tsunagupad` yang berjalan di belakang; keluarannya dibaca per baris. */
class Daemon {
    constructor(argv, onLine, onExit) {
        /* stderr digabung agar kesalahan UxPlay/GStreamer tidak membisu. */
        this._proc = Gio.Subprocess.new(argv,
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE);
        this._cancellable = new Gio.Cancellable();
        this._stream = new Gio.DataInputStream({
            base_stream: this._proc.get_stdout_pipe(),
            close_base_stream: true,
        });
        this._onLine = onLine;
        this._onExit = onExit;
        this._readNext();
    }

    _readNext() {
        this._stream.read_line_async(GLib.PRIORITY_DEFAULT, this._cancellable, (stream, res) => {
            let line;
            try {
                [line] = stream.read_line_finish_utf8(res);
            } catch {
                return; // dibatalkan karena stop()
            }
            if (line === null) {
                this._proc.wait_async(null, () => this._onExit());
                return;
            }
            this._onLine(line);
            this._readNext();
        });
    }

    stop() {
        this._cancellable.cancel();
        this._proc.send_signal(15);
        return new Promise(resolve => this._proc.wait_async(null, () => resolve()));
    }
}

/* Kelas turunan ModalDialog wajib didaftarkan ke GObject; tanpa itu GNOME
 * Shell melempar "Tried to construct an object without a GType". */
const HotspotConfigDialog = GObject.registerClass(
class HotspotConfigDialog extends ModalDialog.ModalDialog {
    _init(config, onSave) {
        super._init({styleClass: 'tsunagupad-dialog'});
        this._onSave = onSave;
        this._saving = false;

        const box = new St.BoxLayout({vertical: true, x_expand: true, style_class: 'tsunagupad-dialog-box'});
        this.contentLayout.add_child(box);
        box.add_child(new St.Label({text: 'Hotspot Tsunagu-Pad', style_class: 'tsunagupad-dialog-title'}));

        box.add_child(new St.Label({text: 'Nama Wi-Fi', style_class: 'tsunagupad-dialog-label'}));
        this._ssid = new St.Entry({text: config?.ssid ?? '', can_focus: true, x_expand: true});
        box.add_child(this._ssid);

        box.add_child(new St.Label({text: 'Kata sandi (8–63 karakter)', style_class: 'tsunagupad-dialog-label'}));
        this._password = new St.PasswordEntry({text: config?.password ?? '', can_focus: true, x_expand: true});
        box.add_child(this._password);

        this._status = new St.Label({style_class: 'tsunagupad-dialog-status'});
        this._status.clutter_text.line_wrap = true;
        box.add_child(this._status);

        this._ssid.clutter_text.connect('activate', () => this._save());
        this._password.clutter_text.connect('activate', () => this._save());
        this.setButtons([
            {label: 'Batal', action: () => this.close(), key: Clutter.KEY_Escape},
            {label: 'Simpan', action: () => this._save(), default: true},
        ]);
        this.setInitialKeyFocus(this._ssid.clutter_text);
    }

    async _save() {
        if (this._saving)
            return;
        const ssid = this._ssid.get_text().trim();
        const password = this._password.get_text();
        const ssidBytes = new TextEncoder().encode(ssid).length;
        if (ssidBytes < 1 || ssidBytes > 32 || /[\x00-\x1f\x7f]/.test(ssid)) {
            this._status.text = 'Nama Wi-Fi harus 1–32 byte.';
            return;
        }
        if (!/^[\x20-\x7e]{8,63}$/.test(password)) {
            this._status.text = 'Kata sandi harus 8–63 karakter: huruf, angka, simbol, atau spasi (tanpa emoji/huruf beraksen).';
            return;
        }
        this._saving = true;
        this._status.text = 'Menyimpan…';
        try {
            await this._onSave(ssid, password);
            this.close();
        } catch (e) {
            this._status.text = `Gagal: ${e.message}`;
        } finally {
            this._saving = false;
        }
    }
});

const QrCode = GObject.registerClass(
class QrCode extends St.DrawingArea {
    constructor() {
        super({style_class: 'tsunagupad-qr', x_align: Clutter.ActorAlign.CENTER});
        this._text = '';
        this._rows = [];
        this.connect('repaint', () => this._paint());
    }

    setRows(rows) {
        rows ??= '';
        if (rows === this._text)
            return;
        this._text = rows;
        this._rows = rows ? rows.split(',') : [];
        const modules = this._rows.length + 4;   // termasuk margin putih 2 modul
        const size = Math.max(2, Math.floor(QR_MAX_PX / modules)) * modules;
        this.set_size(size, size);
        this.queue_repaint();
    }

    _paint() {
        const cr = this.get_context();
        const [width, height] = this.get_surface_size();
        cr.setSourceRGB(1, 1, 1);
        cr.rectangle(0, 0, width, height);
        cr.fill();

        const n = this._rows.length;
        if (n > 0) {
            const m = Math.floor(Math.min(width, height) / (n + 4));
            const offset = Math.floor((Math.min(width, height) - m * n) / 2);
            cr.setSourceRGB(0, 0, 0);
            this._rows.forEach((row, y) => {
                for (let x = 0; x < row.length; x++) {
                    if (row[x] === '1')
                        cr.rectangle(offset + x * m, offset + y * m, m, m);
                }
            });
            cr.fill();
        }
        cr.$dispose();
    }
});

const TsunaguPadToggle = GObject.registerClass(
class TsunaguPadToggle extends QuickMenuToggle {
    constructor(extension, panelIcon) {
        super({title: 'Tsunagu-Pad', iconName: 'video-display-symbolic', toggleMode: false});

        this._panelIcon = panelIcon;
        this._config = loadConfig();
        saveConfig(this._config);

        this._daemon = null;
        this._daemonKind = null;
        this._starting = false;
        this._startedAt = 0;
        this._quickExits = 0;
        this._status = '';
        this._urls = [];
        this._qrRows = '';
        this._outputs = [];
        this._shareOutput = null;
        this._hotspotActive = false;
        this._hotspotBusy = false;
        this._hotspotStatus = '';
        this._hotspotQrText = null;
        this._hotspotRefresh = null;
        this._receiverRestartId = 0;
        this._monitorCheckId = 0;
        this._destroyed = false;

        this.menu.setHeader('video-display-symbolic', 'Tsunagu-Pad', 'Satukan layar Ubuntu dan iPad');
        this._buildMenu();

        this.connect('clicked', () => {
            if (this._starting)
                return;
            if (this._daemon)
                this._stopDaemon().catch(e => this._notify(e.message));
            else
                this._start().catch(e => this._notify(e.message));
        });
        this._menuOpenId = this.menu.connect('open-state-changed', (menu, open) => {
            if (!open)
                return;
            this._rebuildMonitorMenu().catch(e => console.warn(`Tsunagu-Pad: ${e.message}`));
            this._refreshHotspot().catch(e => console.warn(`Tsunagu-Pad: ${e.message}`));
        });
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed',
            () => this._queueMonitorCheck());
        /* Status hotspot hanya dipantau saat perlu, bukan terus-menerus. */
        this._pollId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 5, () => {
            if (this._hotspotActive || this.menu.isOpen)
                this._refreshHotspot().catch(() => {});
            return GLib.SOURCE_CONTINUE;
        });

        this._sync();
        this._detectState().catch(e => console.warn(`Tsunagu-Pad: ${e.message}`));
    }

    /* ---------- menu ---------- */

    _buildMenu() {
        this._modeItems = {
            share: this.menu.addAction('Bagikan layar Ubuntu ke iPad', () => this._setMode('share')),
            receive: this.menu.addAction('Tampilkan layar iPad di Ubuntu (AirPlay)', () => this._setMode('receive')),
        };
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        this._monitorMenu = new PopupMenu.PopupSubMenuMenuItem('', false);
        this.menu.addMenuItem(this._monitorMenu);

        this._qualityMenu = new PopupMenu.PopupSubMenuMenuItem('', false);
        this._qualityItems = QUALITIES.map(([id, label]) =>
            [id, this._qualityMenu.menu.addAction(label, () => this._setQuality(id))]);
        this.menu.addMenuItem(this._qualityMenu);

        this._fullscreenSwitch = new PopupMenu.PopupSwitchMenuItem('Layar penuh', this._config.fullscreen);
        this._fullscreenSwitch.connect('toggled', (item, state) => {
            this._config.fullscreen = state;
            saveConfig(this._config);
            if (this._daemonKind === 'receive')
                this._restart().catch(e => this._notify(e.message));
        });
        this.menu.addMenuItem(this._fullscreenSwitch);

        /* QR sambungan (Ubuntu ke iPad) atau petunjuk AirPlay (iPad ke Ubuntu) */
        this._infoItem = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const infoBox = new St.BoxLayout({vertical: true, x_expand: true, style_class: 'tsunagupad-info'});
        this._qr = new QrCode();
        this._infoLabel = new St.Label({style_class: 'tsunagupad-caption', x_expand: true});
        this._infoLabel.clutter_text.line_wrap = true;
        infoBox.add_child(this._qr);
        infoBox.add_child(this._infoLabel);
        this._infoItem.add_child(infoBox);
        this.menu.addMenuItem(this._infoItem);

        this._copyItem = this.menu.addAction('Salin alamat', () => {
            St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, this._urls[0] ?? '');
        });

        /* Hotspot hostapd di AP virtual; Wi-Fi utama tetap tersambung. */
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._hotspotSwitch = new PopupMenu.PopupSwitchMenuItem('', false);
        this._hotspotSwitch.connect('toggled', (item, state) => {
            this._setHotspot(state).catch(e => this._notify(`Hotspot gagal: ${e.message}`));
        });
        this.menu.addMenuItem(this._hotspotSwitch);

        this._hotspotItem = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const hotspotBox = new St.BoxLayout({vertical: true, x_expand: true, style_class: 'tsunagupad-info'});
        this._hotspotQr = new QrCode();
        this._hotspotLabel = new St.Label({style_class: 'tsunagupad-caption', x_expand: true});
        this._hotspotLabel.clutter_text.line_wrap = true;
        hotspotBox.add_child(this._hotspotQr);
        hotspotBox.add_child(this._hotspotLabel);
        this._hotspotItem.add_child(hotspotBox);
        this.menu.addMenuItem(this._hotspotItem);

        this.menu.addAction('Atur nama dan kata sandi hotspot…', () => {
            const dialog = new HotspotConfigDialog(readHotspotConfig(),
                (ssid, password) => this._saveHotspotConfig(ssid, password));
            dialog.open();
        });
    }

    _outputLabel(o) {
        const kind = o.name.startsWith('eDP') ? 'Layar laptop' : o.name;
        return `${kind} · ${o.width}×${o.height}${o.primary ? ' (utama)' : ''}`;
    }

    _selectedOutput() {
        return this._outputs.find(o => o.name === this._config.monitorName) ??
            this._outputs.find(o => o.primary) ?? this._outputs[0] ?? null;
    }

    async _rebuildMonitorMenu() {
        await this._refreshOutputs();
        if (this._destroyed)
            return;
        this._monitorMenu.menu.removeAll();
        const selected = this._selectedOutput();
        for (const o of this._outputs) {
            const item = this._monitorMenu.menu.addAction(this._outputLabel(o), () => {
                /* Disimpan per nama konektor, bukan urutan: urutan berubah saat kabel dicolok. */
                this._config.monitorName = o.name;
                saveConfig(this._config);
                if (this._daemonKind === 'share')
                    this._restart().catch(e => this._notify(e.message));
                this._sync();
            });
            item.setOrnament(o.name === selected?.name ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
        }
        this._sync();
    }

    _setMode(mode) {
        if (this._config.mode === mode)
            return;
        this._config.mode = mode;
        saveConfig(this._config);
        if (this._daemon)
            this._restart().catch(e => this._notify(e.message));
        this._sync();
    }

    _setQuality(id) {
        this._config.quality = id;
        saveConfig(this._config);
        if (this._daemonKind === 'share')
            this._restart().catch(e => this._notify(e.message));
        this._sync();
    }

    _sync() {
        if (this._destroyed)
            return;
        const running = !!this._daemon;
        const share = this._config.mode === 'share';

        this.checked = running;
        if (this._starting)
            this.subtitle = 'Menyiapkan…';
        else if (running)
            this.subtitle = this._status;
        else
            this.subtitle = share ? 'Ubuntu ke iPad' : 'iPad ke Ubuntu';
        this._panelIcon.visible = running;

        for (const [id, item] of Object.entries(this._modeItems))
            item.setOrnament(id === this._config.mode ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);

        this._monitorMenu.visible = share;
        const selected = this._selectedOutput();
        this._monitorMenu.label.text = `Monitor: ${selected ? this._outputLabel(selected) : 'utama'}`;

        this._qualityMenu.visible = share;
        const quality = QUALITIES.find(([id]) => id === this._config.quality) ?? QUALITIES[1];
        this._qualityMenu.label.text = `Kualitas: ${quality[1]}`;
        for (const [id, item] of this._qualityItems)
            item.setOrnament(id === quality[0] ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);

        this._fullscreenSwitch.visible = !share;

        const showQr = running && this._daemonKind === 'share' && !!this._qrRows;
        const showAirplay = running && this._daemonKind === 'receive';
        this._infoItem.visible = showQr || showAirplay;
        this._copyItem.visible = showQr;
        this._qr.visible = showQr;
        if (showQr) {
            this._qr.setRows(this._qrRows);
            let text = `Scan dengan kamera iPad, atau buka di Safari:\n${this._urls[0] ?? ''}`;
            const others = this._urls.slice(1, 3);
            if (others.length)
                text += `\n\nAlamat lain:\n${others.join('\n')}`;
            this._infoLabel.text = text;
        } else if (showAirplay) {
            this._infoLabel.text =
                `Di iPad: buka Pusat Kontrol, lalu Pencerminan Layar (Screen Mirroring), lalu pilih “${RECEIVER_NAME}”`;
        }

        this._syncHotspot();
    }

    /* ---------- proses tsunagupad ---------- */

    async _start() {
        if (this._starting || this._daemon || this._destroyed)
            return;
        const bin = findBinary();
        if (!bin) {
            this._notify('Program tsunagupad belum terpasang. Jalankan ./install.sh dari folder proyek Tsunagu-Pad.');
            return;
        }

        const kind = this._config.mode;
        this._starting = true;
        this._sync();
        try {
            let argv;
            if (kind === 'share') {
                if (Meta.is_wayland_compositor())
                    throw new Error('Mode Ubuntu ke iPad butuh sesi "Ubuntu on Xorg" (pilih di layar login).');
                const out = await this._shareTarget();
                this._shareOutput = out;
                argv = [bin, 'share', '--region', `${out.x},${out.y},${out.width},${out.height}`,
                    '--quality', this._config.quality];
            } else {
                argv = [bin, 'receive', '--name', RECEIVER_NAME];
                if (this._config.fullscreen)
                    argv.push('--fullscreen');
            }
            if (this._destroyed)
                return;

            const daemon = new Daemon(argv,
                line => this._onLine(daemon, kind, line),
                () => this._onExit(daemon));
            this._daemon = daemon;
            this._daemonKind = kind;
            this._startedAt = GLib.get_monotonic_time();
            this._status = kind === 'share' ? 'Menyiapkan…' : 'Menunggu iPad…';
            this._urls = [];
            this._qrRows = '';
        } finally {
            this._starting = false;
            this._sync();
        }
    }

    async _shareTarget() {
        const {outputs} = await xrandrOutputs();
        this._outputs = outputs;
        const out = this._selectedOutput();
        if (!out)
            throw new Error('Tidak ada monitor aktif untuk dibagikan.');
        return out;
    }

    _stopDaemon() {
        if (this._receiverRestartId) {
            GLib.Source.remove(this._receiverRestartId);
            this._receiverRestartId = 0;
        }
        const daemon = this._daemon;
        this._daemon = null;
        this._daemonKind = null;
        this._shareOutput = null;
        this._sync();
        return daemon ? daemon.stop() : Promise.resolve();
    }

    async _restart() {
        await this._stopDaemon();
        if (!this._destroyed)
            await this._start();
    }

    _onExit(daemon) {
        if (this._daemon !== daemon)
            return;
        const kind = this._daemonKind;
        this._daemon = null;
        this._daemonKind = null;
        this._shareOutput = null;

        if (kind === 'receive' && !this._destroyed && this._config.mode === 'receive') {
            const quick = GLib.get_monotonic_time() - this._startedAt < 10 * GLib.USEC_PER_SEC;
            this._quickExits = quick ? this._quickExits + 1 : 0;
            if (this._quickExits >= 3) {
                this._quickExits = 0;
                this._notify('Penerima AirPlay berhenti berulang kali. Pastikan tidak ada UxPlay lain yang berjalan, lalu nyalakan lagi.');
            } else {
                this._status = 'Penerima berhenti, menyalakan ulang…';
                this._receiverRestartId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
                    this._receiverRestartId = 0;
                    if (!this._destroyed && !this._daemon && this._config.mode === 'receive')
                        this._start().catch(e => this._notify(e.message));
                    return GLib.SOURCE_REMOVE;
                });
            }
        }
        this._sync();
    }

    _onLine(daemon, kind, line) {
        if (this._daemon !== daemon)
            return;

        if (line.startsWith('{')) {
            let ev;
            try {
                ev = JSON.parse(line);
            } catch {
                return;
            }
            switch (ev.event) {
            case 'ready':
                this._urls = (ev.urls ?? '').split(' ').filter(Boolean);
                this._qrRows = ev.qr;
                this._status = 'Menunggu iPad…';
                break;
            case 'client':
                if (ev.state === 'connected')
                    this._status = 'Terhubung ke iPad';
                else if (ev.state === 'disconnected')
                    this._status = 'Menunggu iPad…';
                else if (ev.state === 'rejected')
                    this._status = 'Kode akses ditolak';
                break;
            case 'stats':
                this._status = `iPad · ${ev.fps} fps`;
                break;
            case 'error':
                this._notify(ev.message);
                break;
            case 'warning':
                console.warn(`Tsunagu-Pad: ${ev.message}`);
                break;
            }
        } else if (kind === 'receive') {
            /* Log UxPlay */
            const text = line.toLowerCase();
            if (text.includes('connection request from'))
                this._status = 'iPad menyambung…';
            else if (text.includes('begin streaming'))
                this._status = 'Menampilkan layar iPad';
            else if (text.includes('connection closed'))
                this._status = 'Menunggu iPad…';
            else if (/\berror\b|failed/.test(text))
                console.error(`Tsunagu-Pad UxPlay: ${line}`);
        }
        this._sync();
    }

    /* ---------- monitor & kabel ---------- */

    _queueMonitorCheck() {
        if (this._monitorCheckId)
            GLib.Source.remove(this._monitorCheckId);
        this._monitorCheckId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._monitorCheckId = 0;
            this._checkMonitors().catch(e => console.warn(`Tsunagu-Pad: ${e.message}`));
            return GLib.SOURCE_REMOVE;
        });
    }

    async _refreshOutputs() {
        const {outputs} = await xrandrOutputs();
        if (!this._destroyed)
            this._outputs = outputs;
    }

    /* Kabel/monitor dicolok, dicabut, atau diatur ulang: bagikan area yang benar. */
    async _checkMonitors() {
        if (this._destroyed || this._starting)
            return;
        await this._refreshOutputs();

        const old = this._shareOutput;
        if (this._daemonKind === 'share' && old) {
            const target = await this._shareTarget();
            const moved = ['name', 'x', 'y', 'width', 'height'].some(k => target[k] !== old[k]);
            if (moved) {
                if (!this._outputs.some(o => o.name === old.name))
                    this._notify(`Monitor ${old.name} terlepas; Tsunagu-Pad pindah ke ${this._outputLabel(target)}.`);
                await this._restart();
            }
        }
        this._sync();
    }

    async _detectState() {
        await this._refreshOutputs();
        this._sync();
        await this._refreshHotspot();
    }

    /* ---------- hotspot ---------- */

    _syncHotspot() {
        this._hotspotSwitch.label.text = this._hotspotStatus || 'Hotspot Tsunagu-Pad (Wi-Fi tetap aktif)';
        this._hotspotSwitch.setSensitive(!this._hotspotBusy);
        if (!this._hotspotBusy)
            this._hotspotSwitch.setToggleState(this._hotspotActive);
        this._hotspotItem.visible = this._hotspotActive && !this._hotspotBusy;
    }

    async _systemHotspotActive() {
        const active = await run(['nmcli', '-t', '-f', 'UUID,TYPE', 'connection', 'show', '--active']);
        for (const line of active.stdout.split('\n')) {
            const [uuid, type] = line.split(':');
            if (type !== '802-11-wireless')
                continue;
            const mode = await run(['nmcli', '-g', '802-11-wireless.mode', 'connection', 'show', uuid]);
            if (mode.stdout.trim() === 'ap')
                return true;
        }
        return false;
    }

    async _hotspotFailureReason(res) {
        const log = await run(['journalctl', '-u', HOTSPOT_UNIT, '-b', '-n', '15', '-o', 'cat', '--no-pager']);
        const lines = log.stdout.split('\n').map(l => l.trim()).filter(l =>
            l && !l.startsWith(`${HOTSPOT_UNIT}:`) && !/^(Starting|Stopping|Stopped|Failed to start) /.test(l));
        return lines.pop() ?? lastLine(res, 'layanan hotspot gagal');
    }

    async _setHotspot(on) {
        if (this._hotspotBusy)
            return;
        this._hotspotBusy = true;
        this._hotspotStatus = on ? 'Menyalakan hotspot…' : 'Mematikan hotspot…';
        this._syncHotspot();
        try {
            if (!GLib.file_test(`/etc/systemd/system/${HOTSPOT_UNIT}`, GLib.FileTest.EXISTS))
                throw new Error('komponen hotspot belum dipasang; jalankan ./install.sh');
            if (on && await this._systemHotspotActive())
                throw new Error('matikan dulu hotspot bawaan GNOME (Pengaturan, lalu Wi-Fi)');
            const res = await run(['systemctl', on ? 'start' : 'stop', HOTSPOT_UNIT]);
            if (!res.ok)
                throw new Error(on ? await this._hotspotFailureReason(res) : lastLine(res, 'gagal mematikan hotspot'));
            /* Alamat laptop untuk iPad berubah, jadi QR sambungan dibuat ulang. */
            if (this._daemonKind === 'share')
                await this._restart();
        } finally {
            this._hotspotBusy = false;
            this._hotspotStatus = '';
            await this._refreshHotspot();
        }
    }

    async _saveHotspotConfig(ssid, password) {
        if (!GLib.file_test(HOTSPOT_HELPER, GLib.FileTest.IS_EXECUTABLE))
            throw new Error('komponen hotspot belum dipasang; jalankan ./install.sh');
        /* Kata sandi lewat stdin agar tidak terlihat di daftar proses. */
        const res = await run(['pkexec', HOTSPOT_HELPER, ssid], `${password}\n`);
        if (!res.ok)
            throw new Error(lastLine(res, 'perubahan dibatalkan'));
        this._hotspotQrText = null;
        await this._refreshHotspot();
        this._notify(`Hotspot disimpan: “${ssid}”.`);
    }

    _refreshHotspot() {
        this._hotspotRefresh ??= this._doRefreshHotspot().finally(() => {
            this._hotspotRefresh = null;
        });
        return this._hotspotRefresh;
    }

    async _doRefreshHotspot() {
        let active = (await run(['systemctl', 'is-active', '--quiet', HOTSPOT_UNIT])).ok;
        if (active && !this._hotspotBusy && await this._systemHotspotActive()) {
            /* Hotspot bawaan GNOME dan ap0 berebut radio yang sama. */
            await run(['systemctl', 'stop', HOTSPOT_UNIT]);
            active = false;
            this._notify('Hotspot Tsunagu-Pad dimatikan karena hotspot bawaan GNOME dinyalakan.');
        }
        if (this._destroyed)
            return;
        this._hotspotActive = active;
        this._syncHotspot();
        if (!active)
            return;

        const config = readHotspotConfig();
        if (!config) {
            this._hotspotQr.visible = false;
            this._hotspotLabel.text = 'Hotspot aktif. Nama & kata sandi tidak bisa dibaca; jalankan ./install.sh.';
            return;
        }
        const text = wifiQrText(config.ssid, config.password);
        if (text !== this._hotspotQrText) {
            const bin = findBinary();
            const qr = bin ? await run([bin, 'qr', text]) : null;
            try {
                this._hotspotQr.setRows(JSON.parse(qr.stdout).rows);
                this._hotspotQrText = text;
            } catch {
                this._hotspotQr.setRows('');
            }
        }
        this._hotspotQr.visible = this._hotspotQrText === text;
        this._hotspotLabel.text = `Wi-Fi “${config.ssid}”\nSandi: ${config.password}`;
    }

    _notify(message) {
        Main.notify('Tsunagu-Pad', message);
    }

    shutdown() {
        this._destroyed = true;
        for (const id of [this._receiverRestartId, this._monitorCheckId, this._pollId]) {
            if (id)
                GLib.Source.remove(id);
        }
        this.menu.disconnect(this._menuOpenId);
        Main.layoutManager.disconnect(this._monitorsChangedId);
        this._daemon?.stop();
        this._daemon = null;
    }
});

const TsunaguPadIndicator = GObject.registerClass(
class TsunaguPadIndicator extends SystemIndicator {
    constructor(extension) {
        super();
        const icon = this._addIndicator();
        icon.iconName = 'video-display-symbolic';
        icon.visible = false;
        this.toggle = new TsunaguPadToggle(extension, icon);
        this.quickSettingsItems.push(this.toggle);
    }
});

export default class TsunaguPadExtension extends Extension {
    enable() {
        this._indicator = new TsunaguPadIndicator(this);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator.toggle.shutdown();
        this._indicator.quickSettingsItems.forEach(item => item.destroy());
        this._indicator.destroy();
        this._indicator = null;
    }
}
