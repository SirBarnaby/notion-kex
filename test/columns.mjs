import { chromium } from 'playwright';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const out = path.resolve('test-results');
fs.mkdirSync(out, { recursive: true });
// No desktop interaction and no signed-in user profile.
const browser = await chromium.launch({ channel: process.platform === 'win32' ? 'msedge' : undefined, headless: true });
const evidence = [];
try {
  const page = await browser.newPage({ viewport: { width: 716, height: 1000 } });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const group = (id, ratios, nested = false) => `<div class="notion-column_list-block" id="${id}" style="width:100%;max-width:524px;align-self:center"><div style="display:flex;position:relative">${ratios.map((ratio, index) => `${index ? '<div class="divider" style="width:46px;flex-shrink:0"></div>' : ''}<div class="notion-column-block" style="width:calc((100% - ${46 * (ratios.length - 1)}px) * ${ratio});flex-shrink:0;display:flex;flex-direction:column;padding-block:12px"><div class="text">Column ${index + 1}: wrapping text and native proportions.</div>${index === 0 && nested ? group('nested', [.5, .5]) : ''}${index === 0 ? '<div class="table-scroll"><table><tr><td>A wide table keeps its own horizontal scroll area.</td></tr></table></div>' : ''}</div>`).join('')}</div></div>`;
  // Structure and fixed max-width captured from the current public Notion renderer.
  await page.setContent(`<style>
    * {box-sizing:border-box} body {margin:0;background:#191919;color:#eee;font:16px/1.5 sans-serif}
    .notion-frame {width:100%}.layout {--content-width:minmax(auto,720px);--margin-width:minmax(96px,1fr);display:grid;width:100%;grid-template-columns:[full-start] var(--margin-left-width,var(--margin-width)) [content-start] var(--content-width) [content-end] var(--margin-right-width,var(--margin-width)) [full-end]}
    .layout-content {grid-column:content;min-width:0}.notion-page-content {width:100%;display:flex;flex-direction:column;align-items:stretch;min-width:0}
    .native-wrapper {width:100%;display:flex;flex-direction:column;align-items:stretch;min-width:0}.notion-column-block {min-width:0}.text {overflow-wrap:anywhere}
    .table-scroll {max-width:100%;overflow-x:auto} table {width:400px;border-collapse:collapse} td {border:1px solid #555;padding:8px}
    h2 {margin:20px 0 0;font-size:18px}.notion-column_list-block {outline:1px solid #444}
  </style><main class="notion-frame"><div class="whenContentEditable" contenteditable="true"><div class="layout"><div class="layout-content"><div class="notion-page-content">
    <h2>Two uneven columns with nested columns</h2><div class="native-wrapper">${group('two', [.3, .7], true)}</div>
    <h2>Three columns</h2><div class="native-wrapper">${group('three', [1/3, 1/3, 1/3])}</div>
    <h2>Five columns</h2><div class="native-wrapper">${group('five', [.2, .2, .2, .2, .2])}</div>
    <h2>A deliberately narrower group</h2><div class="native-wrapper">${group('custom', [.4, .6]).replace('width:100%;max-width:524px', 'width:60%;max-width:524px')}</div>
  </div></div></div></div></main>`);
  const original = await page.locator('.notion-page-content').textContent();
  // Reproduce the reported failure before the new stylesheet is applied.
  await page.locator('.layout').evaluate(e => {
    e.style.setProperty('--margin-left-width', '24px'); e.style.setProperty('--margin-right-width', '24px'); e.style.setProperty('--content-width', 'minmax(0,1fr)');
  });
  const before = await page.locator('#two').boundingBox();
  assert.equal(before.x, 96);
  assert.equal(before.width, 524);
  await page.locator('.layout').evaluate(e => e.removeAttribute('style'));
  await page.evaluate(() => {
    const listeners = [];
    window.chrome = {runtime:{},storage:{local:{get:(_key,callback)=>callback({}),set:(values,callback)=>{for(const listener of listeners)listener(Object.fromEntries(Object.entries(values).map(([key,newValue])=>[key,{newValue}])),'local');callback?.();}},onChanged:{addListener:listener=>listeners.push(listener),removeListener:listener=>listeners.splice(listeners.indexOf(listener),1)}}};
  });
  await page.addStyleTag({ path: 'extension/kex.css' });
  await page.addScriptTag({ path: 'extension/policy.js' });
  await page.addScriptTag({ path: 'extension/kex.js' });
  for (const width of [390, 650, 716, 960, 1099, 1100, 1440]) {
    await page.setViewportSize({ width, height: 1000 });
    // Notion recalculates its pixel cap on resize, even while KEX is enabled.
    await page.locator('.notion-column_list-block:not(#nested)').evaluateAll((groups, width) => groups.forEach(e => e.style.maxWidth = `${Math.min(720, width - 192)}px`), width);
    const expectedLeft = width < 520 ? 20 : width < 1100 ? 24 : Math.max(96, (width - 720) / 2);
    await page.waitForFunction(left => Math.abs(document.querySelector('#two').getBoundingClientRect().left - left) < .1, expectedLeft);
    const geometry = await page.evaluate(() => {
      const box = e => {const r=e.getBoundingClientRect();return {left:r.left,width:r.width};};
      const groups = Object.fromEntries(['two','three','five','nested','custom'].map(id => {const e=document.getElementById(id);return [id,{...box(e),columns:[...e.querySelectorAll('.notion-column-block')].filter(c=>c.closest('.notion-column_list-block')===e).map(box),dividers:[...e.querySelectorAll('.divider')].filter(c=>c.closest('.notion-column_list-block')===e).map(box)}];}));
      return {viewport:innerWidth,page:box(document.querySelector('.notion-page-content')),groups,overflow:document.documentElement.scrollWidth>innerWidth};
    });
    assert.equal(geometry.overflow, false);
    for (const id of ['two','three','five']) {
      const row = geometry.groups[id];
      assert.ok(Math.abs(row.left - geometry.page.left) < .1, `${id} left edge at ${width}`);
      assert.ok(Math.abs(row.width - geometry.page.width) < .1, `${id} width at ${width}`);
      const ratios = id === 'two' ? [.3,.7] : id === 'three' ? [1/3,1/3,1/3] : [.2,.2,.2,.2,.2];
      row.columns.forEach((column,index)=>assert.ok(Math.abs(column.width - (row.width - 46 * (ratios.length - 1)) * ratios[index]) < .1, `${id} ratio at ${width}`));
      row.dividers.forEach(divider=>assert.equal(divider.width,46));
    }
    if (width < 1100) {
      assert.ok(Math.abs(geometry.groups.nested.width - geometry.groups.two.columns[0].width) < .1);
      assert.ok(Math.abs(geometry.groups.custom.width - geometry.page.width * .6) < .1);
    }
    evidence.push({check:'column counts, ratios, nested groups, scroll containment and native wide layout',...geometry});
    if ([390,650,960].includes(width)) await page.screenshot({path:path.join(out,`columns-${width}.png`),fullPage:true});
  }
  await page.setViewportSize({width:716,height:1000});
  await page.locator('#two').evaluate(e=>e.style.maxWidth='524px');
  for (const settings of [{enabled:false},{responsive:false}]) {
    await page.evaluate(settings=>chrome.storage.local.set({'notion-kex.settings':settings}),settings);
    await page.waitForFunction(()=>document.querySelector('#two').getBoundingClientRect().left===96);
    assert.equal((await page.locator('#two').boundingBox()).width,524);
  }
  evidence.push({check:'disabling compact margins restores native column caps',passed:true});
  assert.equal(await page.locator('.notion-page-content').textContent(),original);
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(out,'columns.json'),JSON.stringify({browser:browser.version(),headless:true,checks:evidence},null,2));
  console.log(JSON.stringify({passed:evidence.length,browser:browser.version(),headless:true}));
} finally {await browser.close();}
