import { chromium } from 'playwright';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
const out = path.resolve('test-results'); fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ channel: process.platform === 'win32' ? 'msedge' : undefined, headless: true });
const page = await browser.newPage({ viewport: { width: 716, height: 900 } });
const errors = []; page.on('pageerror', (error) => errors.push(error.message));
const evidence = [];
try {
  // The named grid and custom properties are the rules observed in Notion 7.37.0.
  await page.setContent(`<style>
    * { box-sizing: border-box; } body { margin: 0; font: 16px/1.5 sans-serif; }
    .notion-frame { width: 100%; }.notion-scroller { height: 820px; overflow: auto; }
    .layout { --content-width: minmax(auto,720px); --margin-width:minmax(96px,1fr); grid-template-columns:[full-start] var(--margin-left-width,var(--margin-width)) [content-start] var(--content-width) [content-end] var(--margin-right-width,var(--margin-width)) [full-end]; width:100%; display:grid; }
    .layout-wide {--content-width:1fr;--margin-width:96px;}.layout-content {grid-column:content;}.layout-full{grid-column:full;}
    .notion-page-content { width:100%; min-height:1200px; } [contenteditable] {outline:none;min-height:25px;} .layout-full {height:80px;background:#eee;}
  </style><main class="notion-frame"><div class="notion-scroller"><div class="whenContentEditable" contenteditable="true"><div class="layout layout-wide"><div class="layout-full"></div><div class="layout-content"><div class="notion-page-content"><div contenteditable="true" data-content-editable-leaf="true">Notion KEX typing geometry test — unchanged content.</div><input id="search" placeholder="Search"></div></div></div></div></div></main>`);
  await page.evaluate(() => {
    // Native Notion sanitizes custom attributes and classes inside its editable tree.
    new MutationObserver((entries) => {
      for (const entry of entries) {
        if (!entry.target.closest('.whenContentEditable')) continue;
        if (entry.attributeName.startsWith('data-notion-kex-')) entry.target.removeAttribute(entry.attributeName);
        if (entry.attributeName === 'class' && entry.target.classList.contains('notion-kex-native-caret-hidden')) entry.target.classList.remove('notion-kex-native-caret-hidden');
      }
    }).observe(document.querySelector('.whenContentEditable'), { subtree: true, attributes: true });
    const listeners = [];
    const storage = {};
    window.chrome = { runtime: {}, storage: { local: {
      get: (_key, callback) => callback(storage),
      set: (value, callback) => { Object.assign(storage, value); for (const listener of listeners) listener(Object.fromEntries(Object.entries(value).map(([key, newValue]) => [key, { newValue }])), 'local'); callback?.(); },
    }, onChanged: { addListener: (listener) => listeners.push(listener), removeListener: (listener) => listeners.splice(listeners.indexOf(listener), 1) } } };
  });
  await page.addStyleTag({ path: 'extension/kex.css' });
  await page.addScriptTag({ path: 'extension/policy.js' });
  await page.addScriptTag({ path: 'extension/kex.js' });
  const layout = page.locator('.layout');
  await page.waitForFunction(() => document.querySelector('.notion-frame')?.dataset.notionKexCompact === 'true');
  for (const width of [390, 716, 960, 1440]) {
    await page.setViewportSize({ width, height: 900 });
    await page.waitForFunction((compact) => (document.querySelector('.notion-frame')?.dataset.notionKexCompact === 'true') === compact, width < 1100);
    await page.waitForFunction((expected) => document.querySelector('.notion-page-content').getBoundingClientRect().left === expected, width < 520 ? 20 : width < 1100 ? 24 : 96);
    const geometry = await page.evaluate(() => { const e = document.querySelector('.notion-page-content').getBoundingClientRect(); return { width: innerWidth, content: e.width, left: e.left, overflow: document.documentElement.scrollWidth > innerWidth }; });
    assert.equal(geometry.overflow, false);
    assert.equal(geometry.left, width < 520 ? 20 : width < 1100 ? 24 : 96);
    assert.equal(geometry.content, width - geometry.left * 2);
    evidence.push({ check: 'responsive grid', ...geometry });
    await page.screenshot({ path: path.join(out, `grid-${width}.png`) });
  }
  await page.setViewportSize({ width: 716, height: 900 });
  const editor = page.locator('[data-content-editable-leaf]');
  const original = await editor.textContent();
  async function selection(start, end = start) {
    await page.locator('.whenContentEditable').focus();
    await page.keyboard.press('ArrowRight');
    await editor.evaluate((element, { start, end }) => {
      element.focus(); const range = document.createRange(); range.setStart(element.firstChild, start); range.setEnd(element.firstChild, end);
      const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    }, { start, end });
  }
  await selection(1);
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
  await selection(5);
  await page.waitForFunction(() => document.querySelector('[data-content-editable-leaf]').getAnimations().some(animation => animation.effect.getTiming().duration === 100));
  await page.screenshot({ path: path.join(out, 'caret.png'), caret: 'initial' });
  await selection(1, 5);
  await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
  evidence.push({ check: 'selection uses native caret', passed: true });
  await selection(3);
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
  await editor.dispatchEvent('compositionstart');
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'composing');
  assert.equal(await editor.evaluate((element) => getComputedStyle(element).caretColor !== 'rgba(0, 0, 0, 0)'), true);
  await editor.dispatchEvent('compositionend');
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
  evidence.push({ check: 'IME composition retains native caret', passed: true });
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'reduced');
  assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await page.locator('#search').focus();
  await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'native');
  await page.evaluate(() => window.chrome.storage.local.set({ 'notion-kex.settings': { enabled: false } }));
  await page.waitForFunction(() => !document.querySelector('.notion-frame')?.dataset.notionKexCompact);
  assert.equal(await editor.textContent(), original);
  evidence.push({ check: 'disable restores layout; text unchanged; reduced motion and search use native caret', passed: true });
  // Navigation remount: newly inserted page grids receive the same policy.
  await page.evaluate(() => {
    window.chrome.storage.local.set({ 'notion-kex.settings': { enabled: true } });
    const layout = document.querySelector('.layout'); const next = layout.cloneNode(true); next.removeAttribute('data-notion-kex-compact'); layout.replaceWith(next);
  });
  await page.waitForFunction(() => document.querySelector('.notion-frame')?.dataset.notionKexCompact === 'true');
  await layout.evaluate((element) => element.style.gridTemplateColumns = '1fr');
  await page.setViewportSize({ width: 700, height: 900 });
  await page.waitForFunction(() => !document.querySelector('.notion-frame')?.dataset.notionKexCompact);
  evidence.push({ check: 'SPA remount and unsupported grid fallback', passed: true });
  // A nested page pane owns its width; outer compact styling must stop at its frame.
  await page.setViewportSize({ width: 716, height: 900 });
  await layout.evaluate((element) => element.style.removeProperty('grid-template-columns'));
  await page.evaluate(() => {
    const nested = document.createElement('aside'); nested.className = 'notion-frame nested-pane'; nested.style.width = '1440px';
    nested.innerHTML = '<div class="layout"><div class="layout-content"><div class="notion-page-content">Nested native layout</div></div></div>';
    document.querySelector('main.notion-frame').append(nested);
  });
  await page.waitForFunction(() => document.querySelector('main.notion-frame').dataset.notionKexCompact === 'true');
  await page.waitForFunction(() => {
    const frame = document.querySelector('.nested-pane'), content = frame.querySelector('.notion-page-content');
    return content.getBoundingClientRect().left - frame.getBoundingClientRect().left === 360;
  });
  assert.equal(await page.locator('.nested-pane').getAttribute('data-notion-kex-compact'), null);
  evidence.push({ check: 'nested panes retain independent layout despite outer compact frame', passed: true });
  assert.deepEqual(errors, []);
  // The same settings UI is opened by Notion's native extension menu.
  const settingsPage = await browser.newPage({ viewport: { width: 520, height: 700 } });
  await settingsPage.addInitScript(() => {
    const values = {}; window.chrome = { runtime: {}, storage: { local: {
      get: (_key, callback) => callback(values),
      set: (next, callback) => { Object.assign(values, next); callback?.(); },
    } } };
  });
  await settingsPage.goto(new URL('../extension/settings.html', import.meta.url).href);
  assert.equal(await settingsPage.locator('#enabled').isChecked(), true);
  assert.equal(await settingsPage.locator('#touchpadZoom').isChecked(), true);
  await settingsPage.locator('#touchpadZoom').uncheck();
  assert.equal(await settingsPage.evaluate(() => new Promise(resolve => chrome.storage.local.get('notion-kex.settings', values => resolve(values['notion-kex.settings'].touchpadZoom)))), false);
  await settingsPage.locator('#smoothCaret').uncheck();
  assert.equal(await settingsPage.locator('#smoothCaret').isChecked(), false);
  await settingsPage.locator('#gutter').focus();
  await settingsPage.keyboard.press('ArrowRight');
  assert.equal(await settingsPage.locator('#gutter-value').textContent(), '26 px');
  await settingsPage.locator('#reset').click();
  assert.equal(await settingsPage.locator('#smoothCaret').isChecked(), true);
  assert.equal(await settingsPage.locator('#touchpadZoom').isChecked(), true);
  assert.equal(await settingsPage.locator('#gutter-value').textContent(), '24 px');
  await settingsPage.screenshot({ path: path.join(out, 'settings.png') });
  evidence.push({ check: 'settings save, keyboard range control and reset defaults', passed: true });
  fs.writeFileSync(path.join(out, 'browser.json'), JSON.stringify({ browser: browser.version(), checks: evidence }, null, 2));
  console.log(JSON.stringify({ passed: evidence.length, browser: browser.version(), output: out }));
} finally { await browser.close(); }
