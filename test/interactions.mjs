import { chromium } from 'playwright';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const out = path.resolve('test-results');
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ channel: process.platform === 'win32' ? 'msedge' : undefined, headless: true });
const results = [];
const fixture = async ({ sidebar = true, important = false } = {}) => {
  const page = await browser.newPage({ viewport: { width: 650, height: 900 } });
  page.setDefaultTimeout(5000);
  await page.setContent(`<style>
    *{box-sizing:border-box}body{margin:0;font:16px/1.5 sans-serif}
    .notion-frame{width:100%;padding-top:80px}.notion-scroller{height:800px;overflow:auto}
    .layout{--content-width:minmax(auto,720px);--margin-width:minmax(96px,1fr);display:grid;width:100%;grid-template-columns:[full-start] var(--margin-left-width,var(--margin-width)) [content-start] var(--content-width) [content-end] var(--margin-right-width,var(--margin-width)) [full-end]}
    .layout-content{grid-column:content;min-width:0}.notion-page-content{width:100%}
    .notion-text-block{position:relative;min-height:80px;max-width:458px}
    [contenteditable]{outline:none;caret-color:#222}.formatted{caret-color:#c00}
    .handle{position:absolute;left:-20px;top:0;width:18px;height:28px;cursor:grab}
    .notion-sidebar-container{position:fixed;left:0;top:48px;height:400px;z-index:10;pointer-events:none}
    .notion-sidebar{width:240px;height:400px;background:#ddd;visibility:hidden;pointer-events:none}
    #edge{position:absolute;inset:0 auto 0 0;width:20px;pointer-events:auto;visibility:visible}
  </style>${sidebar ? '<button class="notion-open-sidebar" id="open">Open sidebar</button><nav class="notion-sidebar-container" style="width:0px" aria-hidden="true"><div id="edge"></div><div class="notion-sidebar" id="peek"><button id="sidebar-item">Native sidebar item</button></div></nav>' : ''}
  <main class="notion-frame"><div class="notion-scroller"><div class="whenContentEditable" contenteditable="true"><div class="layout"><div class="layout-content"><div class="notion-page-content">
    <div class="notion-text-block" id="a"><button contenteditable="false" class="handle" draggable="true">⋮⋮</button><div contenteditable="true" data-content-editable-leaf="true"><span class="formatted" ${important ? 'style="caret-color:#c00!important"' : ''}>Formatted text remains native content.</span></div></div>
    <div class="notion-text-block" id="b">Drop target</div><input id="search" placeholder="Search">
  </div></div></div></div></div></main>`);
  await page.evaluate(() => {
    const frames = new Set(), requestFrame = window.requestAnimationFrame.bind(window);
    const observers = [], Observer = window.MutationObserver;
    window.MutationObserver = class extends Observer { constructor(callback) { super(callback); observers.push(this); } };
    window.frameRequests = 0;
    window.requestAnimationFrame = callback => {
      window.frameRequests++;
      const id = requestFrame(time => { frames.delete(id); callback(time); }); frames.add(id); return id;
    };
    window.freezeCaretTracking = () => { for (const id of frames) cancelAnimationFrame(id); frames.clear(); for (const observer of observers) observer.disconnect(); window.requestAnimationFrame = () => 0; };
    const listeners = [];
    window.chrome = { runtime: {}, storage: { local: { get: (_key, callback) => callback({}), set: (values, callback) => { for (const listener of listeners) listener(Object.fromEntries(Object.entries(values).map(([key, newValue]) => [key, { newValue }])), 'local'); callback?.(); } }, onChanged: { addListener: (callback) => listeners.push(callback), removeListener: () => {} } } };
    window.peekSidebar = () => {
      const nav = document.querySelector('.notion-sidebar-container');
      nav.removeAttribute('aria-hidden');
      Object.assign(document.getElementById('peek').style, { visibility: 'visible', pointerEvents: 'auto' });
    };
    const toggle = () => {
      const nav = document.querySelector('.notion-sidebar-container');
      nav.style.width = nav.style.width === '0px' ? '240px' : '0px';
      window.peekSidebar();
    };
    document.getElementById('edge')?.addEventListener('mousemove', window.peekSidebar);
    document.getElementById('open')?.addEventListener('click', toggle);
    document.addEventListener('keydown', event => { if (event.ctrlKey && event.key === '\\') toggle(); });
    document.getElementById('sidebar-item')?.addEventListener('click', () => { window.sidebarClicks = (window.sidebarClicks || 0) + 1; });
    const handle = document.querySelector('.handle');
    handle.addEventListener('dragstart', (event) => { event.dataTransfer.setData('text/plain', 'a'); });
    const target = document.getElementById('b');
    target.addEventListener('dragover', (event) => event.preventDefault());
    target.addEventListener('drop', (event) => { event.preventDefault(); target.after(document.getElementById(event.dataTransfer.getData('text/plain'))); });
  });
  const sheet = await page.addStyleTag({ path: 'extension/kex.css' });
  await sheet.evaluate((element) => { element.id = 'kex-test-style'; });
  await page.addScriptTag({ path: 'extension/policy.js' });
  await page.addScriptTag({ path: 'extension/kex.js' });
  await page.waitForFunction(() => document.querySelector('.notion-frame')?.dataset.notionKexCompact === 'true');
  return page;
};
const select = async (page, offset = 2) => {
  await page.locator('.whenContentEditable').focus();
  await page.keyboard.press('ArrowRight');
  await page.evaluate((offset) => {
    const leaf = document.querySelector('.formatted');
    document.querySelector('.whenContentEditable').focus();
    const range = document.createRange(); range.setStart(leaf.firstChild, offset); range.collapse(true);
    getSelection().removeAllRanges(); getSelection().addRange(range);
  }, offset);
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
};
const test = async (name, run) => {
  try { await run(); results.push({ name, passed: true }); }
  catch (error) { results.push({ name, passed: false, error: error.message }); process.exitCode = 1; }
};
const caretPixels = async page => {
  const shot = await page.screenshot({ caret: 'initial' });
  return page.evaluate(async data => {
    const img = new Image(); img.src = 'data:image/png;base64,' + data; await img.decode();
    const canvas = new OffscreenCanvas(img.width, img.height), context = canvas.getContext('2d'); context.drawImage(img, 0, 0);
    const rgba = context.getImageData(0, 0, img.width, img.height).data, points = [];
    for (let y = 0; y < img.height; y++) for (let x = 0; x < img.width; x++) {
      const i = (y * img.width + x) * 4;
      if (rgba[i] > 160 && rgba[i + 1] < 30 && rgba[i + 2] < 30) points.push({ x, y });
    }
    return points;
  }, shot.toString('base64'));
};
const holdBlink = async (page, finishMotion = true) => page.locator('[data-content-editable-leaf]').evaluate((host, finishMotion) => {
  for (const animation of host.getAnimations()) {
    if (animation.id === 'notion-kex-blink') {
      // Hold the current blink phase, on its visible side.
      animation.pause();
      if (getComputedStyle(host).backgroundSize.startsWith('0px')) animation.currentTime = animation.currentTime < animation.effect.getTiming().delay ? animation.effect.getTiming().delay : 0;
    }
    else if (finishMotion && animation.effect.getKeyframes().some(frame => 'backgroundPosition' in frame)) animation.finish();
  }
}, finishMotion);

try {
  await test('compact margins stay equal, edge hover stays hidden, and block dragging works', async () => {
    const page = await fixture();
    try {
      for (const width of [390, 650, 960]) {
        await page.setViewportSize({ width, height: 900 });
        await page.waitForFunction((expected) => {
          const content = document.querySelector('.notion-page-content').getBoundingClientRect();
          return Math.abs(innerWidth - content.right - expected) < 0.1;
        }, width < 520 ? 20 : 24);
        const handle = await page.locator('.handle').boundingBox();
        assert.ok(handle.x >= 0 && handle.x < 20, `drag handle should stay compressed at ${width}px`);
        const content = await page.locator('.notion-page-content').boundingBox();
        assert.equal(content.x, width < 520 ? 20 : 24);
        assert.equal(width - content.x - content.width, width < 520 ? 20 : 24);
        await page.mouse.move(8, 300);
        assert.equal(await page.locator('#peek').isVisible(), false);
        await page.locator('.handle').hover();
        assert.equal(await page.locator('#peek').isVisible(), false);
      }
      await page.locator('.handle').dragTo(page.locator('#b'));
      assert.deepEqual(await page.locator('.notion-page-content > .notion-text-block').evaluateAll((blocks) => blocks.map((block) => block.id)), ['b', 'a']);
      assert.equal(await page.locator('#peek').isVisible(), false);
      // Electron can request a peek without a DOM mouse event. It must stay hidden too.
      await page.evaluate(() => window.peekSidebar());
      assert.equal(await page.locator('#peek').isVisible(), false);
      await page.screenshot({ path: path.join(out, 'compact-no-sidebar-hover.png') });
    } finally { await page.close(); }
  });
  await test('formatted text and the focused editable root use a single element-owned caret', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      const colors = await page.locator('.whenContentEditable, [data-content-editable-leaf], .formatted').evaluateAll((elements) => elements.map((element) => getComputedStyle(element).caretColor));
      assert.ok(colors.every((color) => color === 'rgba(0, 0, 0, 0)'), `native caret still visible: ${colors}`);
      assert.equal(await page.locator('#notion-kex-caret').count(), 0);
      assert.match(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), /^linear-gradient/);
      await page.locator('#search').focus();
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
      assert.notEqual(await page.locator('#search').evaluate((element) => getComputedStyle(element).caretColor), 'rgba(0, 0, 0, 0)');
    } finally { await page.close(); }
  });
  await test('native sidebar button, item and keyboard shortcut remain usable with compact margins', async () => {
    const page = await fixture();
    try {
      await page.locator('.notion-frame').evaluate((frame) => { frame.style.marginLeft = '240px'; frame.style.width = 'calc(100% - 240px)'; });
      await page.locator('#open').click();
      await page.locator('#sidebar-item').click();
      assert.equal(await page.evaluate(() => window.sidebarClicks), 1);
      await page.waitForFunction(() => {
        const layout = document.querySelector('.layout').getBoundingClientRect();
        const content = document.querySelector('.notion-page-content').getBoundingClientRect();
        return Math.abs(layout.right - content.right - 20) < 0.1;
      });
      const content = await page.locator('.notion-page-content').boundingBox();
      assert.equal(content.x, 260);
      await page.keyboard.press('Control+Backslash');
      await page.waitForFunction(() => getComputedStyle(document.querySelector('.notion-sidebar')).visibility === 'hidden');
      await page.keyboard.press('Control+Backslash');
      await page.waitForFunction(() => getComputedStyle(document.querySelector('.notion-sidebar')).visibility === 'visible');
      await page.locator('#sidebar-item').click();
      assert.equal(await page.evaluate(() => window.sidebarClicks), 2);
    } finally { await page.close(); }
  });
  await test('disabling KEX restores native hover and a remounted sidebar is handled', async () => {
    const page = await fixture();
    try {
      await page.evaluate(() => chrome.storage.local.set({ 'notion-kex.settings': { enabled: false } }));
      await page.waitForFunction(() => !document.querySelector('.notion-frame').dataset.notionKexCompact);
      await page.mouse.move(8, 300);
      assert.equal(await page.locator('#peek').isVisible(), true);
      await page.evaluate(() => {
        const nav = document.querySelector('.notion-sidebar-container');
        nav.replaceWith(nav.cloneNode(true));
        chrome.storage.local.set({ 'notion-kex.settings': { responsive: false, smoothCaret: false } });
      });
      await page.waitForFunction(() => getComputedStyle(document.querySelector('.notion-sidebar')).visibility === 'hidden');
      assert.equal(await page.locator('.notion-frame[data-notion-kex-compact]').count(), 0);
      await page.evaluate(() => {
        // An unknown future width contract retains native rendering.
        document.querySelector('.notion-sidebar-container').style.removeProperty('width');
      });
      await page.waitForFunction(() => getComputedStyle(document.querySelector('.notion-sidebar')).visibility === 'visible');
    } finally { await page.close(); }
  });
  await test('pointer selection temporarily uses only the native caret and then resumes smoothing', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      const text = await page.locator('.formatted').boundingBox();
      await page.mouse.move(text.x + 12, text.y + 10); await page.mouse.down();
      assert.equal(await page.locator('#notion-kex-caret[data-visible="true"]').count(), 0);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await page.mouse.up();
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
    } finally { await page.close(); }
  });
  await test('a native caret that cannot be hidden never gets a second overlay caret', async () => {
    const page = await fixture({ important: true, sidebar: false });
    try {
      await select(page); await page.waitForTimeout(150);
      assert.notEqual(await page.locator('.formatted').evaluate((element) => getComputedStyle(element).caretColor), 'rgba(0, 0, 0, 0)');
      assert.equal(await page.locator('#notion-kex-caret[data-visible="true"]').count(), 0);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
    } finally { await page.close(); }
  });
  await test('a block drag revokes smooth painting until fresh editing intent', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.evaluate(() => {
        const handle = document.querySelector('.handle');
        handle.dispatchEvent(new DragEvent('dragstart', { bubbles: true, dataTransfer: new DataTransfer() }));
        handle.dispatchEvent(new PointerEvent('pointercancel', { bubbles: true }));
      });
      await page.waitForTimeout(100);
      assert.equal(await page.locator('#notion-kex-caret[data-visible="true"]').count(), 0);
      await page.locator('.handle').dispatchEvent('dragend');
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await page.keyboard.press('ArrowRight');
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
    } finally { await page.close(); }
  });
  await test('caret pixels follow their text owner even when all tracking frames are stopped', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      const before = await caretPixels(page);
      assert.ok(before.length > 10, 'the caret must actually be painted');
      await page.evaluate(() => window.freezeCaretTracking());
      await page.locator('#a').evaluate((block) => { block.style.transform = 'translate(60px, 160px)'; });
      const after = await caretPixels(page);
      assert.equal(after.length, before.length);
      assert.equal(after[0].x - before[0].x, 60);
      assert.equal(after[0].y - before[0].y, 160);
      assert.equal(await page.locator('#notion-kex-caret').count(), 0);
    } finally { await page.close(); }
  });
  await test('a hidden selected editor cannot leave a floating blinking overlay', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('#a').evaluate((block) => { block.style.visibility = 'hidden'; });
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'), undefined, { timeout: 1500 });
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await page.locator('#a').evaluate((block) => { block.style.visibility = ''; });
      await select(page, 3);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
    } finally { await page.close(); }
  });
  await test('loss of native caret suppression is detected even while the selection is idle', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('.formatted').evaluate((text) => { text.style.setProperty('caret-color', '#c00', 'important'); });
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'), undefined, { timeout: 1500 });
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      assert.notEqual(await page.locator('.formatted').evaluate((text) => getComputedStyle(text).caretColor), 'rgba(0, 0, 0, 0)');
    } finally { await page.close(); }
  });
  await test('a stale selection in a noneditable block uses only the native caret', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('[data-content-editable-leaf]').evaluate((leaf) => { leaf.contentEditable = 'false'; });
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'), undefined, { timeout: 1500 });
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
    } finally { await page.close(); }
  });
  await test('the owned caret visibly glides between characters instead of jumping', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page, 2);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      const before = await caretPixels(page);
      assert.ok(before.length > 10);
      await select(page, 12);
      const target = await page.evaluate(() => {
        const host = document.querySelector('[data-content-editable-leaf]');
        const motion = host.getAnimations().find(animation => animation.effect.getTiming().duration === 100 && animation.effect.getKeyframes().some(frame => 'backgroundPosition' in frame));
        if (!motion) return undefined;
        motion.pause(); motion.currentTime = 50;
        return getSelection().getRangeAt(0).getBoundingClientRect().left;
      });
      // Wait for the selection event's draw, then pause its actual interpolation.
      if (target === undefined) await page.waitForFunction(() => document.querySelector('[data-content-editable-leaf]').getAnimations().some(animation => animation.effect.getTiming().duration === 100));
      const endpoint = await page.evaluate(() => {
        const motion = document.querySelector('[data-content-editable-leaf]').getAnimations().find(animation => animation.effect.getTiming().duration === 100 && animation.effect.getKeyframes().some(frame => 'backgroundPosition' in frame));
        motion.pause(); motion.currentTime = 50;
        return getSelection().getRangeAt(0).getBoundingClientRect().left;
      });
      await holdBlink(page, false);
      const middle = await caretPixels(page);
      assert.ok(middle.length > 10);
      assert.ok(middle[0].x > before[0].x && middle[0].x < endpoint - 1, `expected a glide between ${before[0].x} and ${endpoint}, saw ${middle[0].x}`);
    } finally { await page.close(); }
  });
  await test('typing then repeatedly dragging the block leaves no caret paint until editing resumes', async () => {
    const page = await fixture();
    try {
      for (let iteration = 0; iteration < 3; iteration++) {
        await page.locator('.formatted').click({ position: { x: 12, y: 8 } });
        await page.keyboard.press('End');
        await page.keyboard.type(` typed-${iteration}`);
        await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
        assert.ok((await page.locator('.formatted').textContent()).endsWith(`typed-${iteration}`));
        await page.locator('.handle').dragTo(page.locator('#b'));
        await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
        await page.waitForTimeout(100);
        assert.equal(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), 'none');
        assert.equal(await page.locator('#notion-kex-caret').count(), 0);
        assert.notEqual(await page.locator('.formatted').evaluate(text => getComputedStyle(text).caretColor), 'rgba(0, 0, 0, 0)');
      }
    } finally { await page.close(); }
  });
  await test('a cloned or removed block cannot retain an owned caret or edited attributes', async () => {
    const page = await fixture({ sidebar: false });
    try {
      const markup = await page.locator('#a').evaluate(block => block.outerHTML);
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      assert.equal(await page.locator('#a').evaluate(block => block.outerHTML), markup);
      assert.ok((await caretPixels(page)).length > 10);
      await page.evaluate(() => {
        window.freezeCaretTracking();
        const block = document.getElementById('a'), clone = block.cloneNode(true); clone.id = 'clone'; block.after(clone);
      });
      assert.equal(await page.locator('#clone [data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), 'none');
      assert.equal(await page.locator('#clone [data-content-editable-leaf]').evaluate(host => host.getAnimations().length), 0);
      await page.locator('#a').evaluate(block => { block.remove(); getSelection().removeAllRanges(); });
      assert.deepEqual(await caretPixels(page), []);
      assert.equal(await page.locator('#notion-kex-caret').count(), 0);
    } finally { await page.close(); }
  });
  await test('paint cannot escape a resized text owner even with tracking stopped', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page, 20);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      assert.ok((await caretPixels(page)).length > 10);
      await page.evaluate(() => { window.freezeCaretTracking(); document.querySelector('[data-content-editable-leaf]').style.width = '10px'; });
      assert.deepEqual(await caretPixels(page), []);
    } finally { await page.close(); }
  });
  await test('existing text backgrounds retain a visible native caret and their original styling', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('.formatted').evaluate(text => { text.style.backgroundColor = 'yellow'; });
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
      assert.notEqual(await page.locator('.formatted').evaluate(text => getComputedStyle(text).caretColor), 'rgba(0, 0, 0, 0)');
      assert.equal(await page.locator('.formatted').evaluate(text => getComputedStyle(text).backgroundColor), 'rgb(255, 255, 0)');
      await page.locator('.formatted').evaluate(text => { text.style.backgroundColor = ''; });
      await page.locator('[data-content-editable-leaf]').evaluate(host => { host.style.backgroundImage = 'linear-gradient(white, silver)'; });
      await select(page, 3); await page.waitForTimeout(100);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      assert.equal(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), 'linear-gradient(rgb(255, 255, 255), rgb(192, 192, 192))');
    } finally { await page.close(); }
  });
  await test('owned caret coordinates survive zoom, wrapped lines and RTL text', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.locator('[data-content-editable-leaf]').evaluate(host => { host.style.width = '220px'; });
      await page.locator('.formatted').evaluate(text => { text.textContent = 'Wrapped content keeps its insertion point tied to the same editable text element across several lines.'; });
      for (const zoom of [1, 1.25]) {
        await page.locator('.notion-frame').evaluate((frame, zoom) => { frame.style.zoom = zoom; }, zoom);
        await select(page, 75);
        await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
        await holdBlink(page);
        const points = await caretPixels(page), range = await page.evaluate(() => getSelection().getRangeAt(0).getBoundingClientRect().toJSON());
        assert.ok(points.length > 10);
        assert.ok(Math.abs(points[0].x - range.left) <= 2 && Math.abs(points[0].y - range.top) <= 2, `caret should follow wrapped text at zoom ${zoom}`);
      }
      await page.locator('.notion-frame').evaluate(frame => { frame.style.zoom = ''; });
      await page.locator('[data-content-editable-leaf]').evaluate(host => { host.style.direction = 'rtl'; });
      await page.locator('.formatted').evaluate(text => { text.textContent = 'טקסט לבדיקה של מיקום הסמן'; });
      await select(page, 4);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      const points = await caretPixels(page), range = await page.evaluate(() => getSelection().getRangeAt(0).getBoundingClientRect().toJSON());
      assert.ok(points.length > 10);
      assert.ok(Math.abs(points[0].x - range.left) <= 2);
    } finally { await page.close(); }
  });
  await test('scrolling an editable owner carries its caret paint without tracking', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.locator('[data-content-editable-leaf]').evaluate(host => { Object.assign(host.style, { width: '200px', whiteSpace: 'nowrap', overflow: 'auto' }); });
      await select(page, 12);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      const before = await caretPixels(page); assert.ok(before.length > 10);
      await page.evaluate(() => { window.freezeCaretTracking(); document.querySelector('[data-content-editable-leaf]').scrollLeft = 40; });
      const after = await caretPixels(page); assert.ok(after.length > 10);
      assert.equal(after[0].x - before[0].x, -40);
      assert.equal(after[0].y, before[0].y);
    } finally { await page.close(); }
  });
  await test('rotated, skewed and reflected editors keep the native caret', async () => {
    const page = await fixture({ sidebar: false });
    try {
      for (const transform of ['rotate(20deg)', 'skewX(10deg)', 'scaleX(-1)']) {
        await page.locator('.layout').evaluate((layout, transform) => { layout.style.transform = transform; }, transform);
        await select(page); await page.waitForTimeout(80);
        assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
        assert.notEqual(await page.locator('.formatted').evaluate(text => getComputedStyle(text).caretColor), 'rgba(0, 0, 0, 0)');
      }
      await page.locator('.layout').evaluate(layout => { layout.style.transform = ''; });
      await page.locator('.formatted').evaluate(text => { Object.assign(text.style, { display: 'inline-block', transform: 'rotate(20deg)' }); });
      await select(page); await page.waitForTimeout(80);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await page.locator('.formatted').evaluate(text => { text.style.display = ''; text.style.transform = ''; });
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
    } finally { await page.close(); }
  });
  await test('application classes and background animations retain their own rendering', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.addStyleTag({ content: '.app-background { background-image: linear-gradient(blue, green) !important; } .app-background-normal { background-image: linear-gradient(white, silver); }' });
      for (const className of ['app-background', 'app-background-normal']) {
        await select(page);
        await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
        await page.locator('[data-content-editable-leaf]').evaluate((host, className) => host.classList.add(className), className);
        await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
        assert.equal(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundSize), 'auto');
        assert.notEqual(await page.locator('.formatted').evaluate(text => getComputedStyle(text).caretColor), 'rgba(0, 0, 0, 0)');
        await page.locator('[data-content-editable-leaf]').evaluate((host, className) => host.classList.remove(className), className);
      }
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('[data-content-editable-leaf]').evaluate(host => { window.appAnimation = host.animate([{ backgroundImage: 'linear-gradient(blue, green)' }, { backgroundImage: 'linear-gradient(blue, green)' }], { duration: 1000, fill: 'both' }); });
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
      assert.equal(await page.evaluate(() => window.appAnimation.playState), 'running');
      assert.match(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), /rgb\(0, 0, 255\)/);
    } finally { await page.close(); }
  });
  await test('theme changes refresh the caret and synthetic pointer events cannot rearm it', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.addStyleTag({ content: '.new-theme .formatted { color: white; caret-color: rgb(0, 102, 255); }' });
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.locator('.notion-frame').evaluate(frame => frame.classList.add('new-theme'));
      await page.waitForFunction(() => !document.querySelector('[data-notion-kex-caret-active]'));
      await page.locator('.formatted').dispatchEvent('pointerdown');
      await page.locator('.formatted').dispatchEvent('pointerup');
      await page.waitForTimeout(80);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await select(page, 4);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      assert.match(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), /rgb\(0, 102, 255\)/);
    } finally { await page.close(); }
  });
  await test('a restored selection cannot start custom painting without trusted editing intent', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.evaluate(() => {
        document.querySelector('.whenContentEditable').focus();
        const range = document.createRange(); range.setStart(document.querySelector('.formatted').firstChild, 3); range.collapse(true);
        getSelection().removeAllRanges(); getSelection().addRange(range);
      });
      await page.waitForTimeout(80);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      await page.keyboard.press('ArrowRight');
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
    } finally { await page.close(); }
  });
  await test('an intentionally hidden native caret is never replaced by visible custom paint', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await page.locator('.formatted').evaluate(text => { text.style.caretColor = 'transparent'; });
      await select(page); await page.waitForTimeout(80);
      assert.equal(await page.locator('[data-notion-kex-caret-active]').count(), 0);
      assert.equal(await page.locator('[data-content-editable-leaf]').evaluate(host => getComputedStyle(host).backgroundImage), 'none');
    } finally { await page.close(); }
  });
  await test('an idle caret really turns off and on without pausing its animation', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.waitForFunction(() => getComputedStyle(document.querySelector('[data-content-editable-leaf]')).backgroundSize.startsWith('0px'), undefined, { timeout: 2500 });
      assert.deepEqual(await caretPixels(page), []);
      await page.waitForFunction(() => !getComputedStyle(document.querySelector('[data-content-editable-leaf]')).backgroundSize.startsWith('0px'), undefined, { timeout: 1500 });
      assert.ok((await caretPixels(page)).length > 10);
    } finally { await page.close(); }
  });
  await test('editor rerenders at the same insertion point cannot keep resetting the blink', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.evaluate(() => {
        window.rerenderTimer = setInterval(() => {
          const text = document.querySelector('.formatted'), offset = getSelection().anchorOffset;
          text.replaceChildren(document.createTextNode(text.textContent));
          const range = document.createRange(); range.setStart(text.firstChild, offset); range.collapse(true);
          getSelection().removeAllRanges(); getSelection().addRange(range);
        }, 120);
      });
      await page.waitForFunction(() => getComputedStyle(document.querySelector('[data-content-editable-leaf]')).backgroundSize.startsWith('0px'), undefined, { timeout: 2500 });
      assert.deepEqual(await caretPixels(page), []);
    } finally { await page.evaluate(() => clearInterval(window.rerenderTimer)); await page.close(); }
  });
  await test('an idle caret stops per-frame work and keeps no endless animation', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await page.waitForTimeout(800);
      const frames = await page.evaluate(() => new Promise(resolve => {
        const start = window.frameRequests;
        setTimeout(() => resolve(window.frameRequests - start), 1000);
      }));
      assert.ok(frames < 10, `an idle caret requested ${frames} frames in one second`);
      const timings = await page.locator('[data-content-editable-leaf]').evaluate(host => host.getAnimations().map(animation => animation.effect.getTiming().iterations));
      assert.ok(timings.length > 0 && timings.every(iterations => iterations !== Infinity), `endless animations: ${timings}`);
      assert.equal(await page.evaluate(() => document.documentElement.dataset.notionKexCaret), 'smooth');
    } finally { await page.close(); }
  });
  await test('text changes before an idle caret move it on the next frame', async () => {
    const page = await fixture({ sidebar: false });
    try {
      await select(page);
      await page.waitForFunction(() => document.documentElement.dataset.notionKexCaret === 'smooth');
      await holdBlink(page);
      await page.waitForTimeout(800);
      await page.evaluate(() => { window.caretX = () => parseFloat(getComputedStyle(document.querySelector('[data-content-editable-leaf]')).backgroundPosition); });
      const before = await page.evaluate(() => window.caretX());
      await page.locator('.formatted').evaluate(span => { const bold = document.createElement('b'); bold.textContent = 'Inserted '; span.before(bold); });
      await page.waitForFunction(x => window.caretX() > x + 20, before, { timeout: 150 });
      const moved = await page.evaluate(() => window.caretX());
      await page.locator('.formatted').evaluate(span => { span.firstChild.insertData(0, 'More '); });
      await page.waitForFunction(x => window.caretX() > x + 10, moved, { timeout: 150 });
    } finally { await page.close(); }
  });
  fs.writeFileSync(path.join(out, 'interactions.json'), JSON.stringify({ browser: browser.version(), headless: true, checks: results }, null, 2));
  console.log(JSON.stringify(results, null, 2));
} finally { await browser.close(); }
