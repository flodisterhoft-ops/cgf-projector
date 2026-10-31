// Back-screen mirror: shows exactly what the front projector shows, from the
// projector's own rendered layers (background and text) with its fades, and
// the live video in step. Click once to go full screen.

import { connectBridge } from './bridge.js';
import { createVideoPlayer } from './video.js';

const stage = document.getElementById('stage');
const backs = [...document.querySelectorAll('img.back')];
const texts = [...document.querySelectorAll('img.text')];
const offline = document.getElementById('offline');
const player = createVideoPlayer(document.getElementById('video'));

let size = { w: 1920, h: 1080 };
let live = null;
let videoSeq = null;   // the video loaded on the stage
let offlineTimer = null;

const bridge = connectBridge({
  onMessage(m) {
    if (m.type === 'hello') bridge.send({ type: 'subscribe', frames: true });
    else if (m.type === 'frame') showFrame(m);
    else if (m.type === 'live') {
      live = m;
      updateVideo();
    } else if (m.type === 'video') {
      player.setState(m);
      updateVideo();
    }
  },
  onStatus(up) {
    clearTimeout(offlineTimer);
    if (up) offline.hidden = true;
    // The last picture stays up; a small dot shows the laptop is unreachable.
    else offlineTimer = setTimeout(() => { offline.hidden = false; }, 10000);
  },
});

function fitStage() {
  const scale = Math.min(innerWidth / size.w, innerHeight / size.h);
  stage.style.width = size.w * scale + 'px';
  stage.style.height = size.h * scale + 'px';
}

function showFrame(m) {
  if (m.width > 0 && m.height > 0 && (m.width !== size.w || m.height !== size.h)) {
    size = { w: m.width, h: m.height };
    fitStage();
  }
  const ms = m.fade ? m.ms : 0;
  crossfade(backs, m.back, ms);
  crossfade(texts, m.text, ms);
}

// Two images per layer: the new one loads unseen, then fades in as the old
// one fades out, as the projector does.
function crossfade(pair, url, ms) {
  const [front, spare] = pair[1].classList.contains('on') ? [pair[1], pair[0]] : pair;
  if (!url || front.dataset.url === url) return;
  spare.dataset.url = url;
  spare.src = url;
  spare.decode().catch(() => {}).then(() => {
    if (spare.dataset.url !== url) return;   // a newer frame took over
    for (const img of pair) img.style.transitionDuration = ms + 'ms';
    spare.classList.add('on');
    front.classList.remove('on');
  });
}

function updateVideo() {
  const item = live && live.visible && live.kind === 'video' && !live.video.audioOnly ? live.video : null;
  const playback = player.state;
  const wanted = item && playback && playback.seq === item.seq && playback.state !== 'stopped' ? item : null;
  if (!wanted) {
    if (videoSeq !== null) player.clear();
    videoSeq = null;
  } else if (videoSeq !== wanted.seq) {
    videoSeq = wanted.seq;
    player.load(wanted.url, wanted.seq);
  }
}

addEventListener('resize', fitStage);
document.addEventListener('click', () => {
  if (!document.fullscreenElement) document.documentElement.requestFullscreen().catch(() => {});
});
fitStage();
