// Anonymous public-page inspection only; never attaches to the user's desktop/session.
import { chromium } from 'playwright';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const browser = await chromium.launch({channel:'msedge',headless:true});
try {
  const page = await browser.newPage({viewport:{width:650,height:1100}});
  const networkFailures=[];
  page.on('requestfailed',request=>networkFailures.push({path:new URL(request.url()).pathname,error:request.failure()?.errorText}));
  await page.route('**/*',r=>['image','media','font'].includes(r.request().resourceType())?r.abort():r.continue());
  await page.goto(process.argv[2] || 'https://j-ann.notion.site/codenames-how-to', {waitUntil:'domcontentloaded'});
  try {await page.waitForSelector('.notion-page-content',{timeout:25000});}
  catch(error){console.error(JSON.stringify({publicPageLoadFailed:true,title:await page.title(),origin:new URL(page.url()).origin,networkFailures:networkFailures.slice(-8)}));throw error;}
  for(let attempt=0;attempt<8&&!await page.locator('.notion-tab-block').count();attempt++){
    await page.evaluate(()=>{const s=document.querySelector('.notion-scroller.vertical');if(s)s.scrollTop+=800;});
    await page.waitForTimeout(150);
  }
  const measure=()=>page.evaluate(()=>{
    const box=e=>{const r=e.getBoundingClientRect(),s=getComputedStyle(e);return {className:e.className,style:e.getAttribute('style'),left:r.left,right:r.right,width:r.width,maxWidth:s.maxWidth,paddingLeft:s.paddingLeft,paddingRight:s.paddingRight};};
    const groups=[...document.querySelectorAll('.notion-tab-block')];
    const chain=groups.slice(0,2).map(e=>{const c=[];for(let n=e;n&&c.length<8;n=n.parentElement)c.push(box(n));return c;});
    const caps={};
    for(const e of document.querySelectorAll('.notion-page-content .notion-selectable')){
      const type=[...e.classList].find(c=>/^notion-.*-block$/.test(c));
      if(type&&!caps[type]&&/px$/.test(e.style.maxWidth))caps[type]=box(e);
    }
    return {tabs:groups.length,chain,caps,content:box(document.querySelector('.notion-page-content'))};
  });
  const before=await measure();
  await page.addStyleTag({path:'extension/kex.css'});
  await page.addScriptTag({path:'extension/policy.js'});
  await page.addScriptTag({path:'extension/kex.js'});
  await page.waitForTimeout(350);
  const after=await measure();
  if(before.tabs){
    const tab=after.chain[0][0];
    assert.ok(Math.abs(tab.left-after.content.left)<.1);
    assert.ok(Math.abs(tab.right-after.content.right)<.1);
    assert.equal(tab.paddingLeft,before.chain[0][0].paddingLeft);
    assert.equal(tab.paddingRight,before.chain[0][0].paddingRight);
  }
  for(const type of ['notion-bulleted_list-block','notion-header-block','notion-text-block','notion-sub_header-block']){
    const block=after.caps[type];
    if(block){assert.equal(block.maxWidth,'100%');assert.ok(Math.abs(block.right-after.content.right)<.1);}
  }
  for(const type of ['notion-image-block','notion-video-block']){
    const old=before.caps[type],next=after.caps[type];
    if(old&&next&&/(?:^|;)\s*width:\s*[\d.]+px;/.test(old.style))assert.equal(next.width,old.width);
  }
  const evidence={publicPageOnly:true,headless:true,source:process.argv[2]||'https://j-ann.notion.site/codenames-how-to',version:JSON.parse(fs.readFileSync('extension/manifest.json','utf8')).version,before,after};
  fs.mkdirSync('test-results',{recursive:true});
  fs.writeFileSync(`test-results/public-${before.tabs?'tabs':'text'}-widths.json`,JSON.stringify(evidence,null,2));
  console.log(JSON.stringify({headless:true,version:evidence.version,before:{content:before.content,tabs:before.chain[0]?.[0]},after:{content:after.content,tabs:after.chain[0]?.[0]},textKinds:Object.keys(after.caps)}));
} finally {await browser.close();}
