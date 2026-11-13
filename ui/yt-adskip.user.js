// ==UserScript==
// @name         YouTube Ad Skipper
// @namespace    neonnet.builtin
// @version      1.0.0
// @description  Auto-skips and fast-forwards YouTube video ads and hides static ad slots.
// @match        https://www.youtube.com/*
// @match        https://m.youtube.com/*
// @run-at       document-end
// @grant        none
// ==/UserScript==

(() => {
  'use strict';

  const STYLE_ID = 'nn-yt-adskip-style';
  if (!document.getElementById(STYLE_ID)) {
    const st = document.createElement('style');
    st.id = STYLE_ID;
    st.textContent = [
      'ytd-ad-slot-renderer',
      'ytd-banner-promo-renderer',
      'ytd-display-ad-renderer',
      'ytd-promoted-sparkles-web-renderer',
      'ytd-companion-slot-renderer',
      'ytd-in-feed-ad-layout-renderer',
      '#player-ads',
      '.ytp-ad-overlay-container',
      '.ytp-ad-image-overlay',
      '.ytd-merch-shelf-renderer'
    ].join(',') + '{display:none!important}';
    (document.head || document.documentElement).appendChild(st);
  }

  let savedMuted = null;
  let scheduled = 0;

  function tick() {
    scheduled = 0;
    const player = document.getElementById('movie_player');
    const video = player ? player.querySelector('video.html5-main-video') : document.querySelector('video.html5-main-video');
    const adActive = !!(player && (player.classList.contains('ad-showing') || player.classList.contains('ad-interrupting')));

    for (const sel of ['.ytp-skip-ad-button', '.ytp-ad-skip-button-modern', '.ytp-ad-skip-button', '.ytp-ad-overlay-close-button']) {
      const btn = document.querySelector(sel);
      if (btn && btn.offsetParent !== null) btn.click();
    }

    if (!video) return;
    if (adActive) {
      if (savedMuted === null) savedMuted = video.muted;
      video.muted = true;
      if (Number.isFinite(video.duration) && video.duration > 0.25 && video.currentTime < video.duration - 0.15) {
        video.currentTime = Math.max(0, video.duration - 0.1);
      } else if (video.playbackRate < 16) {
        video.playbackRate = 16;
      }
    } else if (savedMuted !== null) {
      video.muted = savedMuted;
      savedMuted = null;
      if (video.playbackRate !== 1) video.playbackRate = 1;
    }
  }

  function schedule() {
    if (!scheduled) scheduled = setTimeout(tick, 120);
  }

  const obs = new MutationObserver(schedule);
  obs.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
  document.addEventListener('yt-navigate-finish', schedule, true);
  tick();
})();
