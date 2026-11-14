import { chromium } from 'playwright';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const out=path.resolve('test-results');
fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({channel: process.platform === 'win32' ? 'msedge' : undefined,headless:true});
const evidence=[];
try {
  const page=await browser.newPage({viewport:{width:650,height:1100}});
  const errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  // Pixel caps and 8px outer tab padding observed in the public Notion renderer.
  await page.setContent(`<style>
    *{box-sizing:border-box} body{margin:0;font:16px/1.5 monospace;color:#eee;background:#191919}
    .notion-frame{width:100%}.notion-scroller{height:1050px;overflow-y:scroll;overflow-x:hidden}
    .layout{--content-width:minmax(auto,720px);--margin-width:minmax(96px,1fr);display:grid;width:100%;grid-template-columns:[full-start] var(--margin-left-width,var(--margin-width)) [content-start] var(--content-width) [content-end] var(--margin-right-width,var(--margin-width)) [full-end]}
    .layout-wide-right-margin-expanded{--content-width:1fr;--margin-left-width:96px;--margin-right-width:468px}
    .layout-full{grid-column:full}.cover{height:100px;background:#397ad8}.layout-content{grid-column:content;min-width:0}
    .notion-page-content,.wrapper{width:100%;display:flex;flex-direction:column;align-items:stretch;min-width:0}
    .fluid{width:100%;max-width:443px;overflow-wrap:anywhere}.notion-header-block{font-size:28px;font-weight:bold;margin-block:18px}
    .notion-tab-block{padding:8px;margin-top:12px}.tab-card{border:1px solid #444;border-radius:16px;min-width:0}
    .tab-nav{display:flex;white-space:nowrap;overflow-x:auto;gap:8px;padding:12px}
    button{font:inherit;color:inherit;background:#292929;border:0;border-radius:12px;padding:4px 8px}
    .tab-panel{padding:0 16px 16px;min-width:0}.notion-quote-block{border-inline-start:3px solid #eee;padding-inline-start:12px;margin-block:12px}
    .notion-callout-block{border:1px solid #444;border-radius:12px;padding:12px;margin-block:12px}
    .notion-code-block pre{overflow-x:auto;margin:0;padding:8px;background:#292929}
    .notion-image-block,.notion-video-block{height:40px;background:#555;margin-block:8px}
  </style><main class="notion-frame"><div class="notion-scroller"><div class="whenContentEditable" contenteditable="true"><div class="layout">
    <div class="layout-full cover"></div><div class="layout-content"><div class="notion-page-content">
      <div id="heading" class="notion-selectable notion-header-block fluid">A page with tabs and notes</div>
      <div id="paragraph" class="notion-selectable notion-text-block fluid">A paragraph should use the full available content width, with equal space at both page edges.</div>
      <div id="todo" class="notion-selectable notion-to_do-block fluid">☐ A task with enough text to show how its line wrapping uses the page width.</div>
      <div id="list" class="notion-selectable notion-bulleted_list-block fluid">• A list follows the same content width.</div>
      <div class="wrapper"><div id="tabs" class="notion-selectable notion-tab-block fluid"><div class="tab-card">
        <div class="tab-nav" role="tablist"><button data-panel="a" role="tab">WEEK 3</button><button data-panel="b" role="tab">WEEK 4</button><button role="tab">WEEK 5</button><button role="tab">WEEK 6</button><button role="tab">WEEK 7</button><button role="tab">WEEK 8</button></div>
        <div class="tab-panel" id="a" role="tabpanel"><div id="quote" class="notion-selectable notion-quote-block fluid">A quote keeps its native border and inset.</div><div id="callout" class="notion-selectable notion-callout-block fluid">A nested callout uses the tab panel's available width.</div><div id="code" class="notion-selectable notion-code-block fluid"><pre>A long code line remains horizontally scrollable inside its own code block rather than widening the page. 012345678901234567890123456789</pre></div></div>
        <div class="tab-panel" id="b" role="tabpanel" hidden><div id="second" class="notion-selectable notion-text-block fluid">Switching a tab keeps both sides aligned.</div></div>
      </div></div></div>
      <div id="image" class="notion-selectable notion-image-block" style="width:200px;max-width:200px"></div>
      <div id="video" class="notion-selectable notion-video-block" style="width:240px;max-width:240px"></div>
    </div></div></div></div></div></main>`);
  const original=await page.locator('.notion-page-content').textContent();
  await page.evaluate(()=>{
    const listeners=[];
    window.chrome={runtime:{},storage:{local:{get:(_key,cb)=>cb({}),set:(values,cb)=>{for(const listener of listeners)listener(Object.fromEntries(Object.entries(values).map(([key,newValue])=>[key,{newValue}])),'local');cb?.();}},onChanged:{addListener:l=>listeners.push(l),removeListener:l=>listeners.splice(listeners.indexOf(l),1)}}};
    document.querySelectorAll('[data-panel]').forEach(button=>button.addEventListener('click',()=>document.querySelectorAll('[role=tabpanel]').forEach(panel=>panel.hidden=panel.id!==button.dataset.panel)));
  });
  const nativeCaps=()=>page.evaluate(()=>{
    const width=document.querySelector('.layout').getBoundingClientRect().width;
    document.querySelectorAll('.fluid').forEach(e=>e.style.maxWidth=`${Math.min(720,width-192)-(e.closest('.tab-panel')?50:0)}px`);
  });
  const measure=()=>page.evaluate(()=>{
    const box=e=>{const r=e.getBoundingClientRect();return {left:r.left,right:r.right,width:r.width};};
    const layout=box(document.querySelector('.layout')),content=box(document.querySelector('.notion-page-content'));
    return {viewport:innerWidth,layout,content,left:content.left-layout.left,right:layout.right-content.right,tab:box(document.getElementById('tabs')),card:box(document.querySelector('.tab-card')),cover:box(document.querySelector('.cover')),overflow:document.documentElement.scrollWidth>innerWidth};
  });
  await nativeCaps();
  await page.addStyleTag({path:'extension/kex.css'});
  await page.addScriptTag({path:'extension/policy.js'});
  await page.addScriptTag({path:'extension/kex.js'});
  for(const width of [390,640,650,960,1440]){
    await page.setViewportSize({width,height:1100});
    await nativeCaps();
    await page.waitForFunction(()=>{
      const l=document.querySelector('.layout').getBoundingClientRect(),p=document.querySelector('.notion-page-content').getBoundingClientRect();
      const gutter=l.width<520?20:l.width<1100?24:Math.max(96,(l.width-720)/2);
      return Math.abs(p.left-l.left-gutter)<.1&&Math.abs(l.right-p.right-gutter)<.1;
    });
    const geometry=await measure();
    assert.ok(Math.abs(geometry.left-geometry.right)<.1);
    assert.ok(Math.abs(geometry.tab.left-geometry.content.left)<.1);
    assert.ok(Math.abs(geometry.tab.right-geometry.content.right)<.1);
    assert.ok(Math.abs(geometry.card.left-geometry.layout.left-(geometry.left+8))<.1);
    assert.ok(Math.abs(geometry.layout.right-geometry.card.right-(geometry.right+8))<.1);
    assert.equal(geometry.cover.width,geometry.layout.width);
    assert.equal(geometry.overflow,false);
    for(const id of ['heading','paragraph','todo','list','quote','callout','code']){
      const fit=await page.locator(`#${id}`).evaluate(e=>({width:e.getBoundingClientRect().width,parent:e.parentElement.clientWidth-parseFloat(getComputedStyle(e.parentElement).paddingLeft)-parseFloat(getComputedStyle(e.parentElement).paddingRight)}));
      if(geometry.layout.width<1100)assert.ok(Math.abs(fit.width-fit.parent)<.1,`${id} fills its own parent at ${width}`);
    }
    assert.equal((await page.locator('#image').boundingBox()).width,200);
    assert.equal((await page.locator('#video').boundingBox()).width,240);
    evidence.push({check:'equal gutters and fluid text/tab width, native internal insets and media sizes',...geometry});
    if(width===640)await page.screenshot({path:path.join(out,'gutters-640.png'),fullPage:true});
  }
  await page.setViewportSize({width:650,height:1100});
  await nativeCaps();
  await page.locator('[data-panel=b]').click();
  await page.waitForFunction(()=>!document.getElementById('b').hidden);
  assert.ok(Math.abs((await page.locator('#second').boundingBox()).width-(await page.locator('#b').evaluate(e=>e.clientWidth-32)))<.1);
  await page.locator('[data-panel=a]').click();
  await page.evaluate(()=>document.documentElement.dir='rtl');
  await page.waitForFunction(()=>{const l=document.querySelector('.layout').getBoundingClientRect(),p=document.querySelector('.notion-page-content').getBoundingClientRect();return Math.abs(p.left-l.left-24)<.1&&Math.abs(l.right-p.right-24)<.1;});
  evidence.push({check:'tab changes and RTL keep both gutters aligned',passed:true});
  await page.evaluate(()=>document.documentElement.dir='ltr');
  await page.evaluate(()=>chrome.storage.local.set({'notion-kex.settings':{gutter:36}}));
  await page.waitForFunction(()=>{const l=document.querySelector('.layout').getBoundingClientRect(),p=document.querySelector('.notion-page-content').getBoundingClientRect();return p.left-l.left===36&&l.right-p.right===36;});
  evidence.push({check:'custom gutter applies to both edges',passed:true});
  await page.setViewportSize({width:960,height:1100});
  await page.evaluate(()=>{chrome.storage.local.set({'notion-kex.settings':{}});document.querySelector('.layout').classList.add('layout-wide-right-margin-expanded');});
  await page.waitForFunction(()=>{const l=document.querySelector('.layout').getBoundingClientRect(),p=document.querySelector('.notion-page-content').getBoundingClientRect();return p.left-l.left===24&&l.right-p.right===468;});
  evidence.push({check:'native reserved right-side panel space is retained',passed:true});
  await page.evaluate(()=>document.querySelector('.layout').classList.remove('layout-wide-right-margin-expanded'));
  await page.setViewportSize({width:650,height:1100});
  await nativeCaps();
  for(const settings of [{enabled:false},{responsive:false}]){
    await page.evaluate(s=>chrome.storage.local.set({'notion-kex.settings':s}),settings);
    await page.waitForFunction(()=>{const l=document.querySelector('.layout').getBoundingClientRect(),p=document.querySelector('.notion-page-content').getBoundingClientRect();return p.left-l.left===96&&l.right-p.right===96;});
    assert.ok((await page.locator('#tabs').evaluate(e=>getComputedStyle(e).maxWidth)).endsWith('px'));
  }
  evidence.push({check:'disable restores native gutters and block caps',passed:true});
  assert.equal(await page.locator('.notion-page-content').textContent(),original);
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(out,'gutters.json'),JSON.stringify({headless:true,browser:browser.version(),checks:evidence},null,2));
  console.log(JSON.stringify({headless:true,passed:evidence.length,browser:browser.version()}));
}finally{await browser.close();}
