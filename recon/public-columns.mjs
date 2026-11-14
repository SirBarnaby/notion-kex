// Public renderer reconnaissance only. Fresh headless browser; no desktop or user profile access.
import { chromium } from 'playwright';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const root = '.scratch/column-recon';
const browser = await chromium.launch({ channel: 'msedge', headless: true });
const pending = [];
try {
  const page = await browser.newPage({ viewport: { width: 716, height: 1000 } });
  await page.route('**/*', route => ['image', 'media', 'font'].includes(route.request().resourceType()) ? route.abort() : route.continue());
  page.on('response', response => {
    if (!/https:\/\/(?:[^/]+\.notion\.site|app\.notion\.com)\/_assets\/[^/]+\.js$/.test(response.url())) return;
    pending.push(response.text().then(source => {
      if (source.includes('240414(e') || source.includes('layout-nested-grid')) {
        const file = new URL(response.url()).pathname.split('/').at(-1);
        fs.writeFileSync(`${root}/${file}`, source);
        console.log(JSON.stringify({ renderer: file, selectableModule: source.includes('240414(e'), nestedGrid: source.includes('layout-nested-grid') }));
      }
    }).catch(() => {}));
  });
  await page.goto('https://notioner.notion.site/9d9c6a658f14412196a2be51e467c9ef', { waitUntil: 'domcontentloaded' });
  await page.waitForSelector('.notion-page-content', { timeout: 25000 });
  console.log(JSON.stringify(await page.evaluate(() => {
    const box = e => { const s = getComputedStyle(e), r = e.getBoundingClientRect(); return { className:e.className, style:e.getAttribute('style'), width:r.width, x:r.x, paddingLeft:s.paddingLeft, paddingRight:s.paddingRight, gridColumn:s.gridColumn }; };
    return { columnGroups: [...document.querySelectorAll('.notion-column_list-block')].slice(0, 3).map(e => { const chain=[]; for(let n=e;n&&chain.length<7;n=n.parentElement) chain.push(box(n));return chain; }), page:box(document.querySelector('.notion-page-content')) };
  })));
  const before = await page.locator('.notion-column_list-block').first().boundingBox();
  await page.addStyleTag({path:'extension/kex.css'});
  await page.addScriptTag({path:'extension/policy.js'});
  await page.addScriptTag({path:'extension/kex.js'});
  await page.waitForFunction(() => {
    const group=document.querySelector('.notion-column_list-block'),content=group?.closest('.notion-page-content');
    return content && Math.abs(group.getBoundingClientRect().width-content.getBoundingClientRect().width)<.1 && group.closest('.notion-frame')?.dataset.notionKexCompact==='true';
  });
  const after = await page.locator('.notion-column_list-block').first().boundingBox();
  assert.equal(after.x,24);
  assert.ok(after.width>before.width);
  const result={headless:true,publicPageOnly:true,before:{left:before.x,width:before.width},after:{left:after.x,width:after.width},version:JSON.parse(fs.readFileSync('extension/manifest.json','utf8')).version};
  fs.mkdirSync('test-results',{recursive:true});
  fs.writeFileSync('test-results/public-columns.json',JSON.stringify(result,null,2));
  console.log(JSON.stringify(result));
  await Promise.allSettled(pending);
} finally { await browser.close(); }
