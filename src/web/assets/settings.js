// Stream settings: changes go to the bridge, which saves them on the laptop
// and passes them to every stream page at once (OBS and the preview here).

import { connectBridge, withDefaults, streamBibles } from './bridge.js';

const $ = (id) => document.getElementById(id);

let style = withDefaults({});
let hidden = false;
let hello = null;
let live = null;
let connected = false;
let lastEdit = 0;
let sendTimer = null;

const bridge = connectBridge({
  onMessage(m) {
    if (m.type === 'hello') hello = m;
    else if (m.type === 'style') {
      hidden = !!m.hidden;
      // While a slider is moving, the bridge echoes older values; keep ours.
      if (performance.now() - lastEdit > 600) style = withDefaults(m.style);
    } else if (m.type === 'live') live = m;
    else return;
    render();
  },
  onStatus(up) {
    connected = up;
    render();
  },
});

function edit(change) {
  change(style);
  lastEdit = performance.now();
  render();
  if (!sendTimer) {
    sendTimer = setTimeout(() => {
      sendTimer = null;
      bridge.send({ type: 'setStyle', style });
    }, 60);
  }
}

// ---------------------------------------------------------------------------
// Rendering

function render() {
  $('conn').classList.toggle('ok', connected);
  $('connText').textContent = connected ? (hello ? hello.host : 'Connected') : 'Not connected';

  const label = liveLabel(live);
  $('liveChip').hidden = !label;
  $('liveText').textContent = label;
  $('sampleSeg').hidden = !!label;

  const hideBtn = $('hideBtn');
  hideBtn.setAttribute('aria-pressed', String(hidden));
  hideBtn.textContent = hidden ? 'Show on stream' : 'Hide from stream';
  $('hiddenBadge').hidden = !hidden;

  document.querySelectorAll('[data-show]').forEach((cb) => { cb.checked = !!style.show[cb.dataset.show]; });
  $('reference').checked = !!style.reference;
  $('shadow').checked = !!style.shadow;
  setFont(style.font);
  setRange('size', style.size, ' px');
  setRange('fadeIn', style.fadeIn, ' ms');
  setRange('fadeOut', style.fadeOut, ' ms');
  document.querySelectorAll('[data-key]').forEach((group) => {
    const value = String(style[group.dataset.key]).toLowerCase();
    group.querySelectorAll('button').forEach((b) => {
      b.setAttribute('aria-pressed', String(b.dataset.value.toLowerCase() === value));
    });
  });
  renderBibles();
  renderAddress();
}

function setRange(id, value, unit) {
  const input = $(id);
  if (document.activeElement !== input) input.value = value;
  $(id + 'Out').textContent = value + unit;
}

function setFont(font) {
  const select = $('font');
  if (![...select.options].some((o) => o.value === font)) select.add(new Option(font));
  select.value = font;
}

function renderBibles() {
  const list = $('bibleList');
  if (!hello) return;
  if (list.childElementCount !== hello.bibles.length) {
    list.replaceChildren(...hello.bibles.map((b) => {
      const label = document.createElement('label');
      label.className = 'check';
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.dataset.bible = b.id;
      label.append(cb, b.abbr || b.name);
      return label;
    }));
  }
  const chosen = streamBibles(style, live, hello);
  list.querySelectorAll('input').forEach((cb) => { cb.checked = chosen.includes(cb.dataset.bible); });
}

function renderAddress() {
  // Opened on the laptop itself, the page's own address would be "localhost".
  const local = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname);
  const host = local && hello ? `${hello.host.toLowerCase()}:${hello.port}` : location.host;
  $('obsUrl').textContent = `http://${host}/stream`;
}

function liveLabel(m) {
  if (!m || !m.visible || m.kind === 'none') return '';
  switch (m.kind) {
    case 'song': {
      const s = m.song;
      return `${[s.number > 0 ? s.number : '', s.title].filter(Boolean).join(' · ')} (${s.index + 1}/${s.count})`;
    }
    case 'bible': {
      const all = m.bible.versions;
      const v = all.find((x) => x.id === m.bible.projected[0]) || all[0];
      return v ? v.reference : 'Bible';
    }
    case 'announcement': return m.announcement.title || 'Announcement';
    case 'picture': return m.picture.name || `Picture ${m.picture.index + 1}/${m.picture.count}`;
    case 'video': return m.video.name || 'Video';
    default: return '';
  }
}

// ---------------------------------------------------------------------------
// Controls

document.querySelectorAll('[data-show]').forEach((cb) => {
  cb.addEventListener('change', () => edit((s) => { s.show[cb.dataset.show] = cb.checked; }));
});
$('reference').addEventListener('change', (e) => edit((s) => { s.reference = e.target.checked; }));
$('shadow').addEventListener('change', (e) => edit((s) => { s.shadow = e.target.checked; }));
$('font').addEventListener('change', (e) => edit((s) => { s.font = e.target.value; }));
for (const id of ['size', 'fadeIn', 'fadeOut']) {
  $(id).addEventListener('input', (e) => edit((s) => { s[id] = Number(e.target.value); }));
}
document.querySelectorAll('[data-key]').forEach((group) => {
  group.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-value]');
    if (!b) return;
    const key = group.dataset.key;
    edit((s) => { s[key] = key === 'weight' ? Number(b.dataset.value) : b.dataset.value; });
  });
});

$('bibleList').addEventListener('change', (e) => {
  const id = e.target.dataset.bible;
  if (!id) return;
  const chosen = new Set(streamBibles(style, live, hello));
  if (e.target.checked) chosen.add(id); else chosen.delete(id);
  if (!chosen.size) {
    e.target.checked = true;   // the stream always shows at least one Bible
    return;
  }
  edit((s) => { s.bibles = hello.bibles.map((b) => b.id).filter((b) => chosen.has(b)); });
});

$('hideBtn').addEventListener('click', () => bridge.send({ type: 'setHidden', hidden: !hidden }));

$('sampleSeg').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-sample]');
  if (!b) return;
  $('sampleSeg').querySelectorAll('button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
  $('preview').src = `/stream?preview=${b.dataset.sample}`;
});

$('copyBtn').addEventListener('click', async () => {
  const text = $('obsUrl').textContent;
  let ok = false;
  try {
    await navigator.clipboard.writeText(text);
    ok = true;
  } catch {
    // Plain http on the network is not a secure context: no clipboard API.
    const area = document.createElement('textarea');
    area.value = text;
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.append(area);
    area.select();
    ok = document.execCommand('copy');
    area.remove();
  }
  $('copyBtn').textContent = ok ? 'Copied' : 'Copy';
  setTimeout(() => { $('copyBtn').textContent = 'Copy'; }, 1500);
});

// The preview is the real stream page at 1920x1080, scaled to fit.
const previewBox = $('previewBox');
new ResizeObserver(() => {
  $('preview').style.transform = `scale(${previewBox.clientWidth / 1920})`;
}).observe(previewBox);

render();
