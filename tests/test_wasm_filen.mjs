#!/usr/bin/env node
// Checks the Filen client as built into the web app (build-web/cwtty.wasm):
// the same C as tests/test_filen.c, but with the web build's libc, Mbed TLS
// configuration and growing memory. A fake Filen answers in Node, checking
// the cryptography with Node's own (and tests/filen_vectors.h):
//
//   make -C web && node tests/test_wasm_filen.mjs build-web/cwtty.wasm

import { readFileSync } from 'node:fs';
import crypto from 'node:crypto';

const wasmPath = process.argv[2] ?? 'build-web/cwtty.wasm';
const vectors = Object.fromEntries(
  [...readFileSync(new URL('./filen_vectors.h', import.meta.url), 'utf-8').matchAll(/^#define (FV_\w+) (".*")$/gm)]
    .map(([, k, v]) => [k, JSON.parse(v)]));

let failures = 0;
const check = (ok, what) => {
  if (!ok) {
    failures++;
    console.log(`  FAIL ${what}`);
  }
};

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), { math: Math });
const w = instance.exports;
const heap = () => new Uint8Array(w.memory.buffer);
const enc = new TextEncoder(), dec = new TextDecoder();
const str = (p) => {
  const h = heap();
  let e = p;
  while (h[e]) e++;
  return dec.decode(h.subarray(p, e));
};
const putIn = (bytes) => heap().set(bytes, w.cw_cloud_in());

// ---- Filen's metadata, done by Node ----
const key002 = (key) => crypto.pbkdf2Sync(key, key, 1, 32, 'sha512');
function meta002(key, plain) {
  const iv = Buffer.from('AbCdEfGhIjKl');
  const c = crypto.createCipheriv('aes-256-gcm', key002(key), iv);
  const ct = Buffer.concat([c.update(plain, 'utf-8'), c.final(), c.getAuthTag()]);
  return `002${iv}${ct.toString('base64')}`;
}
function open002(key, meta) {
  if (!meta.startsWith('002')) throw new Error('not 002');
  const raw = Buffer.from(meta.slice(15), 'base64');
  const d = crypto.createDecipheriv('aes-256-gcm', key002(key), Buffer.from(meta.slice(3, 15)));
  d.setAuthTag(raw.subarray(raw.length - 16));
  return Buffer.concat([d.update(raw.subarray(0, raw.length - 16)), d.final()]).toString('utf-8');
}
const sha512 = (b) => crypto.createHash('sha512').update(b).digest('hex');

const DEK = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeef0';
const OLD_KEY = 'OldMasterKeyFromBefore2023xxxxxx';
const hmacKey = Buffer.from(vectors.FV_HMAC_KEY, 'hex');

// ---- A fake Filen ----
function fake(version) {
  const s = { version, folders: [], files: [], uploads: 0, bad: [] };
  const lastKey = version === 3 ? DEK : OLD_KEY;
  const nameHash = (n) => version === 3
    ? crypto.createHmac('sha256', hmacKey).update(n.toLowerCase()).digest('hex')
    : crypto.createHash('sha1').update(crypto.createHash('sha512').update(n.toLowerCase()).digest('hex')).digest('hex');
  const ok = (data = {}) => ({ status: 200, body: JSON.stringify({ status: true, message: 'ok', code: 'ok', data }) });
  const no = (code) => ({ status: 200, body: JSON.stringify({ status: false, message: code, code }) });
  s.serve = (method, url, headers, body) => {
    const u = new URL(url), path = u.pathname;
    if (u.host === 'egest.filen.io') {
      const f = s.files.find((f) => url.includes(f.uuid));
      return f ? { status: 200, bytes: f.chunk } : { status: 404, body: 'no' };
    }
    if (u.host === 'ingest.filen.io') {
      const p = Object.fromEntries(u.searchParams);
      if (sha512(body) !== p.hash || sha512(JSON.stringify(p)) !== headers.Checksum) s.bad.push('upload checksum');
      s.files.push({ uuid: p.uuid, parent: p.parent, uploadKey: p.uploadKey, chunk: Buffer.from(body) });
      return ok({ bucket: 'filen-1', region: 'de-1' });
    }
    const text = dec.decode(body ?? new Uint8Array());
    if (method === 'POST' && sha512(text) !== headers.Checksum) s.bad.push(`checksum of ${path}`);
    const d = method === 'POST' ? JSON.parse(text) : {};
    switch (path) {
      case '/v3/auth/info':
        return ok({ authVersion: version, salt: version === 3 ? vectors.FV_SALT_V3 : vectors.FV_SALT_V2 });
      case '/v3/login':
        if (d.password !== (version === 3 ? vectors.FV_V3_PASSWORD : vectors.FV_V2_PASSWORD)) return no('email_or_password_wrong');
        if (d.twoFactorCode === 'XXXXXX') return no('enter_2fa');
        return ok({ apiKey: 'key-1' });
      case '/v3/user/baseFolder':
        return ok({ uuid: 'base' });
      case '/v3/user/masterKeys':
        if (open002(vectors.FV_V2_MASTER_KEY, d.masterKeys) !== vectors.FV_V2_MASTER_KEY) s.bad.push('master keys');
        return ok({ keys: meta002(vectors.FV_V2_MASTER_KEY, `${OLD_KEY}|${vectors.FV_V2_MASTER_KEY}`) });
      case '/v3/user/dek':
        return ok({ dek: meta002(vectors.FV_V3_MASTER_KEY, DEK) });
      case '/v3/user/keyPair/info':
        return ok({ publicKey: 'x'.repeat(40), privateKey: meta002(DEK, vectors.FV_PRIVATE_KEY) });
      case '/v3/dir/exists': {
        const f = s.folders.find((f) => f.parent === d.parent && f.hash === d.nameHashed);
        return ok({ exists: !!f, uuid: f?.uuid ?? '' });
      }
      case '/v3/dir/create': {
        const name = JSON.parse(open002(lastKey, d.name)).name;
        if (d.nameHashed !== nameHash(name)) s.bad.push(`name hash of ${name}`);
        s.folders.push({ uuid: d.uuid, parent: d.parent, hash: d.nameHashed, name });
        return ok();
      }
      case '/v3/dir/content':
        return ok({
          folders: [],
          uploads: s.files.filter((f) => f.done && f.parent === d.uuid && !f.old)
            .map((f) => ({ uuid: f.uuid, metadata: f.metadata, timestamp: 1, bucket: 'filen-1', region: 'de-1', chunks: 1 })),
        });
      case '/v3/upload/done': {
        const f = s.files.find((f) => f.uuid === d.uuid);
        const meta = JSON.parse(open002(lastKey, d.metadata));
        if (!f || f.uploadKey !== d.uploadKey || open002(meta.key, d.name) !== meta.name ||
            d.nameHashed !== nameHash(meta.name) || Number(open002(meta.key, d.size)) !== meta.size) {
          s.bad.push('upload done');
          return no('bad');
        }
        // The chunk: version 2, the key's 32 characters, the IV as text.
        const c = f.chunk, dch = crypto.createDecipheriv('aes-256-gcm', Buffer.from(meta.key), c.subarray(0, 12));
        dch.setAuthTag(c.subarray(c.length - 16));
        const plain = Buffer.concat([dch.update(c.subarray(12, c.length - 16)), dch.final()]);
        if (sha512(plain) !== meta.hash || plain.length !== meta.size) s.bad.push('chunk');
        s.files.forEach((o) => { if (o !== f && o.parent === f.parent && o.hash === d.nameHashed) o.old = true; });
        Object.assign(f, { done: true, metadata: d.metadata, hash: d.nameHashed, plain: plain.toString('utf-8') });
        s.uploads++;
        return ok();
      }
    }
    s.bad.push(`unknown ${path}`);
    return no('unknown');
  };
  return s;
}

// Runs the wasm task against the fake: its final state.
function drive(server) {
  for (let guard = 0; guard < 100; guard++) {
    const st = w.cw_cloud_run();
    if (st !== 1) return st; // FILEN_WAIT
    const headers = {};
    if (str(w.cw_cloud_auth())) headers.Authorization = str(w.cw_cloud_auth());
    if (str(w.cw_cloud_checksum())) headers.Checksum = str(w.cw_cloud_checksum());
    const post = w.cw_cloud_post();
    const body = post ? heap().slice(w.cw_cloud_body(), w.cw_cloud_body() + w.cw_cloud_body_len()) : undefined;
    const r = server.serve(post ? 'POST' : 'GET', str(w.cw_cloud_url()), headers, body);
    const bytes = r.bytes ?? enc.encode(r.body);
    heap().set(bytes, w.cw_cloud_resp());
    w.cw_cloud_reply(r.status, bytes.length);
  }
  return -1;
}

const login = (email, pw, code) => {
  putIn(enc.encode(`${email}\0${pw}\0${code}\0`));
  w.cw_cloud_login();
};

// Statistics to sync: this device's id, and answers from an import.
function statsFrom(text) {
  heap().set(enc.encode(`${text}\0`), w.cw_stats_buf());
  return w.cw_stats_preview() === 0 && (w.cw_stats_import(), true);
}
w.cw_init(7);
heap().set(enc.encode('abcd1234\0'), w.cw_buf());
w.cw_stats_init(w.cw_buf());
putIn(crypto.randomBytes(32));
w.cw_cloud_seed();

for (const version of [2, 3]) {
  console.log(`wasm filen: version ${version}`);
  const server = fake(version);
  w.cw_cloud_sign_out();
  login('op@example.org', vectors.FV_PASSWORD, '');
  check(drive(server) === 3 && w.cw_cloud_need_code() === 1, `v${version} asks for the code`);
  login('op@example.org', vectors.FV_PASSWORD, '123456');
  const t0 = performance.now();
  check(drive(server) === 2 && w.cw_cloud_signed_in() === 1, `v${version} sign in: ${str(w.cw_cloud_error())}`);
  console.log(`  signed in (${Math.round(performance.now() - t0)} ms of key derivation and requests)`);
  check(server.folders.map((f) => f.name).join('/') === 'CWtty/stats-v1', `v${version} folders`);
  const session = str(w.cw_cloud_session());
  check(!session.includes(vectors.FV_PASSWORD) && session.includes('api_key=key-1'), `v${version} session`);
  if (version === 3) check(session.includes(`hmac_key=${vectors.FV_HMAC_KEY}`) && session.includes(`master_key=${DEK}`), 'v3 keys');

  // Answers here, then a sync: the section goes up, readable by Node.
  check(statsFrom('copy_answers=3\ncopy_correct=2\n'), 'import');
  check(w.cw_cloud_sync(Date.now()) === 1 && drive(server) === 2 && w.cw_cloud_merge() === 0 && server.uploads === 1 &&
    server.files[0].plain.startsWith('[device abcd1234 rev='), `v${version} sync: ${str(w.cw_cloud_error())}`);
  check(server.bad.length === 0, `v${version} protocol: ${server.bad.join(', ')}`);

  // Another device's file comes down and joins the total.
  const other = '[device cafe02 rev=4 name=Terminal]\ncopy_answers=9\ncopy_correct=8\n';
  server.files.push({ uuid: 'f-other', parent: server.folders[1].uuid, done: true, chunk: null });
  {
    const key = 'K'.repeat(32), iv = Buffer.from('IvIvIvIvIvIv');
    const c = crypto.createCipheriv('aes-256-gcm', Buffer.from(key), iv);
    const body = Buffer.concat([iv, c.update(other), c.final(), c.getAuthTag()]);
    const f = server.files[server.files.length - 1];
    f.chunk = body;
    f.metadata = meta002(version === 3 ? DEK : OLD_KEY, JSON.stringify({ name: 'cafe02.stats', size: other.length, mime: 'text/plain', key }));
  }
  check(w.cw_cloud_sync(Date.now()) === 1 && drive(server) === 2 && w.cw_cloud_merge() === 1 && w.cw_stat_devices() === 2,
    `v${version} the other device: ${str(w.cw_cloud_error())}`);
  check(server.bad.length === 0, `v${version} protocol: ${server.bad.join(', ')}`);
}

console.log(failures ? `${failures} check(s) failed` : 'all tests passed');
process.exit(failures ? 1 : 0);
