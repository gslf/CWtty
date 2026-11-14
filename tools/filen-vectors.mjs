#!/usr/bin/env node
// Development only. Computes reference values with Filen's official SDK so
// that tests/test_filen.c can check src/fcrypto.c and src/filen.c byte for
// byte: key derivation (v2, v3), metadata (002, 003), name hashes, file
// chunks, the HMAC key and the Checksum header.
//
// The SDK (@filen/sdk, AGPL-3.0) is not part of CWtty and is not needed to
// build or test it: only the values it printed are kept, in
// tests/filen_vectors.h. To regenerate them, install the SDK anywhere and
// point this script at it:
//
//   node tools/filen-vectors.mjs /path/to/node_modules/@filen/sdk > tests/filen_vectors.h
//
// Its randomness is replaced by fixed bytes, so the output is stable.

import { createRequire } from 'node:module';
import nodeCrypto from 'node:crypto';

const sdk = process.argv[2];
if (!sdk) {
  console.error('usage: node tools/filen-vectors.mjs <path to @filen/sdk>');
  process.exit(2);
}
const require = createRequire(`${sdk}/package.json`);
const utils = require(`${sdk}/dist/node/crypto/utils.js`);
const { Encrypt } = require(`${sdk}/dist/node/crypto/encrypt.js`);
const { Decrypt } = require(`${sdk}/dist/node/crypto/decrypt.js`);
const { parseURLParams } = require(`${sdk}/dist/node/utils.js`);
const version = require(`${sdk}/package.json`).version;

const enc = new Encrypt({ config: {} });
const dec = new Decrypt({ config: {} });

// The SDK's randomness, fixed: randomBytes(n) gives 0, 1, 2... from `start`.
async function fixed(start, fn) {
  const real = nodeCrypto.randomBytes;
  nodeCrypto.randomBytes = (n) => Buffer.from(Array.from({ length: n }, (_, i) => (start + i * 37) & 255));
  try {
    return await fn();
  } finally {
    nodeCrypto.randomBytes = real;
  }
}

const out = [];
const cstr = (s) => JSON.stringify(s); // a C string literal for ASCII and escapes alike
const def = (name, value) => out.push(`#define ${name} ${cstr(value)}`);

const pw = 'Pässwörd "quoted" ✓ \\ 73';
const saltV2 = 'qG3fXbZp9LkR2mWcT7vYhN4sJ8dEuA1o';
const saltV3 = '0f1e2d3c4b5a69788796a5b4c3d2e1f00112233445566778899aabbccddeeff0';

const v2 = await utils.generatePasswordAndMasterKeyBasedOnAuthVersion({ rawPassword: pw, authVersion: 2, salt: saltV2 });
const v3 = await utils.generatePasswordAndMasterKeyBasedOnAuthVersion({ rawPassword: pw, authVersion: 3, salt: saltV3 });

const key2 = 'kG5vP8rT2wQ9zX4nB7mL1cF6hJ3sD0yA'; // a 32-character key (002, file v2)
const key3 = 'a3f1c2e4b5d6978899aabbccddeeff00112233445566778899aabbccddeeff01'; // 64 hex (003)
const meta = '{"name":"stats-v1"}';
const meta002 = await fixed(7, () => enc.metadata({ metadata: meta, key: key2, version: 2 }));
const meta003 = await fixed(11, () => enc.metadata({ metadata: meta, key: key3, version: 3 }));
const back002 = await dec.metadata({ metadata: meta002, key: key2 });
const back003 = await dec.metadata({ metadata: meta003, key: key3 });
if (back002 !== meta || back003 !== meta) throw new Error('the SDK does not read its own metadata');

const privateKey = Buffer.from(Array.from({ length: 48 }, (_, i) => (i * 29 + 5) & 255)).toString('base64');
const hmacKey = await utils.generatePrivateKeyHMAC(privateKey);
const nameV2 = await utils.hashFileName({ name: 'Stats-V1', authVersion: 2 });
const nameV3 = await utils.hashFileName({ name: 'Stats-V1', authVersion: 3, hmacKey });

const chunk = Buffer.from('[device 0123456789abcdef rev=3 name=Terminal]\ncopy_answers=12\n', 'utf-8');
const chunkEnc = await fixed(3, () => enc.data({ data: chunk, key: key2, version: 2 }));

const body = {
  email: 'op@example.org',
  password: 'x"y\\z\n\t\u0001é✓',
  twoFactorCode: 'XXXXXX',
  authVersion: 2,
  ok: true,
  list: [1, 'two', false],
};
const bodyJSON = JSON.stringify(body);
const bodyChecksum = nodeCrypto.createHash('sha512').update(Buffer.from(bodyJSON, 'utf-8')).digest('hex');

const uploadURL = 'https://ingest.filen.io/v3/upload?uuid=0b1c2d3e-4f50-4617-8293-a4b5c6d7e8f9&index=0' +
  '&parent=11111111-2222-4333-8444-555555555555&uploadKey=UpLoAdKeY0123456789abcdefghijklm' +
  `&hash=${nodeCrypto.createHash('sha512').update(chunkEnc).digest('hex')}`;
const uploadChecksum = nodeCrypto.createHash('sha512').update(Buffer.from(JSON.stringify(parseURLParams({ url: uploadURL })), 'utf-8')).digest('hex');

out.push('/*');
out.push(` * filen_vectors.h - Reference values from Filen's official SDK ${version},`);
out.push(' * printed by tools/filen-vectors.mjs (see there). Generated: do not edit.');
out.push(' */');
out.push('#ifndef FILEN_VECTORS_H');
out.push('#define FILEN_VECTORS_H');
out.push('');
def('FV_PASSWORD', pw);
def('FV_SALT_V2', saltV2);
def('FV_SALT_V3', saltV3);
def('FV_V2_MASTER_KEY', v2.derivedMasterKeys);
def('FV_V2_PASSWORD', v2.derivedPassword);
def('FV_V3_MASTER_KEY', v3.derivedMasterKeys);
def('FV_V3_PASSWORD', v3.derivedPassword);
out.push('');
def('FV_KEY2', key2);
def('FV_KEY3', key3);
def('FV_META', meta);
def('FV_META_002', meta002); // IV from the fixed bytes 7, 44, 81...
def('FV_META_003', meta003); // IV the fixed bytes 11, 48, 85...
out.push('');
def('FV_PRIVATE_KEY', privateKey);
def('FV_HMAC_KEY', Buffer.from(hmacKey).toString('hex'));
def('FV_NAME_V2', nameV2);
def('FV_NAME_V3', nameV3);
out.push('');
def('FV_CHUNK', chunk.toString('utf-8'));
def('FV_CHUNK_ENC', chunkEnc.toString('hex')); // IV from the fixed bytes 3, 40, 77...
out.push('');
def('FV_BODY_JSON', bodyJSON);
def('FV_BODY_CHECKSUM', bodyChecksum);
def('FV_UPLOAD_URL', uploadURL);
def('FV_UPLOAD_CHECKSUM', uploadChecksum);
out.push('');
out.push('#endif');
console.log(out.join('\n'));
