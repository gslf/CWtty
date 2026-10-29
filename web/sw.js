// sw.js - Works offline: answers from the cache at once and refreshes it
// from the network, so a new version shows up on the next start.

const CACHE = 'cwtty';
const FILES = ['./', 'index.html', 'style.css', 'app.js', 'audio.js', 'worklet.js', 'install.js', 'cwtty.wasm',
               'manifest.webmanifest', 'icon.svg', 'icon-32.png', 'icon-192.png', 'icon-512.png',
               'apple-touch-icon.png'];

addEventListener('install', (e) => e.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(FILES))));

addEventListener('fetch', (e) => {
  if (e.request.method !== 'GET') return;
  const fresh = fetch(e.request).then(async (response) => {
    if (response.ok) await (await caches.open(CACHE)).put(e.request, response.clone());
    return response;
  });
  e.waitUntil(fresh.catch(() => {}));
  e.respondWith(caches.match(e.request).then((cached) => cached ?? fresh));
});
