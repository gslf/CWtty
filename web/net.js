// net.js - The online room's transport in the browser: a WebSocket per MQTT
// broker. The MQTT sessions, the room protocol and the timing live in
// cwtty.wasm (src/mqtt.c, src/link.c, src/room.c, wasm/page.c); this only
// opens and closes the sockets it is told to and moves the bytes.

export function createNet(w, str) {
  const n = w.cw_net_brokers();
  const sockets = new Array(n).fill(null);

  function open(i) {
    close(i);
    let ws;
    try {
      ws = new WebSocket(str(w.cw_net_broker_url(i)), 'mqtt');
    } catch {
      w.cw_net_closed(i);
      return;
    }
    ws.binaryType = 'arraybuffer';
    // Events of a socket that was replaced meanwhile are stale.
    ws.onopen = () => sockets[i] === ws && w.cw_net_opened(i);
    ws.onmessage = (e) => {
      if (sockets[i] !== ws) return;
      const data = new Uint8Array(e.data), cap = w.cw_net_in_cap();
      for (let at = 0; at < data.length; at += cap) {
        const chunk = data.subarray(at, at + cap);
        new Uint8Array(w.memory.buffer, w.cw_net_in_buf(), chunk.length).set(chunk);
        w.cw_net_input(i, chunk.length);
      }
    };
    ws.onclose = () => {
      if (sockets[i] !== ws) return;
      sockets[i] = null;
      w.cw_net_closed(i);
    };
    sockets[i] = ws;
  }

  function close(i) {
    const ws = sockets[i];
    sockets[i] = null;
    if (ws) ws.close();
  }

  // Carries out what cw_net_update() asked, then sends what each link has.
  function pump(actions) {
    for (let i = 0; i < n; i++) {
      if (actions & (1 << (2 * i))) open(i);
      else if (actions & (2 << (2 * i))) close(i);
    }
    for (let i = 0; i < n; i++) {
      const ws = sockets[i];
      if (ws?.readyState !== WebSocket.OPEN) continue;
      const len = w.cw_net_out(i);
      if (len) ws.send(new Uint8Array(w.memory.buffer, w.cw_net_out_buf(), len).slice());
    }
  }

  return { pump };
}
