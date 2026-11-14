// cloud.js - The statistics on Filen, in the browser. The client lives in
// cwtty.wasm (src/filen.c, src/statsync.c, wasm/page.c): it signs in,
// encrypts, prepares every request and reads every reply. This file only
// sends the requests with fetch(), keeps the session (encrypted in
// IndexedDB with a key the page cannot read back) and decides when to sync.

const WAIT = 1, DONE = 2;
const DB = 'cwtty-cloud';

// ---- The session, encrypted at rest ----

function idb() {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open(DB, 1);
    open.onupgradeneeded = () => open.result.createObjectStore('kv');
    open.onsuccess = () => resolve(open.result);
    open.onerror = () => reject(open.error);
  });
}

async function kv(mode, fn) {
  const db = await idb();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction('kv', mode), req = fn(tx.objectStore('kv'));
      tx.oncomplete = () => resolve(req?.result);
      tx.onerror = tx.onabort = () => reject(tx.error);
    });
  } finally {
    db.close();
  }
}

const kvGet = (key) => kv('readonly', (s) => s.get(key));
const kvSet = (key, value) => kv('readwrite', (s) => s.put(value, key));
const kvDel = (key) => kv('readwrite', (s) => s.delete(key));

// A key that never leaves the browser's crypto: the page can use it, not read it.
async function vaultKey() {
  let key = await kvGet('key');
  if (!key) {
    key = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
    await kvSet('key', key);
  }
  return key;
}

async function keep(text) {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const data = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, await vaultKey(), new TextEncoder().encode(text));
  await kvSet('session', { iv, data });
}

async function kept() {
  const s = await kvGet('session');
  if (!s) return null;
  const plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: s.iv }, await vaultKey(), s.data);
  return new TextDecoder().decode(plain);
}

// ---- The client ----

export function createCloud({ w, str, onStats, onChange }) {
  const enc = new TextEncoder();
  const heap = () => new Uint8Array(w.memory.buffer);
  let busy = false, again = false, remember = true, due = 0, timer = 0, lastSync = 0, failed = false, error = '';

  // Text for the C side: cw_cloud_in() (zeroed there after use).
  function input(bytes) {
    if (bytes.length >= w.cw_cloud_in_cap()) return false;
    heap().set(bytes, w.cw_cloud_in());
    heap()[w.cw_cloud_in() + bytes.length] = 0;
    return true;
  }

  function seed() {
    input(crypto.getRandomValues(new Uint8Array(32)));
    w.cw_cloud_seed();
  }

  // Performs the requests of the task until it is over: DONE or a failure.
  async function run() {
    for (;;) {
      const state = w.cw_cloud_run();
      if (state !== WAIT) return state;
      const post = w.cw_cloud_post(), headers = {};
      const auth = str(w.cw_cloud_auth()), sum = str(w.cw_cloud_checksum());
      if (auth) headers.Authorization = auth;
      if (sum) headers.Checksum = sum;
      if (post) headers['Content-Type'] = w.cw_cloud_json() ? 'application/json' : 'application/octet-stream';
      const body = post ? heap().slice(w.cw_cloud_body(), w.cw_cloud_body() + w.cw_cloud_body_len()) : undefined;
      let status = 0, bytes = new Uint8Array();
      try {
        const r = await fetch(str(w.cw_cloud_url()), {
          method: post ? 'POST' : 'GET', headers, body, cache: 'no-store', credentials: 'omit',
          signal: AbortSignal.timeout(20000),
        });
        bytes = new Uint8Array(await r.arrayBuffer());
        status = r.status;
      } catch {} // no connection: status 0
      if (bytes.length > w.cw_cloud_resp_cap()) {
        status = 0;
        bytes = new Uint8Array();
      }
      heap().set(bytes, w.cw_cloud_resp());
      w.cw_cloud_reply(status, bytes.length);
    }
  }

  // One task at a time, and one tab at a time where the browser can tell.
  const exclusive = (fn) => (navigator.locks ? navigator.locks.request('cwtty-cloud', fn) : fn());

  async function save() {
    const p = w.cw_cloud_session();
    try {
      if (p && remember) await keep(str(p));
      else await kvDel('session');
    } catch {} // private mode: signed in until the page closes
  }

  async function sync() {
    if (busy) {
      again = true;
      return;
    }
    if (!w.cw_cloud_signed_in()) return;
    busy = true;
    error = '';
    onChange();
    await exclusive(async () => {
      seed();
      w.cw_cloud_sync(Date.now());
      const state = await run();
      if (state === DONE) {
        failed = false;
        lastSync = Date.now();
        if (w.cw_cloud_merge()) onStats();
        await save(); // the folders may have been made again
      } else if (w.cw_cloud_signed_out()) {
        error = 'Filen ended the session: sign in again';
        w.cw_cloud_sign_out();
        await save();
      } else {
        failed = true;
        error = str(w.cw_cloud_error());
        schedule(300000); // again in five minutes
      }
    });
    busy = false;
    onChange();
    if (again) {
      again = false;
      sync();
    }
  }

  function schedule(ms) {
    if (!w.cw_cloud_signed_in()) return;
    const at = Date.now() + ms;
    if (due && due <= at) return;
    due = at;
    clearTimeout(timer);
    timer = setTimeout(() => {
      due = 0;
      sync();
    }, ms);
  }

  return {
    async start() {
      seed();
      try {
        const text = await kept();
        if (text && input(enc.encode(text)) && w.cw_cloud_session_load()) schedule(1500);
      } catch {}
      // Leaving: the last answers go up, if the browser lets the page finish.
      addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden' && w.cw_cloud_pending() && !busy) sync();
      });
      onChange();
    },

    async login(email, password, code, keepSession) {
      if (busy) return;
      remember = keepSession;
      busy = true;
      error = '';
      onChange();
      // The wasm call that derives the keys holds the page for a moment:
      // let "Signing in" show first.
      await new Promise((resolve) => setTimeout(resolve, 50));
      await exclusive(async () => {
        seed();
        const text = enc.encode(`${email}\0${password}\0${code}\0`);
        if (!input(text)) {
          error = 'That is too long';
          return;
        }
        text.fill(0);
        w.cw_cloud_login();
        if ((await run()) === DONE) {
          await save();
        } else {
          error = str(w.cw_cloud_error());
          w.cw_cloud_sign_out(); // nothing half done is kept
        }
      });
      busy = false;
      onChange();
      if (w.cw_cloud_signed_in()) sync();
    },

    async signOut() {
      clearTimeout(timer);
      due = 0;
      w.cw_cloud_sign_out();
      lastSync = 0;
      failed = false;
      error = '';
      try {
        await kvDel('session');
      } catch {}
      onChange();
    },

    syncNow: () => sync(),
    // The training counted: once it pauses for a minute.
    changed() {
      if (!w.cw_cloud_signed_in()) return;
      clearTimeout(timer);
      due = 0;
      schedule(60000);
    },
    soon: () => schedule(1500),
    // Looking at the statistics: the other devices' latest, now and then.
    fresh() {
      if (w.cw_cloud_signed_in() && !busy && Date.now() - lastSync > 60000) sync();
    },

    get state() {
      return busy ? 'busy' : w.cw_cloud_signed_in() ? 'ready' : 'off';
    },
    get email() {
      return str(w.cw_cloud_email());
    },
    get needCode() {
      return !!w.cw_cloud_need_code();
    },
    get error() {
      return error;
    },
    get failed() {
      return failed;
    },
    get lastSync() {
      return lastSync;
    },
  };
}
