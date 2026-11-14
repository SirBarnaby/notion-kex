import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
const context = vm.createContext({});
vm.runInContext(readFileSync(new URL('../extension/policy.js', import.meta.url), 'utf8'), context);
const policy = context.NotionKexPolicy;

test('the page pane determines compact layout, including sidebar and split windows', () => {
  const config = policy.normalize();
  for (const width of [390, 520, 716, 960, 1099]) assert.equal(policy.compact(width, config), true);
  for (const width of [0, 1100, 1440]) assert.equal(policy.compact(width, config), false);
  assert.equal(policy.compact(716, policy.normalize({ enabled: false })), false);
  assert.equal(policy.compact(716, policy.normalize({ responsive: false })), false);
});
test('malformed and older settings retain usable defaults', () => {
  for (const value of [undefined, null, false, 'bad', []]) assert.equal(policy.normalize(value).gutter, 24);
  const config = policy.normalize({ gutter: 900, narrowAt: -5, caretDuration: Infinity, enabled: 'false', smoothCaret: false });
  assert.equal(config.gutter, 64); assert.equal(config.narrowAt, 640);
  assert.equal(config.caretDuration, 100); assert.equal(config.enabled, true); assert.equal(config.smoothCaret, false);
  assert.equal(policy.normalize({ touchpadZoom: false }).touchpadZoom, false);
  assert.equal(policy.normalize({ touchpadZoom: 'false' }).touchpadZoom, true);
  assert.equal(policy.normalize({ gutter: 24 }).touchpadZoom, true);
});
test('one compact gutter serves both page sides', () => {
  const config = policy.normalize();
  assert.equal(policy.gutter(390, config), 20);
  assert.equal(policy.gutter(650, config), 24);
  assert.equal(policy.gutter(390, policy.normalize({ gutter: 16 })), 16);
  assert.equal(policy.gutter(650, policy.normalize({ gutter: 40 })), 40);
});
test('the portable manifest requests only local settings and Notion origins', () => {
  const manifest = JSON.parse(readFileSync(new URL('../extension/manifest.json', import.meta.url)));
  assert.equal(manifest.name, 'Notion KEX');
  assert.deepEqual(manifest.permissions, ['storage']);
  assert.equal(manifest.content_scripts[0].run_at, 'document_start');
  assert.equal(manifest.content_scripts[0].matches.some((origin) => origin.includes('<all_urls>')), false);
  const source = readFileSync(new URL('../extension/kex.js', import.meta.url), 'utf8');
  assert.equal(/\b(fetch|XMLHttpRequest|WebSocket|eval)\s*\(/.test(source), false);
});
