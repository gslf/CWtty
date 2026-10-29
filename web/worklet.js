// worklet.js - The audio thread: runs the transmitter engine and the capture
// resampler of cwtty.wasm (wasm/worklet.c). Reports the operator's key
// events, the clock and the 8 kHz capture to the page every 20 ms, and at
// once when the key changes.

const REPORT_S = 0.02;

class CWtty extends AudioWorkletProcessor {
  constructor({ processorOptions: { wasm } }) {
    super();
    const w = new WebAssembly.Instance(new WebAssembly.Module(wasm), { math: Math }).exports;
    w.wk_init(sampleRate);
    this.w = w;
    this.capturing = false;
    this.due = 0;
    const commands = {
      key: w.wk_key,
      enable: w.wk_enable,
      params: w.wk_params,
      stop: w.wk_stop,
      play: (bytes, wpm, effWpm, leadMs) => {
        const text = new Uint8Array(w.memory.buffer, w.wk_text(), 1024);
        text.set(bytes.subarray(0, 1023));
        text[Math.min(bytes.length, 1023)] = 0;
        w.wk_play(wpm, effWpm, leadMs);
      },
      capture: (on) => (this.capturing = !!on),
    };
    this.port.onmessage = ({ data: [command, ...args] }) => commands[command](...args);
  }

  process(inputs, [[out]]) {
    const w = this.w, input = inputs[0][0];
    // The memory never grows: views on it stay valid.
    this.input ??= new Float32Array(w.memory.buffer, w.wk_input(), out.length);
    this.output ??= new Float32Array(w.memory.buffer, w.wk_output(), out.length);
    if (this.capturing && input) {
      this.input.set(input);
      w.wk_capture(input.length);
    }
    w.wk_render(out.length);
    out.set(this.output);

    const events = w.wk_take_events();
    if (events || currentTime >= this.due) {
      this.due = currentTime + REPORT_S;
      this.port.postMessage({
        clock: w.wk_clock_ms(),
        busy: w.wk_busy(),
        idle: w.wk_idle_ms(),
        playing: w.wk_playing(),
        events: new Float64Array(w.memory.buffer, w.wk_events(), 3 * events).slice(),
        rx: new Float32Array(w.memory.buffer, w.wk_rx(), w.wk_take_rx()).slice(),
      });
    }
    return true;
  }
}

registerProcessor('cwtty', CWtty);
