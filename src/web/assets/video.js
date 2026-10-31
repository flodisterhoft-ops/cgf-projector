// Plays the projector's live video in step with it: the stream overlay and
// the back-screen mirror both use this. Always muted; the sound reaches the
// room and the stream through the mixer.

export function createVideoPlayer(container) {
  let state = null;     // last playback state from the bridge, with arrival time
  // A seek takes a moment to land (the file comes over the network), so
  // seeks aim that far ahead; the lead is learned from how far each fell short.
  let seekLead = 0.15;

  const element = () => container.querySelector('video');

  function projectorTime() {
    const playing = state.state === 'playing';
    return (state.position + (playing ? performance.now() - state.receivedAt : 0)) / 1000;
  }

  function learnSeekLead(e) {
    if (!state || state.state !== 'playing') return;
    seekLead = Math.min(1.5, Math.max(0, seekLead + projectorTime() - e.target.currentTime));
  }

  // Big differences seek; small ones play slightly faster or slower until
  // they close, which is invisible. A paused frame matches exactly.
  function sync() {
    const v = element();
    if (!v || !state || Number(v.dataset.seq) !== state.seq) return;
    const playing = state.state === 'playing';
    const at = projectorTime();
    const drift = at - v.currentTime;
    if (Number.isFinite(at) && !v.seeking && Math.abs(drift) > (playing ? 0.4 : 0.05)) {
      try { v.currentTime = playing ? at + seekLead : at; } catch { /* not seekable yet */ }
    }
    v.playbackRate = playing && Math.abs(drift) > 0.06 && Math.abs(drift) <= 0.4 ? (drift > 0 ? 1.06 : 0.94) : 1;
    if (playing && v.paused) v.play().catch(() => {});
    if (!playing && !v.paused) v.pause();
  }

  setInterval(sync, 500);

  return {
    get state() { return state; },

    setState(message) {
      state = Object.assign({ receivedAt: performance.now() }, message);
      sync();
    },

    // Resolves once the first frame is there (or after 4 s regardless).
    async load(url, seq) {
      const v = document.createElement('video');
      v.muted = true;
      v.playsInline = true;
      v.preload = 'auto';
      v.dataset.seq = String(seq);
      v.src = url;
      v.addEventListener('seeked', learnSeekLead);
      container.replaceChildren(v);
      await Promise.race([
        new Promise((r) => v.addEventListener('loadeddata', r, { once: true })),
        new Promise((r) => setTimeout(r, 4000)),
      ]);
      sync();
    },

    clear() {
      const v = element();
      if (v) {
        v.pause();
        v.removeAttribute('src');
        v.load();       // stops the download
        v.remove();
      }
    },
  };
}
