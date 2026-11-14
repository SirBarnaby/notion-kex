// Notion KEX demo mode, appended to kex.js only by tools/demo/Record-Demo.ps1 for recording the README clips.
// It runs once, on a page titled "Notion Demo", before the expiry time the recorder bakes in, then restores
// the saved settings and removes the sample text it typed.
// The release build never contains this file.
(() => {
  'use strict';
  const expires = Number('__KEX_DEMO_EXPIRES__');
  if (!(Date.now() < expires) || globalThis.__notionKexDemo) return;
  globalThis.__notionKexDemo = true;
  const api = globalThis.chrome, key = globalThis.NotionKexPolicy.storageKey;
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const read = () => new Promise((resolve) => api.storage.local.get(key, (result) => resolve(result?.[key])));
  const write = (value) => new Promise((resolve) => api.storage.local.set({ [key]: value }, resolve));
  const editables = () => [...document.querySelectorAll('.notion-page-content [contenteditable="true"]')];
  const placeAtEnd = (element) => {
    element.focus();
    const range = document.createRange();
    range.selectNodeContents(element);
    range.collapse(false);
    const selection = getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
  };

  async function run() {
    for (let i = 0; i < 120 && !/notion demo/i.test(document.title); i++) await sleep(500);
    if (!/notion demo/i.test(document.title)) return;
    await sleep(3000);
    const saved = await read();
    const base = { ...globalThis.NotionKexPolicy.normalize(saved) };
    const target = editables()[0];
    if (!target) return;
    const text = 'Ship the new onboarding flow by the end of the month. Keep the first run under two minutes, and fix the slowest step first.';
    let typed = 0;
    try {
      window.focus();
      await sleep(600);
      placeAtEnd(target);
      await sleep(1500);
      document.execCommand('insertLineBreak');
      typed++;
      await sleep(500);
      for (const character of text) {
        document.execCommand('insertText', false, character);
        typed++;
        await sleep(55 + (character === ' ' ? 35 : 0));
      }
      await sleep(1800);
      await write({ ...base, enabled: false }); await sleep(2000);
      await write({ ...base, enabled: true }); await sleep(2600);
      await write({ ...base, enabled: false }); await sleep(1600);
      await write({ ...base, enabled: true }); await sleep(2600);
    } finally {
      placeAtEnd(target);
      for (; typed > 0; typed--) { document.execCommand('delete'); await sleep(8); }
      await write(saved === undefined ? base : saved);
    }
  }
  run().catch(() => {});
})();
