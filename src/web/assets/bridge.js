// Connection to the CGF Projector network bridge: one WebSocket on the host
// that served the page. Reconnects by itself; pages only react to messages.
// Protocol: docs/network-bridge.md

export const defaultStyle = {
  show: { song: true, bible: true, announcement: true, picture: true, video: true },
  bibles: [],          // Bible ids to show; empty = the projector's first Bible
  reference: true,
  layout: 'lower',     // 'lower' | 'full'
  lines: 'balance',    // 'balance' | 'keep' (song lines as projected)
  font: 'Segoe UI',
  weight: 600,
  size: 44,            // px on a 1080p frame
  color: '#FFFFFF',
  shadow: true,
  bar: 'soft',         // 'none' | 'soft' | 'solid'
  fadeIn: 450,         // ms
  fadeOut: 300,
};

export function withDefaults(style) {
  const s = Object.assign({}, defaultStyle, style || {});
  s.show = Object.assign({}, defaultStyle.show, (style && style.show) || {});
  s.bibles = Array.isArray(s.bibles) ? s.bibles.map(String) : [];
  return s;
}

// The Bibles a page shows: the chosen ones, else the projector's first.
export function streamBibles(style, live, hello) {
  if (style.bibles.length) return style.bibles;
  const first = (live && live.bibles && live.bibles[0]) || (hello && hello.bibles[0] && hello.bibles[0].id);
  return first ? [String(first)] : [];
}

export function connectBridge({ onMessage, onStatus }) {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  let ws = null;
  let retry = null;
  let watchdog = null;
  let delay = 500;

  function open() {
    clearTimeout(retry);
    const socket = new WebSocket(url);
    ws = socket;
    socket.onopen = () => {
      delay = 500;
      arm();
      onStatus && onStatus(true);
    };
    socket.onmessage = (e) => {
      arm();
      let m;
      try { m = JSON.parse(e.data); } catch { return; }
      if (m.type !== 'tick') onMessage(m);
    };
    socket.onclose = () => drop(socket);
    socket.onerror = () => drop(socket);
  }

  function drop(socket) {
    if (socket !== ws) return;
    ws = null;
    clearTimeout(watchdog);
    socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
    try { socket.close(); } catch { /* already closed */ }
    onStatus && onStatus(false);
    retry = setTimeout(open, delay);
    delay = Math.min(delay * 2, 5000);
  }

  // The bridge ticks every 10 s; silence means the laptop went away.
  function arm() {
    clearTimeout(watchdog);
    const socket = ws;
    watchdog = setTimeout(() => drop(socket), 25000);
  }

  open();
  return {
    send(message) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return false;
      ws.send(JSON.stringify(message));
      return true;
    },
  };
}
