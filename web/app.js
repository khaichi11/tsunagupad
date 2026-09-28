/* Tsunagu-Pad, halaman untuk Safari di iPad.
 * Menampilkan layar Ubuntu lewat WebRTC dan mengirim sentuhan, Apple Pencil,
 * mouse/trackpad, dan keyboard kembali ke Ubuntu. Tanpa library, tanpa build step.
 * SPDX-License-Identifier: MIT
 */
'use strict';

const $ = (id) => document.getElementById(id);
const video = $('video');
const surface = $('surface');
const overlay = $('overlay');
const hud = $('hud');
const kbd = $('kbd');
const toolbar = $('toolbar');

const store = {
  get(key) { try { return localStorage.getItem(key); } catch { return null; } },
  set(key, value) { try { localStorage.setItem(key, value); } catch { /* mode privat */ } },
};

const query = new URLSearchParams(location.search);
const token = query.get('k') || store.get('tsunagupad.token') || '';
if (query.get('k')) store.set('tsunagupad.token', token);

/* ---------- bahasa ---------- */

const STRINGS = {
  en: {
    menu: 'Open the Tsunagu-Pad menu',
    typing: 'Type into Ubuntu',
    keyboard: 'Keyboard',
    fullscreen: 'Full screen',
    info: 'Info',
    reconnect: 'Reconnect',
    tagline: 'REMOTE DISPLAY',
    touch_mouse: 'Touch: Mouse',
    touch_scroll: 'Touch: Scroll',
    touch_off: 'Touch: Off',
    hint: 'Tip: use Share, then Add to Home Screen, so Tsunagu-Pad opens full screen like an app.',
    no_token: 'No access key yet. Scan the QR code from the Tsunagu-Pad panel on Ubuntu.',
    connecting: 'Connecting to Ubuntu...',
    bad_token: 'Wrong access key. Scan the QR code from the Tsunagu-Pad panel again.',
    taken: 'The screen is in use by another device.',
    dropped: 'Disconnected from Ubuntu. Reconnecting...',
    preparing: 'Preparing the video...',
    ice_failed: 'The video could not connect. Make sure the iPad is on the same network as Ubuntu and that the firewall is open (install.sh).',
    tap_to_play: 'Tap to show the Ubuntu screen.',
    retry: 'Try again',
    takeover: 'Take over',
    play: 'Start',
  },
  id: {
    menu: 'Buka menu Tsunagu-Pad',
    typing: 'Ketik ke Ubuntu',
    keyboard: 'Keyboard',
    fullscreen: 'Layar penuh',
    info: 'Info',
    reconnect: 'Sambung ulang',
    tagline: 'LAYAR JARAK JAUH',
    touch_mouse: 'Sentuh: Mouse',
    touch_scroll: 'Sentuh: Gulir',
    touch_off: 'Sentuh: Mati',
    hint: 'Tips: ketuk Bagikan, lalu Tambahkan ke Layar Utama, supaya Tsunagu-Pad terbuka layar penuh seperti aplikasi.',
    no_token: 'Kode akses belum ada. Pindai QR dari panel Tsunagu-Pad di Ubuntu.',
    connecting: 'Menyambung ke Ubuntu...',
    bad_token: 'Kode akses salah. Pindai ulang QR dari panel Tsunagu-Pad.',
    taken: 'Layar sedang dipakai perangkat lain.',
    dropped: 'Terputus dari Ubuntu. Menyambung ulang...',
    preparing: 'Menyiapkan video...',
    ice_failed: 'Video tidak tersambung. Pastikan iPad satu jaringan dengan Ubuntu dan firewall sudah dibuka (install.sh).',
    tap_to_play: 'Ketuk untuk menampilkan layar Ubuntu.',
    retry: 'Coba lagi',
    takeover: 'Ambil alih',
    play: 'Mulai',
  },
};

const LANGS = ['en', 'id'];
let lang = store.get('tsunagupad.lang');
if (!LANGS.includes(lang)) {
  lang = (navigator.language || '').toLowerCase().startsWith('id') ? 'id' : 'en';
}

const t = (key) => STRINGS[lang][key] ?? STRINGS.en[key] ?? key;

function applyLanguage() {
  document.documentElement.lang = lang;
  $('lang-label').textContent = lang.toUpperCase();
  for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  for (const el of document.querySelectorAll('[data-i18n-aria]')) {
    el.setAttribute('aria-label', t(el.dataset.i18nAria));
  }
  updateTouchLabel();
  renderOverlay();
}

let ws = null;
let pc = null;
let info = null;
let chain = Promise.resolve();
let reconnectTimer = 0;
let rtt = 0;

/* ---------- tampilan status ---------- */

let overlayMode = 'wait';   // wait | action | hidden
let overlayState = null;    // { key | text, button }

function renderOverlay() {
  if (!overlayState) return;
  $('status').textContent = overlayState.key ? t(overlayState.key) : overlayState.text;
  $('spinner').hidden = !overlayState.spinner;
  $('btn-action').hidden = !overlayState.button;
  $('btn-action').textContent = overlayState.button ? t(overlayState.button) : '';
  overlay.hidden = false;
  overlayMode = overlayState.button ? 'action' : 'wait';
}

function showKey(key, opts = {}) {
  overlayState = { key, ...opts };
  renderOverlay();
}

function showText(text, opts = {}) {
  overlayState = { text, ...opts };
  renderOverlay();
}

function hideOverlay() {
  overlay.hidden = true;
  overlayMode = 'hidden';
}

const standalone = navigator.standalone ||
  matchMedia('(display-mode: fullscreen), (display-mode: standalone)').matches;
$('hint').hidden = standalone;

/* ---------- sambungan ---------- */

function send(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

function connect() {
  clearTimeout(reconnectTimer);
  if (!token) {
    showKey('no_token');
    return;
  }
  if (ws) {
    ws.onclose = null;
    ws.close();
  }
  closePeer();
  showKey('connecting', { spinner: true });

  const sock = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ws = sock;
  sock.onopen = () => send({ type: 'hello', token });
  sock.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    chain = chain.then(() => onSignal(msg)).catch((err) => console.error(err));
  };
  sock.onclose = (ev) => {
    if (ws !== sock) return;
    ws = null;
    closePeer();
    if (ev.code === 4001) {
      showKey('bad_token', { button: 'retry' });
    } else if (ev.code === 4000) {
      showKey('taken', { button: 'takeover' });
    } else {
      showKey('dropped', { spinner: true });
      reconnectTimer = setTimeout(connect, 1500);
    }
  };
}

async function onSignal(msg) {
  switch (msg.type) {
    case 'welcome':
      info = msg;
      showKey('preparing', { spinner: true });
      break;
    case 'offer':
      await acceptOffer(msg.sdp);
      break;
    case 'ice':
      if (pc && msg.candidate) {
        await pc.addIceCandidate({ candidate: msg.candidate, sdpMLineIndex: msg.sdpMLineIndex })
          .catch(() => { /* kandidat yang tidak cocok diabaikan */ });
      }
      break;
    case 'pong':
      rtt = performance.now() - msg.t;
      break;
    case 'error':
      showText(msg.message, { button: 'retry' });
      break;
  }
}

async function acceptOffer(sdp) {
  closePeer();
  const peer = new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle' });
  pc = peer;

  peer.ontrack = (ev) => {
    // Minta browser menampilkan frame secepatnya, tanpa buffer tambahan.
    for (const key of ['jitterBufferTarget', 'playoutDelayHint']) {
      try { ev.receiver[key] = 0; } catch { /* tidak didukung */ }
    }
    video.srcObject = ev.streams[0] || new MediaStream([ev.track]);
    playVideo();
  };
  peer.onicecandidate = (ev) => {
    if (ev.candidate && ev.candidate.candidate) {
      send({ type: 'ice', candidate: ev.candidate.candidate, sdpMLineIndex: ev.candidate.sdpMLineIndex ?? 0 });
    }
  };
  peer.onconnectionstatechange = () => {
    if (pc === peer && peer.connectionState === 'failed') showKey('ice_failed', { button: 'retry' });
  };

  await peer.setRemoteDescription({ type: 'offer', sdp });
  await peer.setLocalDescription(await peer.createAnswer());
  send({ type: 'answer', sdp: peer.localDescription.sdp });
}

function closePeer() {
  if (pc) {
    pc.ontrack = pc.onicecandidate = pc.onconnectionstatechange = null;
    pc.close();
    pc = null;
  }
  video.srcObject = null;
  lastStats = null;
}

function playVideo() {
  video.play().catch(() => showKey('tap_to_play', { button: 'play' }));
}

video.addEventListener('playing', () => {
  if (overlayMode === 'wait') hideOverlay();
});

$('btn-action').addEventListener('click', () => {
  if (ws && ws.readyState === WebSocket.OPEN && video.srcObject && video.paused) playVideo();
  else connect();
});

/* ---------- statistik & deteksi macet ---------- */

let lastStats = null;
let frozenTicks = 0;
let statsTick = 0;

async function pollStats() {
  if (!pc) return;
  let inbound = null;
  (await pc.getStats()).forEach((s) => {
    if (s.type === 'inbound-rtp' && s.kind === 'video') inbound = s;
  });
  if (!inbound) return;

  if (lastStats) {
    const dt = (inbound.timestamp - lastStats.timestamp) / 1000 || 1;
    const frames = (inbound.framesDecoded || 0) - (lastStats.framesDecoded || 0);
    const emitted = (inbound.jitterBufferEmittedCount || 0) - (lastStats.jitterBufferEmittedCount || 0);
    const fps = frames / dt;
    const mbps = ((inbound.bytesReceived || 0) - (lastStats.bytesReceived || 0)) * 8 / 1e6 / dt;
    const buffer = emitted > 0
      ? ((inbound.jitterBufferDelay || 0) - (lastStats.jitterBufferDelay || 0)) / emitted * 1000 : 0;
    const decode = frames > 0
      ? ((inbound.totalDecodeTime || 0) - (lastStats.totalDecodeTime || 0)) / frames * 1000 : 0;
    const delay = buffer + decode + rtt;

    hud.textContent = `${fps.toFixed(0)} fps · ±${delay.toFixed(0)} ms · ${mbps.toFixed(1)} Mbps · ${info?.encoder ?? ''}`;

    if (frames > 0) {
      frozenTicks = 0;
      if (overlayMode === 'wait' && !video.paused) hideOverlay();
    } else if (pc.connectionState === 'connected' && ++frozenTicks === 2) {
      send({ type: 'keyframe' });
    }
    if (++statsTick % 2 === 0) send({ type: 'stats', fps, delay });
  }
  lastStats = inbound;
}

setInterval(() => { pollStats().catch(() => {}); }, 1000);
setInterval(() => send({ type: 'ping', t: performance.now() }), 2000);

/* ---------- koordinat ---------- */

const round = (v) => Math.round(v * 10000) / 10000;
const clamp01 = (v) => Math.min(1, Math.max(0, v));

// Posisi layar iPad ke 0..1 di dalam gambar video (memperhitungkan bingkai hitam).
function norm(clientX, clientY) {
  const r = video.getBoundingClientRect();
  const vw = video.videoWidth || info?.width || r.width;
  const vh = video.videoHeight || info?.height || r.height;
  const scale = Math.min(r.width / vw, r.height / vh);
  const w = vw * scale;
  const h = vh * scale;
  return [
    round(clamp01((clientX - r.left - (r.width - w) / 2) / w)),
    round(clamp01((clientY - r.top - (r.height - h) / 2) / h)),
  ];
}

const canControl = () => info?.input && ws && ws.readyState === WebSocket.OPEN;

/* ---------- Apple Pencil ---------- */

let penDown = false;
let penLastUse = 0;

function sendPen(kind, events) {
  const pts = events.map((e) => {
    const [x, y] = norm(e.clientX, e.clientY);
    return [x, y, round(e.pressure || 0), Math.round(e.tiltX || 0), Math.round(e.tiltY || 0)];
  });
  send({ type: 'pen', e: kind, pts });
  penLastUse = performance.now();
}

/* ---------- mouse & trackpad ---------- */

const BUTTONS = { 0: 1, 1: 2, 2: 3 };

function sendMouseAt(kind, clientX, clientY, button) {
  const [x, y] = norm(clientX, clientY);
  const msg = { type: 'mouse', e: kind, x, y };
  if (button) msg.b = button;
  send(msg);
}

surface.addEventListener('wheel', (e) => {
  e.preventDefault();
  if (!canControl()) return;
  const unit = e.deltaMode === 1 ? 1 / 3 : e.deltaMode === 2 ? 3 : 1 / 60;
  const [x, y] = norm(e.clientX, e.clientY);
  send({ type: 'scroll', dx: e.deltaX * unit, dy: e.deltaY * unit, x, y });
}, { passive: false });

/* ---------- sentuhan jari ---------- */

const TOUCH_MODES = ['mouse', 'scroll', 'off'];
const TAP_SLOP = 10;
const LONG_PRESS_MS = 550;
const SCROLL_STEP = 40;   // piksel jari per satu klik roda

let touchMode = TOUCH_MODES.includes(store.get('tsunagupad.touch'))
  ? store.get('tsunagupad.touch') : 'mouse';
const touches = new Map();
let gesture = null;

function centroid() {
  let x = 0;
  let y = 0;
  for (const t of touches.values()) { x += t.x; y += t.y; }
  return { x: x / touches.size, y: y / touches.size };
}

function cancelTouches() {
  if (gesture) {
    clearTimeout(gesture.timer);
    if (gesture.kind === 'drag') sendMouseAt('up', gesture.lastX, gesture.lastY, 1);
  }
  touches.clear();
  gesture = null;
}

function longPress() {
  if (gesture?.kind === 'pending' && touchMode === 'mouse') {
    gesture.kind = 'long';
    sendMouseAt('click', gesture.x0, gesture.y0, 3);
  }
}

function touchDown(e) {
  // Tolak telapak tangan saat sedang menulis dengan Pencil.
  if (touchMode === 'off' || penDown || performance.now() - penLastUse < 500) return;
  touches.set(e.pointerId, { x: e.clientX, y: e.clientY });

  if (touches.size === 1) {
    gesture = {
      kind: 'pending', x0: e.clientX, y0: e.clientY, lastX: e.clientX, lastY: e.clientY,
      timer: setTimeout(longPress, LONG_PRESS_MS),
    };
  } else if (touches.size === 2) {
    if (gesture?.kind === 'drag') sendMouseAt('up', gesture.lastX, gesture.lastY, 1);
    clearTimeout(gesture?.timer);
    const c = centroid();
    gesture = { kind: 'two', sx: c.x, sy: c.y, cx: c.x, cy: c.y, moved: false, t0: performance.now() };
  }
}

function scrollBy(dx, dy, at) {
  const [x, y] = norm(at.x, at.y);
  send({ type: 'scroll', dx: -dx / SCROLL_STEP, dy: -dy / SCROLL_STEP, x, y });
}

function touchMove(e) {
  const t = touches.get(e.pointerId);
  if (!t || !gesture) return;
  t.x = e.clientX;
  t.y = e.clientY;

  if (gesture.kind === 'two') {
    const c = centroid();
    if (Math.hypot(c.x - gesture.sx, c.y - gesture.sy) > TAP_SLOP) gesture.moved = true;
    if (gesture.moved) {
      scrollBy(c.x - gesture.cx, c.y - gesture.cy, c);
      gesture.cx = c.x;
      gesture.cy = c.y;
    }
    return;
  }

  if (gesture.kind === 'pending' && Math.hypot(e.clientX - gesture.x0, e.clientY - gesture.y0) > TAP_SLOP) {
    clearTimeout(gesture.timer);
    if (touchMode === 'mouse') {
      gesture.kind = 'drag';
      sendMouseAt('down', gesture.x0, gesture.y0, 1);
    } else {
      gesture.kind = 'scroll';
    }
  }
  if (gesture.kind === 'drag') {
    sendMouseAt('move', e.clientX, e.clientY);
  } else if (gesture.kind === 'scroll') {
    scrollBy(e.clientX - gesture.lastX, e.clientY - gesture.lastY, { x: e.clientX, y: e.clientY });
  }
  gesture.lastX = e.clientX;
  gesture.lastY = e.clientY;
}

function touchUp(e) {
  if (!touches.delete(e.pointerId) || !gesture) return;
  const g = gesture;

  if (g.kind === 'two') {
    if (touches.size === 0) {
      // Ketuk dua jari = klik kanan.
      if (!g.moved && performance.now() - g.t0 < 350 && touchMode === 'mouse') sendMouseAt('click', g.cx, g.cy, 3);
      gesture = null;
    }
    return;
  }
  clearTimeout(g.timer);
  if (g.kind === 'pending' && touchMode === 'mouse' && e.type === 'pointerup') sendMouseAt('click', g.x0, g.y0, 1);
  else if (g.kind === 'drag') sendMouseAt('up', e.clientX, e.clientY, 1);
  gesture = null;
}

/* ---------- pointer events (Pencil, mouse, jari) ---------- */

surface.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  collapseToolbar();
  if (!canControl()) return;

  if (e.pointerType === 'pen') {
    cancelTouches();
    penDown = true;
    surface.setPointerCapture(e.pointerId);
    sendPen('down', [e]);
  } else if (e.pointerType === 'mouse') {
    surface.setPointerCapture(e.pointerId);
    sendMouseAt('down', e.clientX, e.clientY, BUTTONS[e.button] ?? 1);
  } else {
    touchDown(e);
  }
});

surface.addEventListener('pointermove', (e) => {
  if (!canControl()) return;
  if (e.pointerType === 'pen') {
    // Pencil mengirim sekitar 240 titik per detik; ambil semuanya agar tulisan halus.
    if (penDown) sendPen('move', e.getCoalescedEvents?.().length ? e.getCoalescedEvents() : [e]);
    else sendPen('hover', [e]);   // iPad dengan fitur hover Pencil
  } else if (e.pointerType === 'mouse') {
    sendMouseAt('move', e.clientX, e.clientY);
  } else {
    touchMove(e);
  }
});

function endPointer(e) {
  if (e.pointerType === 'pen') {
    if (penDown) {
      penDown = false;
      sendPen('up', [e]);
    }
  } else if (e.pointerType === 'mouse') {
    sendMouseAt('up', e.clientX, e.clientY, BUTTONS[e.button] ?? 1);
  } else {
    touchUp(e);
  }
}

surface.addEventListener('pointerup', endPointer);
surface.addEventListener('pointercancel', endPointer);
surface.addEventListener('pointerleave', (e) => {
  if (e.pointerType === 'pen' && !penDown) sendPen('leave', []);
});

document.addEventListener('gesturestart', (e) => e.preventDefault());
document.addEventListener('dblclick', (e) => e.preventDefault());

/* ---------- keyboard ---------- */

const sentKeys = new Set();
const KBD_SENTINEL = ' ';   // agar tombol hapus tetap memicu event walau kolom "kosong"

function sendKey(kind, code, key) {
  send({ type: 'key', e: kind, code, key });
}

document.addEventListener('keydown', (e) => {
  if (!canControl()) return;
  const typing = document.activeElement === kbd;
  const named = e.key && e.key.length > 1 && !['Unidentified', 'Process', 'Dead'].includes(e.key);
  const combo = e.ctrlKey || e.metaKey || e.altKey;

  // Huruf biasa dari keyboard layar diketik lewat event "input" di bawah.
  if (typing && !named && !combo) return;
  if (!e.code && !named) return;

  e.preventDefault();
  sentKeys.add(e.code || e.key);
  sendKey('down', e.code || '', e.key || '');
});

document.addEventListener('keyup', (e) => {
  if (!sentKeys.delete(e.code || e.key)) return;
  e.preventDefault();
  sendKey('up', e.code || '', e.key || '');
});

let composing = false;

function resetKbd() {
  kbd.value = KBD_SENTINEL;
  kbd.setSelectionRange(kbd.value.length, kbd.value.length);
}

function tapKey(code) {
  sendKey('down', code, code);
  sendKey('up', code, code);
}

kbd.addEventListener('compositionstart', () => { composing = true; });
kbd.addEventListener('compositionend', (e) => {
  composing = false;
  if (e.data) send({ type: 'text', s: e.data });
  resetKbd();
});
kbd.addEventListener('input', (e) => {
  if (composing || e.isComposing) return;
  if ((e.inputType === 'insertText' || e.inputType === 'insertReplacementText') && e.data) {
    send({ type: 'text', s: e.data });
  } else if (e.inputType === 'insertLineBreak' || e.inputType === 'insertParagraph') {
    tapKey('Enter');
  } else if (e.inputType === 'deleteContentBackward') {
    tapKey('Backspace');
  }
  resetKbd();
});

/* ---------- toolbar ---------- */

let toolbarTimer = 0;

function collapseToolbar() {
  clearTimeout(toolbarTimer);
  toolbar.classList.remove('open');
}

function keepToolbarOpen() {
  clearTimeout(toolbarTimer);
  toolbarTimer = setTimeout(collapseToolbar, 5000);
}

$('handle').addEventListener('click', () => {
  toolbar.classList.toggle('open');
  if (toolbar.classList.contains('open')) keepToolbarOpen();
});

$('btn-kbd').addEventListener('click', () => {
  if (document.activeElement === kbd) {
    kbd.blur();
  } else {
    resetKbd();
    kbd.focus();
  }
  keepToolbarOpen();
});

function updateTouchLabel() {
  $('touch-label').textContent = t(`touch_${touchMode}`);
}

$('btn-touch').addEventListener('click', () => {
  touchMode = TOUCH_MODES[(TOUCH_MODES.indexOf(touchMode) + 1) % TOUCH_MODES.length];
  store.set('tsunagupad.touch', touchMode);
  cancelTouches();
  updateTouchLabel();
  keepToolbarOpen();
});

$('btn-lang').addEventListener('click', () => {
  lang = LANGS[(LANGS.indexOf(lang) + 1) % LANGS.length];
  store.set('tsunagupad.lang', lang);
  applyLanguage();
  keepToolbarOpen();
});

const root = document.documentElement;
const requestFs = root.requestFullscreen || root.webkitRequestFullscreen;
const exitFs = document.exitFullscreen || document.webkitExitFullscreen;
$('btn-full').hidden = standalone || !requestFs;
$('btn-full').addEventListener('click', () => {
  if (document.fullscreenElement || document.webkitFullscreenElement) exitFs.call(document);
  else requestFs.call(root);
  collapseToolbar();
});

hud.hidden = store.get('tsunagupad.hud') !== '1';
$('btn-info').addEventListener('click', () => {
  hud.hidden = !hud.hidden;
  store.set('tsunagupad.hud', hud.hidden ? '0' : '1');
  keepToolbarOpen();
});

$('btn-reconnect').addEventListener('click', () => {
  collapseToolbar();
  connect();
});

/* ---------- mulai ---------- */

document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  const healthy = ws && ws.readyState === WebSocket.OPEN && pc && pc.connectionState === 'connected';
  if (!healthy) connect();
});

applyLanguage();
connect();
