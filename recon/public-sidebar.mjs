// Public assets only, in an isolated headless browser without a user profile.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
const out = path.resolve('.scratch/sidebar-recon');
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ channel: 'msedge', headless: true });
const pending = [];
try {
  const page = await browser.newPage();
  page.on('response', response => {
    if (response.request().resourceType() !== 'script' || !response.url().includes('/_assets/')) return;
    pending.push((async () => {
      const source = await response.text();
      fs.writeFileSync(path.join(out, new URL(response.url()).pathname.split('/').at(-1)), source);
    })().catch(() => {}));
  });
  await page.goto('https://www.notion.so/login', { waitUntil: 'domcontentloaded', timeout: 30000 });
  await page.waitForTimeout(4000);
  await Promise.all(pending);
  console.log(JSON.stringify({ anonymous: true, scripts: fs.readdirSync(out).length }));
} finally { await browser.close(); }
