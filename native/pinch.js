;/* Notion KEX native pinch v1 */
(() => {
  'use strict';
  if (!process.isMainFrame || location.protocol !== 'https:' || !['app.notion.com', 'www.notion.so', 'notion.so'].includes(location.hostname)) return;
  const { ipcRenderer } = require('electron');
  let enabled = true;
  let applied;
  // Navigation resets early page-scale limits; wait for the new document.
  let change = document.readyState === 'loading'
    ? new Promise(resolve => window.addEventListener('DOMContentLoaded', resolve, { once: true }))
    : Promise.resolve();
  const report = () => window.postMessage({ type: 'notion-kex:pinch-state', version: 1, available: typeof applied === 'boolean', ...(typeof applied === 'boolean' ? { enabled: applied } : {}) }, location.origin);
  const apply = (reset = false) => {
    const requested = enabled;
    change = change.then(async () => {
      const actual = await ipcRenderer.invoke('notion-kex:configure-pinch', 1, requested, reset);
      if (actual !== requested) throw Error('Native pinch preference was not applied.');
      applied = requested;
      report();
    }).catch(() => { applied = undefined; report(); console.warn('Notion KEX: native pinch zoom unavailable.'); });
  };
  window.addEventListener('message', event => {
    const data = event.data;
    if (event.source !== window || event.origin !== location.origin || data?.type !== 'notion-kex:pinch' || data.version !== 1 || typeof data.enabled !== 'boolean') return;
    if (enabled !== data.enabled || applied !== data.enabled) { enabled = data.enabled; apply(); }
    else void change.then(report);
  });
  ipcRenderer.on('notion-kex:reset-pinch', (_event, version) => { if (enabled && version === 1) apply(true); });
  apply();
})();
