// app.js - The page. The application lives in cwtty.wasm, compiled from the
// C sources of the terminal app (see wasm/page.c): this draws its state and
// feeds it with the keys and with what the audio worklet reports.

import * as audio from './audio.js';
import { offerInstall } from './install.js';

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const el = (tag, className = '') => Object.assign(document.createElement(tag), { className });
const make = (tag, className, ...children) => {
  const node = el(tag, className);
  node.append(...children);
  return node;
};

// ---- The C core ----

const wasm = await (await fetch('cwtty.wasm')).arrayBuffer();
const w = (await WebAssembly.instantiate(wasm, { math: Math })).instance.exports;
const utf8 = new TextDecoder();
const encoder = new TextEncoder();
const heap = () => new Uint8Array(w.memory.buffer);

function str(p) {
  const m = heap();
  let end = p;
  while (m[end]) end++;
  return utf8.decode(m.subarray(p, end));
}

function cstr(s) {
  const p = w.cw_buf(), { written } = encoder.encodeInto(s, heap().subarray(p, p + 1023));
  heap()[p + written] = 0;
  return p;
}

const get = (key) => w.cw_get(cstr(key));
const set = (key, value) => w.cw_set(cstr(key), value);

// The enumerations of wasm/page.c and src/training.h.
const MODES = ['tx', 'rx', 'trx', 'train', 'stats'];
const RX = 0, TX = 1;
const CHAR = 0, PROSIGN = 1, SPACE = 3, LINE = 4, KEY_UP = 5, KEY_DOWN = 6;
const SETUP = 0, LISTEN = 1, RESULT = 2, SEND = 3;
const PLAY = 1, WRONG = 4, KOCH = 8, CHECKED = 16;
const OFF = 0, MIC = 1, DEMO = 2;

const FMIN = 250, FMAX = 1250; // the waterfall, as in src/mode_rx.c
const HEAT = [[14, 15, 17], [42, 31, 5], [110, 78, 0], [212, 156, 0], [255, 213, 74], [255, 246, 214]];
const COLORS = { accent: '#ffc61a', accentLo: '#5c4500', muted: '#9aa1ab', rx: '#d9e2eb' };

function names(name) {
  const list = [];
  for (let i = 0, p; (p = name(i)); i++) list.push([i, str(p)]);
  return list;
}

const BANDWIDTHS = [];
for (let i = 0, hz; (hz = w.cw_bandwidth(i)); i++) BANDWIDTHS.push(hz);

const NAMES = {
  keyer: names(w.cw_keyer_name),
  charset: names(w.cw_charset_name),
  content: names(w.cw_content_name),
  bandwidth: BANDWIDTHS.map((hz) => [hz, `${hz} Hz`]),
};

const TABLE = Array.from({ length: w.cw_table_count() }, (_, i) => ({
  text: str(w.cw_table_text(i)),
  code: str(w.cw_table_code(i)),
  group: w.cw_table_group(i),
  name: str(w.cw_table_name(i)),
}));
const BY_TEXT = new Map(TABLE.map((entry) => [entry.text, entry]));

// ---- State ----

let mode = 'tx';
let source = MIC; // what the receiver listens to when it is on
let listening = false;
let report = { clock: 0, busy: 0, idle: 0, playing: 0 };
let reportAt = performance.now();
let keyingOn;
let view;
let trainState;
const held = new Set(); // inputs down: 0 left paddle, 1 right paddle, 2 straight key
const edges = { tx: [], rx: [] }; // key transitions [ms, down], for the tapes
let lastTx = { text: '', at: -Infinity };
let micFailed = false;
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

// ---- Settings ----

const KEYS = [...new Set($$('[data-key]').map((e) => e.dataset.key))];

function save() {
  try {
    localStorage.cwtty = JSON.stringify({ mode, source, settings: Object.fromEntries(KEYS.map((k) => [k, get(k)])) });
  } catch {} // private mode: nothing is remembered
}

function restore() {
  try {
    const saved = JSON.parse(localStorage.cwtty);
    for (const [key, value] of Object.entries(saved.settings)) set(key, value);
    return saved;
  } catch {
    return {};
  }
}

// A segmented control shows its value: the button pressed, the thumb under it.
function segment(seg, value) {
  const buttons = [...seg.querySelectorAll('button')], i = buttons.findIndex((b) => +b.dataset.value === value);
  buttons.forEach((b, k) => b.setAttribute('aria-pressed', k === i));
  seg.style.setProperty('--n', buttons.length);
  seg.style.setProperty('--i', Math.max(i, 0));
  seg.style.setProperty('--shown', i < 0 ? 0 : 1);
}

// Shows the settings in every control bound to them (or only to key).
function refresh(key) {
  $('[data-key="farnsworth"]').max = get('wpm') - 1;
  for (const control of $$(key ? `[data-key="${key}"]` : '[data-key]')) {
    const value = get(control.dataset.key), out = control.nextElementSibling;
    if (control.classList.contains('seg')) {
      segment(control, value);
      continue;
    }
    if (control.type === 'checkbox') control.checked = value > 0;
    else control.value = value;
    if (out?.tagName === 'OUTPUT') out.value = 'off' in control.dataset && !value ? 'Off' : value + (control.dataset.unit ?? '');
  }
}

function changed() {
  refresh();
  $('#speed').textContent = `${get('wpm')} WPM  ${get('tone_hz')} Hz  ${NAMES.keyer[get('keyer_mode')][1]}`;
  const swap = get('swap_paddles');
  $('[data-input="0"] span').textContent = swap ? 'Dah' : 'Dit';
  $('[data-input="1"] span').textContent = swap ? 'Dit' : 'Dah';
  audio.send('params', get('tone_hz'), get('wpm'), get('keyer_mode'), w.cw_sidetone(), get('rise_ms'));
  save();
}

// ---- Training statistics, kept in the browser as the text form of src/stats.c ----

const STATS_KEY = 'cwtty-stats';
let statsSerial = 0; // changes with the statistics: the view redraws

function loadStats(text) {
  if (typeof text !== 'string') return;
  const p = w.cw_stats_buf(), cap = w.cw_stats_cap();
  const { written } = encoder.encodeInto(text, heap().subarray(p, p + cap - 1));
  heap()[p + written] = 0;
  w.cw_stats_load();
  statsSerial++;
}

function saveStats() {
  statsSerial++;
  const p = w.cw_stats_save();
  try {
    if (p) localStorage[STATS_KEY] = str(p);
  } catch {} // private mode: kept until the page closes
}

// Local day number, as in src/stats.h: days since 1970-01-01.
function today() {
  const d = new Date();
  return Math.round(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / 86400000);
}

const dateOf = (day) =>
  day > 0 ? new Date(day * 86400000).toLocaleDateString('en-GB', { timeZone: 'UTC', day: 'numeric', month: 'short', year: 'numeric' }) : '';

const stat = (key) => w.cw_stat(cstr(key));

// ---- Modes and the receiver's input ----

async function setMode(name) {
  // Training again, from a session: back to its setup (the session can be resumed).
  if (name === mode && name === 'train') {
    if (view !== 'train-setup') actions.setup();
    return;
  }
  releaseAll();
  audio.send('stop');
  mode = name;
  w.cw_mode(MODES.indexOf(name));
  for (const button of $$('nav [data-mode]')) button.setAttribute('aria-current', button.dataset.mode === name);
  moveIndicator();
  $('#log-info').textContent = '';
  changed();
  await listen();
}

// The pill under the active tab slides to it (a smaller one behind the icon on phones).
function moveIndicator() {
  const button = $(`nav [data-mode="${mode}"]`), pill = $('nav .indicator');
  if (!button) return;
  const bar = getComputedStyle($('nav')).position === 'fixed', width = bar ? Math.min(button.offsetWidth - 12, 64) : button.offsetWidth;
  pill.style.setProperty('--w', `${width}px`);
  pill.style.setProperty('--x', `${button.offsetLeft + (button.offsetWidth - width) / 2}px`);
}

async function listen() {
  const want = mode === 'rx' || mode === 'trx' ? source : OFF;
  const ok = await audio.microphone(want === MIC);
  micFailed = want === MIC && !ok;
  // An empty transcript explains it; a full one gets a toast.
  if (micFailed && log()?.childElementCount) toast('No microphone: allow it, or listen to the demo band', true);
  listening = ok && want !== OFF;
  w.cw_source(listening ? want : OFF);
  $('#in').classList.toggle('used', want !== OFF);
  segment($('#source'), source);
}

async function useSource(s) {
  source = s;
  save();
  await listen();
}

// ---- Keying ----

function press(input, down) {
  if (down === held.has(input)) return;
  if (down) enable(); // the worklet must agree before the first element
  const contact = w.cw_press(input, +down);
  if (contact < 0) return;
  if (down) held.add(input);
  else held.delete(input);
  audio.send('key', contact, +down);
}

function releaseAll() {
  for (const input of held) press(input, false);
}

// The operator may key: the screen takes it and no modal window is open.
function enable() {
  const on = !!w.cw_keying() && !$('dialog:modal');
  if (on === keyingOn) return;
  keyingOn = on;
  if (!on) held.clear(); // the worklet releases its contacts
  audio.send('enable', +on);
}

function play(text, leadMs) {
  audio.send('stop');
  audio.send('play', encoder.encode(text), get('wpm'), get('farnsworth'), leadMs);
}

// Carries out what a training call asks of the page.
function train(fx) {
  const send = get('train_dir');
  if (fx & KOCH)
    toast(`${Math.floor((w.cw_train_correct() * 100) / w.cw_train_total())}% correct: ready for Koch lesson ${get('train_koch') + 1}`);
  if (fx & WRONG) {
    if (send) releaseAll();
    restart($('#stage'), 'shake');
    toast(send ? 'Not quite: start sending again from the beginning' : 'Not quite: listen and start again', true);
  }
  if (fx & PLAY) play(str(w.cw_train_target()), 350);
  if (fx & CHECKED) saveStats();
}

// ---- What the worklet reports ----

function tape(list, t, down) {
  list.push([t, down]);
  if (list.length > 600) list.splice(0, 300);
}

function onReport(r) {
  report = r;
  reportAt = performance.now();
  for (let i = 0; i < r.events.length; i += 3) {
    // Playback is not shown: in training it would give the answer away.
    if (r.events[i + 2] !== 0) continue;
    w.cw_tx_event(r.events[i], r.events[i + 1]);
    tape(edges.tx, r.events[i], !!r.events[i + 1]);
  }
  for (let i = 0, cap = w.cw_rx_cap(); i < r.rx.length; i += cap) {
    const chunk = r.rx.subarray(i, i + cap);
    new Float32Array(w.memory.buffer, w.cw_rx_buf(), chunk.length).set(chunk);
    w.cw_rx(chunk.length);
  }
  w.cw_demo(r.clock);
  train(w.cw_update(r.clock, r.busy, r.idle));
  for (let p; (p = w.cw_pop()); ) output(w.cw_pop_dir(), w.cw_pop_kind(), str(p));
}

function output(dir, kind, text) {
  if (kind === KEY_UP || kind === KEY_DOWN) {
    tape(edges.rx, w.cw_pop_time(), kind === KEY_DOWN);
    return;
  }
  if (dir === TX && kind !== SPACE) lastTx = { text, at: performance.now() };
  if (mode === 'trx' || (mode === 'tx' && dir === TX) || (mode === 'rx' && dir === RX)) write($(`#log-${mode}`), dir, kind, text);
}

// Appends to the transcript, one line per transmission and direction.
function write(log, dir, kind, text) {
  const atEnd = log.scrollHeight - log.scrollTop - log.clientHeight < 32;
  const name = dir === TX ? 'tx' : 'rx';
  let line = log.lastElementChild;
  if (kind === LINE) {
    line?.setAttribute('data-closed', '');
    return;
  }
  if (!line || line.dataset.dir !== name || line.hasAttribute('data-closed')) {
    if (kind === SPACE) return; // never start a line with a space
    line = log.appendChild(el('div'));
    line.dataset.dir = name;
    if (log.childElementCount > 500) log.firstElementChild.remove();
  }
  if (kind === SPACE) {
    if (line.lastChild && line.lastChild.textContent !== ' ') line.append(' ');
  } else if (kind === CHAR) {
    const char = make('span', 'in', text);
    char.addEventListener('animationend', () => char.replaceWith(text), { once: true });
    line.append(char);
  } else {
    line.append(glyphs(el('span', kind === PROSIGN ? '' : 'unknown'), text));
  }
  if (atEnd) log.scrollTop = log.scrollHeight;
}

// ---- Drawing helpers ----

function setText(node, text) {
  if (node.textContent !== text) node.textContent = text;
}

// Plays a CSS animation again.
function restart(node, className) {
  node.classList.remove(className);
  void node.offsetWidth;
  node.classList.add(className);
}

const icon = (name) => {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'), use = document.createElementNS(svg.namespaceURI, 'use');
  svg.setAttribute('class', 'icon');
  use.setAttribute('href', `#i-${name}`);
  svg.append(use);
  return svg;
};

// Text with the prosigns ("<AR>") as overlined letters.
function glyphs(node, text) {
  if (node.dataset.text === text) return node;
  node.dataset.text = text;
  node.replaceChildren(
    ...text.split(/(<\w+>)/).filter(Boolean).map((part) =>
      /^<\w+>$/.test(part) ? Object.assign(el('span', 'prosign'), { textContent: part.slice(1, -1) }) : part));
  return node;
}

// Codes ".- -..." as dots and bars.
function pattern(node, codes) {
  if (node.dataset.codes === codes) return node;
  node.dataset.codes = codes;
  node.replaceChildren(
    ...codes.split(' ').filter(Boolean).map((code) => {
      const char = el('span');
      for (const c of code) char.append(el('i', c === '-' ? 'dah' : ''));
      return char;
    }));
  return node;
}

const codes = (text) => str(w.cw_codes(cstr(text)));

function label(button, text, key) {
  if (button.dataset.label === text) return;
  button.dataset.label = text;
  button.replaceChildren(`${text} `, Object.assign(el('kbd'), { textContent: key }));
}

// Milliseconds per device pixel on the tapes: a dit is about 12 CSS pixels.
const msPerPx = (ditMs) => Math.min(Math.max(ditMs / 12, 1), 25) / devicePixelRatio;

// Clocks advance between two reports.
const since = () => (audio.running() ? Math.min(performance.now() - reportAt, 60) : 0);

function drawTape(canvas, list, now, ms, color) {
  const g = canvas.getContext('2d'), { width, height } = canvas, t0 = now - width * ms;
  const mark = (a, b) => {
    if (b > t0) g.fillRect((Math.max(a, t0) - t0) / ms, height * 0.25, Math.max(1, (b - Math.max(a, t0)) / ms), height * 0.5);
  };
  let start = list.length && !list[0][1] ? t0 : null;
  g.clearRect(0, 0, width, height);
  g.fillStyle = color;
  for (const [t, down] of list) {
    if (down) {
      start ??= t;
    } else if (start !== null) {
      mark(start, t);
      start = null;
    }
  }
  if (start !== null) mark(start, now);
}

function drawEnvelope(canvas, now, ms) {
  const g = canvas.getContext('2d'), { width, height } = canvas;
  const noise = w.cw_rx_noise(), threshold = w.cw_rx_threshold(), open = w.cw_rx_open();
  const range = Math.max(w.cw_rx_peak() - noise + 4, 12);
  g.clearRect(0, 0, width, height);
  for (let x = 0; x < width; x++) {
    const t = now - (width - x) * ms, db = w.cw_rx_env(t, t + ms), v = (db - noise + 2) / range;
    if (db < -150 || v <= 0.02) continue;
    g.fillStyle = db >= threshold ? (open ? COLORS.accent : COLORS.muted) : COLORS.accentLo;
    g.fillRect(x, height * (1 - Math.min(v, 1)), 1, height * Math.min(v, 1));
  }
}

// The loudest bin of a waterfall column, interpolated when narrower than a bin.
function column(row, f0, f1, binHz) {
  const k0 = Math.max(Math.floor(f0 / binHz), 0), k1 = Math.min(Math.ceil(f1 / binHz), row.length - 1);
  if (k1 <= k0) {
    const fk = (0.5 * (f0 + f1)) / binHz, a = Math.min(Math.max(fk | 0, 0), row.length - 2), t = fk - a;
    return row[a] * (1 - t) + row[a + 1] * t;
  }
  let best = -200;
  for (let k = k0; k <= k1; k++) best = Math.max(best, row[k]);
  return best;
}

function heat(v, px, i) {
  const x = Math.min(Math.max(v, 0), 1) * 5, k = Math.min(x | 0, 4), f = x - k, a = HEAT[k], b = HEAT[k + 1];
  for (let c = 0; c < 3; c++) px[i + c] = a[c] + (b[c] - a[c]) * f;
  px[i + 3] = 255;
}

let wfRows = 0;

// Scrolls the waterfall down by the rows produced since the last call.
function drawWaterfall(all = false) {
  const canvas = $('#wf'), g = canvas.getContext('2d'), rows = w.cw_rx_rows();
  const n = all ? canvas.height : Math.min(rows - wfRows, canvas.height);
  wfRows = rows;
  if (n <= 0 || !canvas.width) return;
  if (!all) g.drawImage(canvas, 0, n);
  const image = g.createImageData(canvas.width, 1), floor = w.cw_rx_floor(), binHz = w.cw_rx_bin_hz();
  const df = (FMAX - FMIN) / canvas.width;
  for (let age = 0; age < n; age++) {
    const p = w.cw_rx_row(age);
    if (!p) {
      g.clearRect(0, age, canvas.width, 1);
      continue;
    }
    const row = new Float32Array(w.memory.buffer, p, w.cw_rx_bins());
    for (let x = 0; x < canvas.width; x++) {
      const f0 = FMIN + x * df;
      heat((column(row, f0, f0 + df, binHz) - floor - 3) / 32, image.data, 4 * x);
    }
    g.putImageData(image, 0, age);
  }
}

function resize(entries) {
  for (const { target: canvas } of entries) {
    const width = Math.round(canvas.clientWidth * devicePixelRatio);
    if (!width || canvas.width === width) continue;
    canvas.width = width;
    if (canvas.id === 'wf') drawWaterfall(true);
    else canvas.height = Math.round(canvas.clientHeight * devicePixelRatio);
  }
}

// ---- Rendering ----

function currentView() {
  if (mode !== 'train') return mode;
  if (w.cw_train_state() === SETUP) return 'train-setup';
  return get('train_dir') ? 'train-send' : 'train-copy';
}

function frame() {
  requestAnimationFrame(frame);
  w.cw_clock(performance.now(), today());
  const v = currentView();
  if (v !== view) view = document.body.dataset.view = v;
  enable();
  $('#out').classList.toggle('on', audio.running());
  $('#in').classList.toggle('on', listening);
  if (v === 'tx' || v === 'rx' || v === 'trx') renderEmpty();
  if (v === 'tx' || v === 'trx' || v === 'train-send') renderKeying();
  if (v === 'rx' || v === 'trx') renderSignal();
  if (v.startsWith('train')) renderTraining(v);
  if (v === 'stats') renderStats();
  if ($('#table-dialog').open) renderTable();
  if (v === 'trx') setText($('#log-info'), $('#log-trx').childElementCount ? 'QSO log' : '');
}

// What to do first, while the transcript is empty.
function renderEmpty() {
  const box = $('#log-empty'), empty = !log().childElementCount;
  box.classList.toggle('gone', !empty);
  $('#log [data-act="clear"]').hidden = empty;
  if (!empty) return;
  const input = get('key_input'), touch = matchMedia('(pointer: coarse)').matches;
  const keys = input === 1 ? 'Key with your USB paddle.' : input === 2 ? 'Key with your USB straight key.'
             : touch ? 'Hold Key below, or tap Dit and Dah.' : 'Hold Space for a straight key, or use ← → as paddles.';
  let name = mode, title, text, buttons = [];
  if (mode === 'tx') {
    title = 'Start keying';
    text = `${keys} What you send is decoded here.`;
  } else if (!listening && source === MIC) {
    name = 'mic';
    title = micFailed ? 'No microphone' : 'Waiting for the microphone';
    text = micFailed ? 'Allow the microphone for this page, or listen to the demo band to see how decoding works.'
                     : 'Allow it when the browser asks.';
    buttons = micFailed ? [['mic', 'Try again', ''], ['demo', 'Demo band', 'primary']] : [];
  } else if (mode === 'rx') {
    name = source === MIC ? 'mic' : 'wave';
    title = source === MIC ? 'Listening' : 'Listening to the demo band';
    text = 'Decoded text appears here. Tap a signal on the waterfall to tune, or Zero-beat.';
    buttons = source === MIC ? [['demo', 'Try the demo band', '']] : [['mic', 'Use the microphone', '']];
  } else {
    title = 'Listening';
    text = `Key to break in and transmit; the receiver comes back after the break-in delay. ${keys}`;
  }
  const key = [name, title, text, buttons].join('|');
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  $('#empty-icon use').setAttribute('href', `#i-${name}`);
  setText($('#empty-title'), title);
  setText($('#empty-text'), text);
  $('#empty-actions').replaceChildren(...buttons.map(([act, text, className]) => {
    const button = make('button', className, text);
    button.type = 'button';
    button.dataset.act = act;
    return button;
  }));
}

function renderKeying() {
  const live = str(w.cw_pattern(TX)), recent = performance.now() - lastTx.at < 1500;
  const text = live ? str(w.cw_live(TX)) : recent ? lastTx.text : '';
  const entry = BY_TEXT.get(text), glyph = $('#glyph');
  if (text && glyph.dataset.text !== text) restart(glyph, 'bump');
  glyphs(glyph, text);
  pattern($('#code'), live || (entry?.code ?? ''));
  setText($('#name'), entry?.name ?? '');
  $('.live').classList.toggle('old', !live);
  for (const key of $$('.keys button')) key.classList.toggle('on', held.has(+key.dataset.input));
  const swapped = get('swap_paddles') ? '  swapped' : '';
  setText($('#keying-info'), `${get('wpm')} WPM  ${NAMES.keyer[get('keyer_mode')][1]}${swapped}`);
  drawTape($('#tape'), edges.tx, report.clock + since(), msPerPx(1200 / get('wpm')), COLORS.accent);
}

function renderSignal() {
  const freq = w.cw_rx_freq(), bw = get('rx_bw'), open = w.cw_rx_open(), span = FMAX - FMIN;
  const rxNow = w.cw_rx_time() + (listening ? since() : 0), ms = msPerPx(1200 / w.cw_wpm(RX));
  if (get('rx_afc')) refresh('rx_freq');
  setText($('#signal-info'), `${freq.toFixed(0)} Hz  ${bw} Hz filter`);
  if (mode === 'rx') setText($('#log-info'), !listening ? 'no audio input' : open ? `${w.cw_wpm(RX).toFixed(0)} WPM` : 'listening');

  drawWaterfall();
  const passband = $('#passband').style;
  passband.left = `${((freq - bw / 2 - FMIN) / span) * 100}%`;
  passband.width = `${(bw / span) * 100}%`;

  drawEnvelope($('#lane-env'), rxNow, ms);
  drawTape($('#lane-rx'), edges.rx, rxNow, ms, COLORS.rx);
  if (mode === 'trx') drawTape($('#lane-tx'), edges.tx, report.clock + since(), ms, COLORS.accent);

  const level = w.cw_rx_level(), noise = w.cw_rx_noise(), threshold = w.cw_rx_threshold(), floor = noise - 6;
  const meter = $('#level');
  meter.style.setProperty('--level', Math.min(Math.max((level - floor) / 50, 0), 1));
  meter.classList.toggle('open', level >= threshold && open);
  $('#threshold').style.setProperty('--threshold', Math.min(Math.max((threshold - floor) / 50, 0), 1));
  setText($('#snr'), listening ? `SNR ${w.cw_rx_snr().toFixed(0)} dB` : 'no input');

  const lock = get('rx_lock');
  const lockButton = $('#lock');
  lockButton.setAttribute('aria-pressed', !!lock);
  setText(lockButton, `${(lock ? get('rx_wpm') : w.cw_wpm(RX)).toFixed(0)} WPM ${lock ? 'locked' : 'auto'}`);

  if (mode === 'trx') {
    const ptt = w.cw_ptt(), breakin = get('breakin_ms');
    const left = ptt ? Math.max(report.busy ? breakin : breakin - report.idle, 0) : 0;
    const pttLabel = $('#ptt');
    setText(pttLabel, ptt ? 'TX' : 'RX');
    pttLabel.className = ptt ? 'tx' : open && w.cw_rx_key() ? 'rx' : '';
    setText($('#breakin-label'), ptt ? 'BREAK-IN' : 'LISTENING');
    $('#breakin').style.width = `${(left / breakin) * 100}%`;
    setText($('#breakin-ms'), ptt ? `${left.toFixed(0)} ms` : '');
  }
}

const ABOUT = [
  'The Koch method plays every character at full speed from day one. Start with two and add one when you score 90%.',
  'Random characters from the chosen set.',
  'Frequent English words, the backbone of every conversation on the air.',
  'Q-codes and abbreviations heard in real contacts, each shown with its meaning.',
];

function renderTraining(v) {
  const send = get('train_dir'), content = get('train_content'), koch = content === 0 && get('train_charset') === 0;
  const total = w.cw_train_total(), correct = w.cw_train_correct();
  const material = koch ? `Koch ${get('train_koch')}` : content === 0 ? NAMES.charset[get('train_charset')][1] : NAMES.content[content][1];
  setText($('#train-title'), send ? `Send · ${material}` : `Copy · ${material} · level ${get('train_level')}`);
  setText($('#train-info'), total ? `${correct}/${total}` : '');
  if (v === 'train-setup') return renderSetup(send, content, koch);

  const state = w.cw_train_state(), ok = w.cw_train_ok(), target = str(w.cw_train_target());
  const typed = str(w.cw_train_typed()), sent = str(w.cw_train_sent()), hint = get('train_hint');
  const prompt = $('#prompt'), big = $('#big'), answer = $('#answer');
  const result = state === RESULT || state > SEND;
  let detail = '';

  // The prompt says what to do now; the steps show where that is in the item.
  const playing = state === LISTEN && report.playing;
  const [say, mark] = playing ? ['Listen', ''] : state === LISTEN ? ['Type what you heard', ''] : state === SEND ? ['Key this', '']
    : ok ? ['Correct', 'check'] : [send ? 'Not quite' : 'Not quite · no listens left', 'cross'];
  if (prompt.dataset.key !== say) {
    prompt.dataset.key = say;
    prompt.replaceChildren(...(mark ? [icon(mark)] : []), say);
  }
  prompt.className = result ? (ok ? 'ok' : 'err') : playing ? 'playing' : '';
  $('#new-record').hidden = !(result && w.cw_train_rec_hit());

  const steps = $('#steps'), names = send ? ['Read', 'Key', 'Result'] : ['Listen', 'Type', 'Result'];
  const now = result ? 2 : send ? (sent || held.size || str(w.cw_pattern(TX)) ? 1 : 0) : playing && !typed ? 0 : 1;
  [...steps.children].forEach((li, i) => {
    setText(li, names[i]);
    li.className = i < now ? 'done' : i === now ? 'now' : '';
  });
  steps.style.setProperty('--step', now);

  // The listens of a copy item: the used ones lit.
  const listens = $('#listens'), max = w.cw_train_max_listens(), used = Math.min(w.cw_train_listens(), max);
  listens.hidden = state !== LISTEN;
  if (state === LISTEN && listens.dataset.key !== `${used}/${max}`) {
    listens.dataset.key = `${used}/${max}`;
    listens.className = used >= max ? 'none-left' : '';
    listens.replaceChildren(
      ...Array.from({ length: max }, (_, i) => make('i', i < used ? 'used' : '', i < used ? '●' : '○')),
      ` listen ${used} of ${max}`);
  }

  if (state === LISTEN) typedView(big, typed);
  else glyphs(big, target);
  big.className = state === LISTEN ? 'typed' : result ? (ok ? 'ok' : 'err') : '';
  pattern($('#pattern'), state === LISTEN || (state === SEND && !hint) ? '' : codes(target));
  $('#pattern').classList.toggle('faint', !hint && state !== RESULT);
  setText($('#meaning'), state === LISTEN ? '' : str(w.cw_train_meaning()));

  if (state === RESULT && !ok) detail = `you typed "${typed || 'nothing'}"`;
  else if (state === SEND) detail = sent;
  else if (result && send) detail = ok ? `${w.cw_train_send_wpm().toFixed(1)} WPM  ·  dah/dit ratio 1:${w.cw_train_send_ratio().toFixed(1)}` : `you sent "${sent}"`;
  glyphs($('#detail'), detail);
  pattern($('#live-sent'), state === SEND ? str(w.cw_pattern(TX)) : '');

  // Copy keeps the field focused through the result (out of sight): the phone
  // keeps its keyboard and Enter goes on. Send has no field: Space keys.
  answer.hidden = !!send;
  answer.classList.toggle('away', state !== LISTEN);
  answer.enterKeyHint = result ? 'next' : 'done';
  if (send && document.activeElement === answer) answer.blur();
  if (!send && state !== trainState && !$('dialog:modal')) answer.focus({ preventScroll: true });
  if (answer.value !== typed && state === LISTEN) answer.value = typed;
  trainState = state;

  const left = max - used, spent = !send && state === LISTEN && left <= 0;
  label($('#tab-act'), send ? (hint ? 'Hide hint' : 'Show hint') : spent ? 'No listens left' : state === LISTEN ? `Replay (${left} left)` : 'Replay', 'Tab');
  $('#tab-act').disabled = spent;
  label($('#enter-act'), result ? 'Next' : 'Check', 'Enter');

  const recent = $('#recent'), history = [];
  for (let i = 0, r; i < 20 && (r = w.cw_train_hist(i)) >= 0; i++) history.unshift(r);
  if (recent.dataset.history !== history.join('')) {
    recent.dataset.history = history.join('');
    recent.replaceChildren(...history.map((r) => Object.assign(el('span', r ? 'ok' : 'err'), { textContent: r ? '●' : '○' })));
  }
  const record = w.cw_train_record();
  setText($('#score'), `${total ? `${correct}/${total}  ${Math.floor((correct * 100) / total)}%  streak ${w.cw_train_streak()}  best ${w.cw_train_best()}  ` : ''}record ${record || '–'}`);
  setText($('#weak'), str(w.cw_train_weak()));
}

// What the student typed, the newest character popping in, then a caret.
function typedView(big, typed) {
  if (big.dataset.text === `\u0001${typed}`) return;
  const grew = typed.length > (big.dataset.typed ?? '').length;
  glyphs(big, typed);
  big.dataset.text = `\u0001${typed}`; // never the target's: glyphs() redraws it
  big.dataset.typed = typed;
  const last = big.lastChild;
  if (grew && last?.nodeType === Node.TEXT_NODE && last.textContent) {
    last.textContent = last.textContent.slice(0, -1);
    big.append(make('span', 'pop', typed.slice(-1)));
  }
  big.append(make('span', 'caret', '▁'));
}

function renderSetup(send, content, koch) {
  const when = { chars: content === 0, koch, send: !!send, copy: !send };
  for (const row of $$('#setup [data-when]')) row.hidden = !when[row.dataset.when];
  setText($('#setup-intro'), send ? 'Send: CWtty shows the text, you key it with Space, the paddles or the buttons.'
                                  : 'Copy by ear: CWtty sends, you type what you hear.');
  if (koch) {
    const chars = $('#koch-chars'), set = str(w.cw_train_charset());
    if (chars.dataset.text !== set) {
      chars.dataset.text = set;
      chars.replaceChildren(set.slice(0, -1), Object.assign(el('b'), { textContent: set.slice(-1) }));
    }
  }
  setText($('#setup-about'), ABOUT[content === 0 ? (koch ? 0 : 1) : content + 1]);
  renderSetupRecords(send);
  const listens = 5 - get('train_level'), record = stat(`copy_record_${get('train_level')}`);
  setText($('#level-hint'), `${listens} listen${listens > 1 ? 's' : ''} per item  ·  record ${record || 'none yet'}`);
  const resumable = !!w.cw_train_resumable();
  $('#resume').hidden = !resumable;
  label($('#start'), resumable ? 'New session' : 'Start', 'Enter');
  const f = get('farnsworth');
  setText($('#setup-speed'), `Speed ${get('wpm')} WPM${f ? `, spaced as ${f} WPM (Farnsworth)` : ''}  ·  tone ${get('tone_hz')} Hz`);
}

// The records to beat: one per copy level, one for send.
function renderSetupRecords(send) {
  const line = $('#setup-records'), level = get('train_level');
  const values = send ? [stat('send_record')] : [1, 2, 3, 4].map((l) => stat(`copy_record_${l}`));
  const key = `${send}:${level}:${values}`;
  if (line.dataset.key === key) return;
  line.dataset.key = key;
  if (send) {
    const v = values[0];
    line.replaceChildren(make('small', '', 'RECORD'), make('b', '', v || 'none yet'), v ? ` correct answer${v > 1 ? 's' : ''} in a row` : '');
  } else {
    line.replaceChildren(
      make('small', '', 'RECORDS'),
      ...values.map((v, i) => make('span', i + 1 === level ? 'current' : '', `L${i + 1} `, make('b', '', v || '–'))),
      make('small', '', 'best streaks'));
  }
}

// ---- Statistics view ----

let statsDrawn;

function fmtTime(seconds) {
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  return `${Math.floor(seconds / 3600)}h ${String(Math.floor(seconds / 60) % 60).padStart(2, '0')}m`;
}

const pct = (part, whole) => (whole > 0 ? Math.floor((part * 100) / whole) : 0);

function figures(dl, rows) {
  dl.replaceChildren(
    ...rows.flatMap(([label, value, detail]) => [
      make('dt', '', label),
      make('dd', value ? '' : 'none', value || '–'),
      make('dd', 'detail', value ? detail ?? '' : ''),
    ]));
}

const CHAR_ROWS = ['ABCDEFGHIJKLMNOPQRSTUVWXYZ', '0123456789.,?/=-()\'":;!@'];

function characters(dir, grid, weak) {
  grid.replaceChildren(
    ...CHAR_ROWS.map((row) => {
      const line = el('p');
      for (const ch of row) {
        const code = ch.charCodeAt(0), p = w.cw_stat_pct(dir, code), seen = w.cw_stat_seen(dir, code);
        const span = make('span', p < 0 ? '' : p >= 90 ? 'good' : p >= 75 ? 'fair' : 'poor', ch);
        span.title = p < 0 ? `${ch}: not yet` : `${ch}: ${p}% of ${seen}`;
        line.append(span);
      }
      return line;
    }));
  const list = [...str(w.cw_stat_weak(dir))];
  weak.replaceChildren(
    make('small', '', 'Weakest'),
    ...(list.length
      ? list.map((ch) => make('span', '', `${ch} `, make('small', '', `${w.cw_stat_pct(dir, ch.charCodeAt(0))}%`)))
      : [make('small', '', 'none yet')]));
}

function renderStats() {
  const key = `${statsSerial}:${today()}:${get('train_level')}`;
  if (statsDrawn === key) return;
  statsDrawn = key;

  const copyAnswers = stat('copy_answers'), sendAnswers = stat('send_answers');
  $('#stats-empty').hidden = copyAnswers + sendAnswers > 0;
  $('#stats-body').hidden = copyAnswers + sendAnswers === 0;
  setText($('#stats-since'), stat('first_day') > 0 ? `since ${dateOf(stat('first_day'))}` : '');
  if (copyAnswers + sendAnswers === 0) return;

  // Records: four copy levels and send.
  const level = get('train_level');
  const tile = (title, prefix, current) => {
    const value = stat(prefix);
    const detail = value ? [`${stat(`${prefix}_wpm`)} WPM`, dateOf(stat(`${prefix}_day`))].filter(Boolean).join('  ·  ') : 'not yet';
    return make('div', `record${current ? ' current' : ''}${value ? '' : ' none'}`, make('small', '', title), make('b', '', value || '–'), make('span', '', detail));
  };
  $('#records').replaceChildren(
    ...[1, 2, 3, 4].map((l) => tile(`Receive · level ${l}`, `copy_record_${l}`, l === level)),
    tile('Send', 'send_record', false));
  [...$('#records').children].forEach((card, i) => card.style.setProperty('--i', i));

  // Practice: days and the last four weeks.
  const koch = stat('koch'), days = stat('days');
  setText($('#practice-note'), `${days} day${days === 1 ? '' : 's'} practised  ·  streak ${w.cw_stat_day_streak()} (best ${stat('best_day_streak')})${koch > 0 ? `  ·  Koch ${koch}` : ''}`);
  const counts = Array.from({ length: 28 }, (_, i) => w.cw_stat_activity(27 - i));
  const most = Math.max(1, ...counts);
  $('#activity').replaceChildren(
    ...counts.map((n, i) => {
      const bar = el('i', `${n ? '' : 'zero'}${i === 27 ? ' today' : ''}`);
      bar.style.setProperty('--h', n ? Math.max(n / most, 0.06) : 0);
      bar.style.setProperty('--i', i);
      bar.title = `${dateOf(today() - 27 + i)}: ${n} answer${n === 1 ? '' : 's'}`;
      return bar;
    }));
  const sum = counts.reduce((a, b) => a + b, 0);
  setText($('#activity-total'), `${sum} answer${sum === 1 ? '' : 's'}`);

  // Receive and send.
  const items = stat('copy_items'), timed = stat('send_timed');
  const sessions = (n) => `${n} session${n === 1 ? '' : 's'}`;
  figures($('#copy-figures'), [
    ['Answers', copyAnswers, `${pct(stat('copy_correct'), copyAnswers)}% correct`],
    ['First listen', items && `${pct(stat('copy_first'), items)}%`, 'items copied at once'],
    ['Listens / item', items && (stat('copy_listens') / items).toFixed(1), `over ${items} item${items === 1 ? '' : 's'}`],
    ['Out of listens', items && String(stat('copy_failed')), 'items lost'],
    ['Time', copyAnswers && fmtTime(stat('copy_seconds')), sessions(stat('copy_sessions'))],
  ]);
  figures($('#send-figures'), [
    ['Answers', sendAnswers, `${pct(stat('send_correct'), sendAnswers)}% correct`],
    ['Best speed', timed && (stat('send_best10') / 10).toFixed(1), 'WPM, fastest correct item'],
    ['Average speed', timed && (stat('send_wpm10') / 10 / timed).toFixed(1), 'WPM'],
    ['Dah/dit ratio', timed && `1:${(stat('send_ratio10') / 10 / timed).toFixed(1)}`, '1:3 is ideal'],
    ['Time', sendAnswers && fmtTime(stat('send_seconds')), sessions(stat('send_sessions'))],
  ]);
  characters(0, $('#copy-chars'), $('#copy-weak'));
  characters(1, $('#send-chars'), $('#send-weak'));
  for (const number of $$('.record b, .figures dd:not(.detail)')) countUp(number);
}

// A figure counts up to its value ("85%", "1.4", "23").
function countUp(node) {
  const m = /^(\d+)(\.\d)?(%?)$/.exec(node.textContent);
  if (!m || reducedMotion.matches) return;
  const value = parseFloat(m[1] + (m[2] ?? '')), places = m[2] ? 1 : 0, t0 = performance.now();
  const tick = (now) => {
    const k = Math.min((now - t0) / 700, 1);
    node.textContent = (value * (1 - (1 - k) ** 3)).toFixed(places) + m[3];
    if (k < 1) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

function renderTable() {
  const live = str(w.cw_pattern(TX)) || (listening ? str(w.cw_pattern(RX)) : '');
  for (const item of $$('#table li')) item.classList.toggle('lit', item.dataset.code === live);
}

let toastTimer;

function toast(text, warn = false, ms = warn ? 4000 : 2200) {
  const t = $('#toast');
  t.textContent = text;
  t.classList.toggle('warn', warn);
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), ms);
}

// ---- Actions, from buttons and keys ----

const log = () => $(`#log-${mode}`);

function step(key, delta, message) {
  const value = set(key, get(key) + delta);
  if (message) toast(message(value));
  changed();
}

function bandwidth(dir) {
  const i = BANDWIDTHS.indexOf(get('rx_bw'));
  toast(`Filter ${set('rx_bw', BANDWIDTHS[(i + dir + BANDWIDTHS.length) % BANDWIDTHS.length])} Hz`);
  changed();
}

const actions = {
  enter() {
    if (mode === 'train') train(w.cw_train_enter());
    else log().lastElementChild?.setAttribute('data-closed', ''); // a new line
  },
  backspace() {
    if (mode === 'train') train(w.cw_train_backspace());
    else if (mode === 'tx') log().lastElementChild?.lastChild?.remove();
  },
  tab() {
    if (get('train_dir')) {
      set('train_hint', +!get('train_hint'));
      changed();
    } else if (w.cw_train_replay() & PLAY) {
      play(str(w.cw_train_target()), 200); // hear it again
    } else {
      toast(`No listens left at level ${get('train_level')}: type what you heard`, true);
    }
  },
  setup() {
    audio.send('stop');
    releaseAll();
    w.cw_train_setup();
  },
  resume: () => train(w.cw_train_resume()),
  mic: () => useSource(MIC),
  demo: () => useSource(DEMO),
  clear: () => log().replaceChildren(),
  'sens+': () => step('rx_sens', 1, (v) => `Sensitivity ${v}`),
  'sens-': () => step('rx_sens', -1, (v) => `Sensitivity ${v}`),
  'tune+': () => step('rx_freq', 10),
  'tune-': () => step('rx_freq', -10),
  'filter+': () => bandwidth(1),
  'filter-': () => bandwidth(-1),
  afc() {
    toast(`AFC ${set('rx_afc', +!get('rx_afc')) ? 'on' : 'off'}`);
    changed();
  },
  lock() {
    toast(w.cw_lock_speed() ? `Speed locked at ${get('rx_wpm')} WPM` : 'Speed tracking on');
    changed();
  },
  zero() {
    const hz = w.cw_zero_beat();
    toast(hz ? `Tuned to ${hz} Hz` : 'No clear signal to tune to', !hz);
    changed();
  },
  table() {
    const dialog = $('#table-dialog');
    if (dialog.open) dialog.close();
    else dialog.show(); // not modal: keying goes on
  },
  menu: () => $('#menu-dialog').showModal(),
  settings: () => $('#settings-dialog').showModal(),
  help: () => $('#help-dialog').showModal(),
  test: () => play('VVV', 50),
  training: () => setMode('train'),
};

const RX_KEYS = {
  '+': 'sens+', '=': 'sens+', '-': 'sens-', '_': 'sens-', '<': 'tune-', ',': 'tune-', '>': 'tune+', '.': 'tune+',
  z: 'zero', b: 'filter+', ']': 'filter+', B: 'filter-', '[': 'filter-', a: 'afc', s: 'lock',
};
const RX_ARROWS = { ArrowUp: 'tune+', ArrowRight: 'tune+', ArrowDown: 'tune-', ArrowLeft: 'tune-' };
const PADDLES = { ArrowLeft: 0, ArrowRight: 1, Space: 2 };

const modifiers = (e) => e.ctrlKey || e.altKey || e.metaKey;
const isField = (target) => target.matches?.('input:not([type="checkbox"]), select, textarea');

function shortcut(e) {
  const field = isField(e.target);
  if (modifiers(e)) return null;
  if (e.key === 'Enter' && !e.target.matches?.('button, select')) return 'enter';
  if (e.key === 'Escape' && $('#table-dialog').open) return 'table';
  if (e.key === 'Escape' && mode === 'train') return 'setup';
  if (e.key === 'Tab' && (view === 'train-copy' || view === 'train-send')) return 'tab';
  if (field) return null;
  if (view === 'stats' && e.key === 'p') return 'training';
  if (e.key === 'Backspace') return 'backspace';
  if (mode === 'rx' || mode === 'trx') return RX_KEYS[e.key] ?? (mode === 'rx' ? RX_ARROWS[e.key] : null);
  return null;
}

addEventListener('keydown', (e) => {
  if ($('dialog:modal')) return; // the dialog has the keys
  const input = PADDLES[e.code];
  if (input !== undefined && !isField(e.target) && !modifiers(e) && get('key_input') === 0 && w.cw_keying()) {
    e.preventDefault();
    if (!e.repeat) press(input, true);
    return;
  }
  const action = shortcut(e);
  if (action) {
    e.preventDefault();
    actions[action]();
  }
});

addEventListener('keyup', (e) => {
  const input = PADDLES[e.code];
  if (input !== undefined && held.has(input)) {
    e.preventDefault();
    press(input, false);
  }
});

// Never leave the transmitter keyed when the page loses the keys.
addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', releaseAll);

// A USB pad or straight key seen as mouse buttons. On the pad left is the left
// paddle, right the right one; either button is the straight key, down while
// one of them is held.
const usb = (e) => get('key_input') > 0 && (e.button === 0 || e.button === 2);
const usbInput = (e) => (get('key_input') === 2 ? 2 : e.button ? 1 : 0);
addEventListener('mousedown', (e) => {
  if (!usb(e) || e.target.closest('button, select, input, label, dialog, header')) return;
  e.preventDefault();
  press(usbInput(e), true);
});
addEventListener('mouseup', (e) => usb(e) && !(get('key_input') === 2 && e.buttons & 3) && press(usbInput(e), false));
addEventListener('contextmenu', (e) => get('key_input') > 0 && e.preventDefault());

// The on-screen keys, for touch screens and mice.
for (const key of $$('.keys button')) {
  const input = +key.dataset.input, up = () => press(input, false);
  key.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    key.setPointerCapture(e.pointerId);
    press(input, true);
  });
  key.addEventListener('pointerup', up);
  key.addEventListener('pointercancel', up);
}

// A tap outside a dialog closes it (its padding is inside), and so does a
// choice in the menu.
for (const dialog of $$('dialog')) {
  dialog.addEventListener('click', (e) => {
    if (e.target === dialog || (dialog.id === 'menu-dialog' && e.target.closest('button'))) dialog.close();
  });
}

document.addEventListener('click', (e) => {
  const target = e.target.closest('[data-mode], [data-act]');
  if (!target) return;
  e.preventDefault();
  if (target.dataset.mode) setMode(target.dataset.mode);
  else actions[target.dataset.act]();
});

// The copy answer: whatever the keyboard (or an IME) did to the field is
// replayed as typed characters and deletions.
$('#answer').addEventListener('input', (e) => {
  const typed = str(w.cw_train_typed()), value = e.target.value.toUpperCase();
  let same = 0, fx = 0;
  while (same < typed.length && typed[same] === value[same]) same++;
  for (let n = typed.length; n > same; n--) w.cw_train_backspace();
  for (const ch of value.slice(same)) {
    fx |= w.cw_train_type(ch.codePointAt(0));
    if (fx || w.cw_train_state() !== LISTEN) break;
  }
  train(fx);
  e.target.value = str(w.cw_train_typed());
});

$('#wf').addEventListener('pointerdown', (e) => {
  const r = e.currentTarget.getBoundingClientRect();
  set('rx_freq', Math.round((FMIN + ((e.clientX - r.left) / r.width) * (FMAX - FMIN)) / 10) * 10);
  changed();
});

$('#source').addEventListener('click', (e) => {
  const button = e.target.closest('button');
  if (button) useSource(+button.dataset.value);
});

for (const control of $$('[data-key]')) {
  if (control.classList.contains('seg')) {
    control.addEventListener('click', (e) => {
      const button = e.target.closest('button');
      if (!button) return;
      set(control.dataset.key, +button.dataset.value);
      changed();
    });
    continue;
  }
  control.addEventListener(control.type === 'range' ? 'input' : 'change', () => {
    set(control.dataset.key, control.type === 'checkbox' ? +control.checked : +control.value);
    changed();
  });
}

// The receiver's tuning drawer: open on wide screens, remembered once toggled.
function openTuning(open) {
  $('#tuning').classList.toggle('open', open);
  $('#tuning-toggle').setAttribute('aria-expanded', open);
}
$('#tuning-toggle').addEventListener('click', () => {
  const open = !$('#tuning').classList.contains('open');
  openTuning(open);
  try {
    localStorage['cwtty-tuning'] = +open;
  } catch {}
});
try {
  openTuning(localStorage['cwtty-tuning'] ? localStorage['cwtty-tuning'] === '1' : matchMedia('(min-width: 40rem)').matches);
} catch {
  openTuning(false);
}
new ResizeObserver(moveIndicator).observe($('nav'));

// ---- Start ----

w.cw_init(crypto.getRandomValues(new Uint32Array(1))[0]);

for (const select of $$('select[data-names]'))
  select.append(...NAMES[select.dataset.names].map(([value, text]) => new Option(text, value)));
$('[data-key="train_koch"]').max = w.cw_koch_max();

$('#table').append(
  ...['Letters', 'Numbers', 'Punctuation', 'Prosigns'].map((title, group) => {
    const section = el('section'), list = el('ul');
    section.append(Object.assign(el('h3'), { textContent: title.toUpperCase() }), list);
    for (const entry of TABLE.filter((e) => e.group === group)) {
      const item = el('li');
      item.title = entry.name;
      item.dataset.code = entry.code;
      item.append(glyphs(el('b'), entry.text), pattern(el('span', 'pattern'), entry.code));
      list.append(item);
    }
    return section;
  }));

try {
  loadStats(localStorage[STATS_KEY]);
} catch {}
// Another tab of the app saved: carry on from its statistics.
addEventListener('storage', (e) => e.key === STATS_KEY && loadStats(e.newValue));

const saved = restore();
source = saved.source ?? MIC;
segment($('#source'), source);
new ResizeObserver(resize).observe($('#wf'));
for (const canvas of $$('#tape, .lanes canvas')) new ResizeObserver(resize).observe(canvas);

try {
  await audio.start(wasm, onReport);
} catch {
  toast('This browser cannot run the audio engine (AudioWorklet)', true);
}
await setMode(MODES.includes(saved.mode) ? saved.mode : 'tx');
requestAnimationFrame(frame);
navigator.serviceWorker?.register('sw.js');
offerInstall($$('[data-install]'), toast);
