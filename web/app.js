// app.js - The page. The application lives in cwtty.wasm, compiled from the
// C sources of the terminal app (see wasm/page.c): this draws its state and
// feeds it with the keys and with what the audio worklet reports.

import * as audio from './audio.js';
import { offerInstall } from './install.js';

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const el = (tag, className = '') => Object.assign(document.createElement(tag), { className });

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
const MODES = ['tx', 'rx', 'trx', 'train'];
const RX = 0, TX = 1;
const CHAR = 0, PROSIGN = 1, SPACE = 3, LINE = 4, KEY_UP = 5, KEY_DOWN = 6;
const SETUP = 0, LISTEN = 1, RESULT = 2, SEND = 3;
const PLAY = 1, WRONG = 4, KOCH = 8;
const OFF = 0, MIC = 1;

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

// Shows the settings in every control bound to them (or only to key).
function refresh(key) {
  $('[data-key="farnsworth"]').max = get('wpm') - 1;
  for (const control of $$(key ? `[data-key="${key}"]` : '[data-key]')) {
    const value = get(control.dataset.key), out = control.nextElementSibling;
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

// ---- Modes and the receiver's input ----

async function setMode(name) {
  releaseAll();
  audio.send('stop');
  mode = name;
  w.cw_mode(MODES.indexOf(name));
  for (const button of $$('nav [data-mode]')) button.setAttribute('aria-current', button.dataset.mode === name);
  $('#log-title').textContent = { tx: 'Transmit', rx: 'Receive', trx: 'Transceiver', train: '' }[name];
  $('#log-info').textContent = '';
  changed();
  await listen();
}

async function listen() {
  const want = mode === 'rx' || mode === 'trx' ? source : OFF;
  const ok = await audio.microphone(want === MIC);
  if (!ok) toast('No microphone: allow it, or listen to the demo band', true);
  listening = ok && want !== OFF;
  w.cw_source(listening ? want : OFF);
  $('#in').classList.toggle('used', want !== OFF);
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
    toast(send ? 'Not quite: start sending again from the beginning' : 'Not quite: listen and start again', true);
  }
  if (fx & PLAY) play(str(w.cw_train_target()), 350);
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
    line.append(text);
  } else {
    line.append(glyphs(el('span', kind === PROSIGN ? '' : 'unknown'), text));
  }
  if (atEnd) log.scrollTop = log.scrollHeight;
}

// ---- Drawing helpers ----

function setText(node, text) {
  if (node.textContent !== text) node.textContent = text;
}

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
  const v = currentView();
  if (v !== view) view = document.body.dataset.view = v;
  enable();
  $('#out').classList.toggle('on', audio.running());
  $('#in').classList.toggle('on', listening);
  if (v === 'tx' || v === 'trx' || v === 'train-send') renderKeying();
  if (v === 'rx' || v === 'trx') renderSignal();
  if (v.startsWith('train')) renderTraining(v);
  if ($('#table-dialog').open) renderTable();
  if (v === 'trx') setText($('#log-info'), $('#log-trx').childElementCount ? 'QSO log' : 'listening, key to break in');
}

function renderKeying() {
  const live = str(w.cw_pattern(TX)), recent = performance.now() - lastTx.at < 1500;
  const text = live ? str(w.cw_live(TX)) : recent ? lastTx.text : '';
  const entry = BY_TEXT.get(text);
  glyphs($('#glyph'), text);
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
  setText($('#train-title'), v === 'train-setup' ? 'Training' : `Training  ${send ? 'send' : 'copy'}  ${material}`);
  setText($('#train-info'), send ? `${get('train_strict') ? 'strict' : 'relaxed'}  ·  score ${correct}/${total}` : total ? `score ${correct}/${total}` : '');
  if (v === 'train-setup') return renderSetup(send, content, koch);

  const state = w.cw_train_state(), ok = w.cw_train_ok(), target = str(w.cw_train_target());
  const typed = str(w.cw_train_typed()), sent = str(w.cw_train_sent()), hint = get('train_hint');
  const prompt = $('#prompt'), big = $('#big'), answer = $('#answer');
  const result = state === RESULT || state > SEND;
  let detail = '';

  if (state === LISTEN) setText(prompt, report.playing ? `Listen${'...'.slice(0, ((performance.now() / 350) | 0) % 4)}` : 'Type what you heard');
  else if (state === SEND) setText(prompt, 'Send');
  else setText(prompt, ok ? '● Correct' : '▲ Not quite');
  prompt.className = result ? (ok ? 'ok' : 'err') : '';

  glyphs(big, state === LISTEN ? typed || ' ' : target);
  big.className = state === LISTEN ? 'typed' : result && ok ? 'ok' : '';
  pattern($('#pattern'), state === LISTEN || (state === SEND && !hint) ? '' : codes(target));
  $('#pattern').classList.toggle('faint', !hint && state !== RESULT);
  setText($('#meaning'), state === LISTEN ? '' : str(w.cw_train_meaning()));

  if (state === RESULT && !ok) detail = `you typed "${typed || 'nothing'}"`;
  else if (state === SEND) detail = sent;
  else if (result && send) detail = ok ? `${w.cw_train_send_wpm().toFixed(1)} WPM  ·  dah/dit ratio 1:${w.cw_train_send_ratio().toFixed(1)}` : `you sent "${sent}"`;
  glyphs($('#detail'), detail);
  pattern($('#live-sent'), state === SEND ? str(w.cw_pattern(TX)) : '');

  answer.hidden = state !== LISTEN;
  if (state === LISTEN && state !== trainState && !$('dialog:modal')) answer.focus();
  if (answer.value !== typed && state === LISTEN) answer.value = typed;
  trainState = state;

  label($('#tab-act'), send ? (hint ? 'Hide hint' : 'Show hint') : 'Replay', 'Tab');
  label($('#enter-act'), result ? 'Next' : 'Check', 'Enter');

  const recent = $('#recent'), history = [];
  for (let i = 0, r; i < 20 && (r = w.cw_train_hist(i)) >= 0; i++) history.unshift(r);
  if (recent.dataset.history !== history.join('')) {
    recent.dataset.history = history.join('');
    recent.replaceChildren(...history.map((r) => Object.assign(el('span', r ? 'ok' : 'err'), { textContent: r ? '●' : '○' })));
  }
  setText($('#score'), total ? `${correct}/${total}  ${Math.floor((correct * 100) / total)}%  streak ${w.cw_train_streak()}  best ${w.cw_train_best()}` : '');
  setText($('#weak'), str(w.cw_train_weak()));
}

function renderSetup(send, content, koch) {
  const when = { chars: content === 0, koch, send: !!send };
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
  const f = get('farnsworth');
  setText($('#setup-speed'), `Speed ${get('wpm')} WPM${f ? `, spaced as ${f} WPM (Farnsworth)` : ''}  ·  tone ${get('tone_hz')} Hz`);
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
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.textContent = ''), ms);
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
    } else {
      play(str(w.cw_train_target()), 200); // hear it again
    }
  },
  setup() {
    audio.send('stop');
    releaseAll();
    w.cw_train_setup();
  },
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

// A USB pad seen as mouse buttons: left is the left paddle, right the right one.
const pad = (e) => get('key_input') === 1 && (e.button === 0 || e.button === 2);
addEventListener('mousedown', (e) => {
  if (!pad(e) || e.target.closest('button, select, input, label, dialog, header')) return;
  e.preventDefault();
  press(e.button ? 1 : 0, true);
});
addEventListener('mouseup', (e) => pad(e) && press(e.button ? 1 : 0, false));
addEventListener('contextmenu', (e) => get('key_input') === 1 && e.preventDefault());

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

$('#source').addEventListener('change', (e) => {
  source = +e.target.value;
  listen();
  save();
});

for (const control of $$('[data-key]')) {
  control.addEventListener(control.type === 'range' ? 'input' : 'change', () => {
    set(control.dataset.key, control.type === 'checkbox' ? +control.checked : +control.value);
    changed();
  });
}

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

const saved = restore();
source = saved.source ?? MIC;
$('#source').value = source;
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
