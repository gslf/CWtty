// install.js - Offers to install the app while it runs in a browser tab:
// the browser's own prompt where there is one (Chrome, Edge, Android), the
// steps to follow in Safari. app.js imports it first, before anything is
// awaited: the browser announces the prompt only once.

const agent = navigator.userAgent;
const ios = /iPhone|iPad|iPod/.test(agent) || (/Macintosh/.test(agent) && navigator.maxTouchPoints > 1);
const safariMac = !ios && /Macintosh/.test(agent) && Number(agent.match(/Version\/(\d+).*Safari/)?.[1]) >= 17;
const STEPS = ios ? 'To install CWtty tap Share, then "Add to Home Screen"'
            : safariMac ? 'To install CWtty choose File, then "Add to Dock"'
            : null;

let prompt = null; // the browser's install prompt, while it offers one
let buttons = [];

const installed = () => matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;

function update() {
  for (const button of buttons) button.hidden = installed() || !(prompt || STEPS);
}

addEventListener('beforeinstallprompt', (e) => {
  e.preventDefault(); // the app's own button instead of the browser's banner
  prompt = e;
  update();
});

addEventListener('appinstalled', () => {
  prompt = null;
  update();
});

async function install(toast) {
  if (!prompt) {
    toast(STEPS, false, 8000);
    return;
  }
  prompt.prompt();
  const { outcome } = await prompt.userChoice;
  prompt = null; // usable once: the browser offers a new one if it wants
  update();
  if (outcome === 'accepted') toast('Installed: open CWtty from your apps');
}

// Shows the elements while the app can be installed; toast(text, warn, ms)
// tells the steps.
export function offerInstall(elements, toast) {
  buttons = [...elements];
  for (const button of buttons) button.addEventListener('click', () => install(toast));
  update();
}
