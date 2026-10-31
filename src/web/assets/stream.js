// The OBS stream overlay. Shows what the projector shows, restyled for the
// stream: its own Bibles, font, size, line breaks and fades. The settings
// page preview loads this page with ?preview=bible|song, which shows a
// sample whenever nothing is live.

import { connectBridge, withDefaults, streamBibles } from './bridge.js';

const sampleKind = new URLSearchParams(location.search).get('preview');
const songSample = 'Amazing grace! How sweet the sound\nThat saved a wretch like me!\n'
  + 'I once was lost, but now am found;\nWas blind, but now I see.';

const root = document.documentElement;
const textLayer = document.getElementById('text');
const mediaLayer = document.getElementById('media');
const canBalance = CSS.supports('text-wrap', 'balance');

let style = withDefaults({});
let hidden = false;
let hello = null;
let live = null;
let video = null;      // last playback state, with the time it arrived
let online = false;
let lostTimer = null;

connectBridge({
  onMessage(m) {
    if (m.type === 'hello') hello = m;
    else if (m.type === 'style') {
      style = withDefaults(m.style);
      hidden = !!m.hidden;
    } else if (m.type === 'live') live = m;
    else if (m.type === 'video') video = Object.assign({ receivedAt: performance.now() }, m);
    else return;
    update();
    if (m.type === 'video') syncVideo();
  },
  onStatus(up) {
    clearTimeout(lostTimer);
    if (up) {
      online = true;
      update();
    } else {
      // A short Wi-Fi drop keeps the overlay; a laptop that is gone clears it.
      lostTimer = setTimeout(() => { online = false; update(); }, 10000);
    }
  },
});

// ---------------------------------------------------------------------------
// What to show

function currentItem() {
  const nothingLive = !live || !live.visible || live.kind === 'none';
  if (sampleKind && nothingLive) return sampleItem();
  return live;
}

function sampleItem() {
  if (sampleKind === 'song') return { visible: true, kind: 'song', song: { text: songSample } };
  if (hello && hello.sample && hello.sample.versions.length) {
    return { visible: true, kind: 'bible', bible: hello.sample, bibles: live && live.bibles };
  }
  return null;
}

function splitLines(text) {
  return String(text || '').split('\n').map((l) => l.trim()).filter(Boolean);
}

// Content only. Style is applied when rendering, so a style change restyles
// what is on screen without fading it.
function viewFor() {
  if (!online || hidden) return null;
  const item = currentItem();
  if (!item || !item.visible) return null;
  const show = style.show;
  switch (item.kind) {
    case 'song':
      if (!show.song || item.cleared) return null;
      return { kind: 'text', blocks: [{ song: true, lines: splitLines(item.song.text) }] };
    case 'announcement':
      if (!show.announcement || item.cleared) return null;
      return { kind: 'text', blocks: [{ lines: splitLines(item.announcement.text) }] };
    case 'bible': {
      if (!show.bible || item.cleared) return null;
      const all = item.bible.versions;
      let versions = streamBibles(style, item, hello)
        .map((id) => all.find((v) => v.id === id)).filter(Boolean);
      if (!versions.length && all.length) versions = [all[0]];
      if (!versions.length) return null;
      return {
        kind: 'text',
        refs: versions.map((v) => v.reference),
        blocks: versions.map((v) => ({ verses: v.verses })),
      };
    }
    case 'picture':
      if (!show.picture) return null;
      return { kind: 'picture', url: item.picture.url };
    case 'video':
      if (!show.video || item.video.audioOnly) return null;
      if (!video || video.seq !== item.video.seq || video.state === 'stopped') return null;
      return { kind: 'video', url: item.video.url, seq: item.video.seq };
    default:
      return null;
  }
}

// ---------------------------------------------------------------------------
// Fades: the old content fades out, then the new one fades in. Changes that
// arrive meanwhile skip straight to the newest.

let shown = null;
let shownKey = '';
let target = null;
let targetKey = '';
let running = false;
let cutFadeIn = null;

function update() {
  const view = viewFor();
  const key = view ? JSON.stringify(view) : '';
  if (key === targetKey) {
    if (key === shownKey && shown && shown.kind === 'text') renderText(shown);
    return;
  }
  target = view;
  targetKey = key;
  if (cutFadeIn) cutFadeIn();
  run();
}

async function run() {
  if (running) return;
  running = true;
  while (shownKey !== targetKey) {
    if (shown) {
      await fade(layerOf(shown), false);
      clearLayer(shown);
      shown = null;
      shownKey = '';
      continue;
    }
    const view = target;
    const key = targetKey;
    if (!view) break;
    if (view.kind === 'text') renderText(view);
    else await loadMedia(view);
    if (key !== targetKey) {
      clearLayer(view);
      continue;
    }
    shown = view;
    shownKey = key;
    if (view.kind === 'video') syncVideo();
    await fade(layerOf(view), true);
  }
  running = false;
}

function layerOf(view) {
  return view.kind === 'text' ? textLayer : mediaLayer;
}

function fade(layer, on) {
  const ms = Math.max(0, Number(on ? style.fadeIn : style.fadeOut) || 0);
  layer.style.transitionDuration = ms + 'ms';
  layer.classList.toggle('on', on);
  return new Promise((resolve) => {
    const timer = setTimeout(done, ms + 20);
    function done() {
      clearTimeout(timer);
      cutFadeIn = null;
      resolve();
    }
    // A fade-in gives way at once to newer content; a fade-out always finishes.
    cutFadeIn = on ? done : null;
  });
}

function clearLayer(view) {
  const layer = layerOf(view);
  const v = layer.querySelector('video');
  if (v) {
    v.pause();
    v.removeAttribute('src');
    v.load();
  }
  layer.replaceChildren();
}

// ---------------------------------------------------------------------------
// Text

const escapeHtml = (s) => String(s).replace(/[&<>"]/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// NKJV marks words the translators supplied with [brackets]; print them in
// italics, as the printed Bible does.
const italics = (html) => html.replace(/\[([^\]]*)\]/g, '<em>$1</em>');

function versesHtml(verses) {
  if (verses.length === 1) return italics(escapeHtml(verses[0].text));
  return verses.map((v) => `<sup>${v.verse}</sup>${italics(escapeHtml(v.text))}`).join(' ');
}

function renderText(view) {
  const s = style;
  const full = s.layout === 'full';
  textLayer.classList.toggle('full', full);
  textLayer.classList.toggle('lower', !full);
  textLayer.classList.toggle('shadow', !!s.shadow);
  textLayer.classList.toggle('keep', s.lines === 'keep');

  const n = view.blocks.length;
  const scale = (n <= 1 ? 1 : n === 2 ? 0.8 : 0.68) * (full ? 1.25 : 1);
  root.style.setProperty('--size', (s.size / 10.8) * scale + 'vh');
  root.style.setProperty('--font', `'${String(s.font).replace(/'/g, '')}'`);
  root.style.setProperty('--weight', String(s.weight));
  root.style.setProperty('--color', s.color);
  root.style.setProperty('--bar', { none: 'transparent', soft: 'rgba(9, 11, 15, 0.6)', solid: '#0D0F13' }[s.bar] || 'transparent');
  root.style.setProperty('--bar-pad', s.bar === 'none' ? '0' : '1.4vh 2.4vw');

  const box = document.createElement('div');
  box.className = 'box';
  if (view.refs && s.reference) {
    const ref = document.createElement('div');
    ref.className = 'ref';
    ref.textContent = view.refs.join('  ·  ');
    box.append(ref);
  }
  for (const block of view.blocks) {
    const p = document.createElement('p');
    p.className = block.song ? 'line song' : 'line';
    if (block.verses) p.innerHTML = versesHtml(block.verses);
    else p.innerHTML = block.lines.map(escapeHtml).join(block.song && s.lines === 'keep' ? '<br>' : ' ');
    box.append(p);
  }
  textLayer.replaceChildren(box);
  fit(box);
  if (!canBalance) box.querySelectorAll('.line').forEach(balance);
}

// Long passages shrink until they fit: the lower third may use about 40% of
// the frame, full screen nearly all of it.
function fit(box) {
  const limit = window.innerHeight * (style.layout === 'full' ? 0.88 : 0.4);
  for (let f = 1; box.offsetHeight > limit && f > 0.5; f -= 0.05) {
    box.style.fontSize = `calc(var(--size) * ${f.toFixed(2)})`;
  }
}

// For browsers without text-wrap: balance (older OBS): narrow the paragraph
// as far as it goes without adding a line, so lines come out even.
function balance(p) {
  if (p.classList.contains('song') && style.lines === 'keep') return;
  p.style.maxWidth = '';
  const height = p.offsetHeight;
  let lo = p.offsetWidth * 0.4;
  let hi = p.offsetWidth;
  while (hi - lo > 4) {
    const mid = (lo + hi) / 2;
    p.style.maxWidth = mid + 'px';
    if (p.offsetHeight > height) lo = mid; else hi = mid;
  }
  p.style.maxWidth = Math.ceil(hi) + 'px';
}

// ---------------------------------------------------------------------------
// Pictures and video

function settle(promise, ms) {
  return Promise.race([promise.catch(() => {}), new Promise((r) => setTimeout(r, ms))]);
}

async function loadMedia(view) {
  if (view.kind === 'picture') {
    const img = new Image();
    img.alt = '';
    img.src = view.url;
    await settle(img.decode(), 5000);
    mediaLayer.replaceChildren(img);
    return;
  }
  const v = document.createElement('video');
  v.muted = true;              // the sound reaches the stream through the mixer
  v.playsInline = true;
  v.preload = 'auto';
  v.dataset.seq = String(view.seq);
  v.src = view.url;
  v.addEventListener('seeked', learnSeekLead);
  mediaLayer.replaceChildren(v);
  await settle(new Promise((r) => v.addEventListener('loadeddata', r, { once: true })), 4000);
}

// Where the projector's video is now, in seconds.
function projectorTime() {
  const playing = video.state === 'playing';
  return (video.position + (playing ? performance.now() - video.receivedAt : 0)) / 1000;
}

// A seek takes a moment to land (the file comes over the network), so seeks
// aim that far ahead; the lead is learned from how far each seek fell short.
let seekLead = 0.15;

function learnSeekLead(e) {
  if (!video || video.state !== 'playing') return;
  const miss = projectorTime() - e.target.currentTime;
  seekLead = Math.min(1.5, Math.max(0, seekLead + miss));
}

// Big differences seek; small ones play slightly faster or slower until they
// close, which is invisible. A paused frame matches exactly.
function syncVideo() {
  const v = mediaLayer.querySelector('video');
  if (!v || !video || Number(v.dataset.seq) !== video.seq) return;
  const playing = video.state === 'playing';
  const at = projectorTime();
  const drift = at - v.currentTime;
  if (Number.isFinite(at) && !v.seeking && Math.abs(drift) > (playing ? 0.4 : 0.05)) {
    try { v.currentTime = playing ? at + seekLead : at; } catch { /* not seekable yet */ }
  }
  v.playbackRate = playing && Math.abs(drift) > 0.06 && Math.abs(drift) <= 0.4 ? (drift > 0 ? 1.06 : 0.94) : 1;
  if (playing && v.paused) v.play().catch(() => {});
  if (!playing && !v.paused) v.pause();
}

setInterval(syncVideo, 500);
