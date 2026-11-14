// sw.js - Works offline: answers from the cache at once and refreshes it
// from the network, so a new version shows up on the next start. Both ask
// the server past the browser's HTTP cache, which could otherwise keep an
// old file of the app next to new ones.

const CACHE = 'cwtty';
const FILES = ['./', 'index.html', 'style.css', 'app.js', 'audio.js', 'net.js', 'cloud.js', 'worklet.js', 'install.js', 'cwtty.wasm',
               'manifest.webmanifest', 'icon.svg', 'icon-32.png', 'icon-192.png', 'icon-512.png',
               'apple-touch-icon.png'];

addEventListener('install', (e) =>
  e.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(FILES.map((f) => new Request(f, { cache: 'reload' }))))));

addEventListener('fetch', (e) => {
  if (e.request.method !== 'GET' || new URL(e.request.url).origin !== location.origin) return;
  // no-cache: the server says whether the file changed (a short 304 if not).
  const fresh = fetch(e.request.url, { cache: 'no-cache' }).then(async (response) => {
    if (response.ok) await (await caches.open(CACHE)).put(e.request, response.clone());
    return response;
  });
  e.waitUntil(fresh.catch(() => {}));
  e.respondWith(caches.match(e.request).then((cached) => cached ?? fresh));
});
