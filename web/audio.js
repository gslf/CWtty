// audio.js - The audio graph: one AudioWorklet node (worklet.js) plays the
// sidetone and takes the microphone.

let ctx, node, stream, source;

export async function start(wasm, report) {
  ctx = new AudioContext({ latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('worklet.js');
  node = new AudioWorkletNode(ctx, 'cwtty', {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    outputChannelCount: [1],
    processorOptions: { wasm },
  });
  node.port.onmessage = (e) => report(e.data);
  node.connect(ctx.destination);
  // Browsers start audio only from a user gesture.
  for (const type of ['pointerdown', 'keydown']) addEventListener(type, () => ctx.resume(), true);
}

export const running = () => ctx?.state === 'running';

export const send = (...message) => node?.port.postMessage(message);

// Returns false when the microphone cannot be opened.
export async function microphone(on) {
  if (on === !!stream) return true;
  if (!on) {
    source.disconnect();
    for (const track of stream.getTracks()) track.stop();
    stream = source = null;
    send('capture', 0);
    return true;
  }
  try {
    // A CW tone is exactly what noise suppression removes.
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
  } catch {
    return false;
  }
  source = ctx.createMediaStreamSource(stream);
  source.connect(node);
  send('capture', 1);
  return true;
}
