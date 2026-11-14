// Development-only private-pipe controller. Not included in the distributable.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';
const executable = path.join(process.env.LOCALAPPDATA, 'Programs/Notion/Notion.exe');
const child = spawn(executable, ['--remote-debugging-pipe'], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });
let sequence = 0;
const pending = new Map();
let buffer = '';
const contexts = new Map();
function call(method, params = {}, sessionId) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Timeout: ${method}`)); }, 15000);
    pending.set(id, { resolve, reject, timer });
    child.stdio[3].write(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }) + '\0');
  });
}
child.stdio[4].on('data', (data) => {
  buffer += data.toString(); let index;
  while ((index = buffer.indexOf('\0')) >= 0) {
    const raw = buffer.slice(0, index); buffer = buffer.slice(index + 1);
    if (!raw) continue;
    const message = JSON.parse(raw); const entry = pending.get(message.id);
    if (message.method === 'Runtime.executionContextCreated') {
      const list = contexts.get(message.sessionId) || [];
      list.push(message.params.context); contexts.set(message.sessionId, list);
    }
    if (entry) { clearTimeout(entry.timer); pending.delete(message.id); message.error ? entry.reject(new Error(message.error.message)) : entry.resolve(message.result); }
  }
});
child.on('error', (error) => { console.error(error.message); process.exitCode = 1; });
child.on('exit', () => { console.log(JSON.stringify({ event: 'notion-exited' })); process.exit(); });
const sessions = new Map();
async function targets() {
  const { targetInfos } = await call('Target.getTargets');
  return targetInfos.filter((target) => target.type === 'page' && /^https:\/\/(app\.notion\.com|www\.notion\.so)\//.test(target.url));
}
async function session(index) {
  const pages = await targets(); const page = pages[index || 0];
  if (!page) throw new Error('No matching Notion page.');
  if (!sessions.has(page.targetId)) sessions.set(page.targetId, (await call('Target.attachToTarget', { targetId: page.targetId, flatten: true })).sessionId);
  return { page, id: sessions.get(page.targetId) };
}
const input = createInterface({ input: process.stdin });
input.on('line', async (line) => {
  if (!line.trim()) return;
  let request;
  try {
    request = JSON.parse(line);
    let result;
    if (request.action === 'pages') result = (await targets()).map((target, index) => ({ index, type: target.type, targetId: target.targetId }));
    else if (request.action === 'protocol' && request.page === undefined && !request.targetId) result = await call(request.method, request.params);
    else if (request.action === 'surfaces') {
      const { targetInfos } = await call('Target.getTargets');
      result = targetInfos.filter(t => t.type === 'page').map(t => ({ targetId: t.targetId, scheme: t.url.split(':')[0], internalPath: /^(file|chrome-extension):/.test(t.url) ? t.url : undefined }));
    }
    else if (request.action === 'quit') { await call('Browser.close'); result = 'closing'; }
    else {
      const { id, page } = request.targetId
        ? { id: (await call('Target.attachToTarget', { targetId: request.targetId, flatten: true })).sessionId, page: { targetId: request.targetId } }
        : await session(request.page);
      if (request.action === 'evaluate') {
        const value = await call('Runtime.evaluate', { expression: request.expression, returnByValue: true, awaitPromise: true, ...(request.contextId ? {contextId: request.contextId} : {}) }, id);
        if (value.exceptionDetails) throw new Error(value.exceptionDetails.text);
        result = value.result.value;
      } else if (request.action === 'bounds') {
        const { windowId } = await call('Browser.getWindowForTarget', { targetId: page.targetId });
        await call('Browser.setWindowBounds', { windowId, bounds: { windowState: 'normal' } });
        await call('Browser.setWindowBounds', { windowId, bounds: { width: request.width, height: request.height } });
        result = true;
      } else if (request.action === 'screenshot') {
        const value = await call('Page.captureScreenshot', { format: 'png' }, id);
        const output = path.resolve('test-results', path.basename(request.file)); mkdirSync(path.dirname(output), { recursive: true });
        writeFileSync(output, Buffer.from(value.data, 'base64')); result = output;
      } else if (request.action === 'key') {
        await call('Input.dispatchKeyEvent', request.params, id); result = true;
      } else if (request.action === 'front') {
        await call('Page.bringToFront', {}, id); result = true;
      } else if (request.action === 'reload') {
        await call('Page.reload', {}, id); result = true;
      } else if (request.action === 'emulate') {
        await call('Emulation.setEmulatedMedia', { features: request.features || [] }, id); result = true;
      } else if (request.action === 'contexts') {
        await call('Runtime.enable', {}, id);
        result = (contexts.get(id) || []).map(c => ({id:c.id,origin:c.origin,name:c.name,isDefault:c.auxData?.isDefault}));
      } else if (request.action === 'protocol') {
        result = await call(request.method, request.params, id);
      } else throw new Error('Unknown action.');
    }
    console.log(JSON.stringify({ id: request.id, result }));
  } catch (error) { console.log(JSON.stringify({ id: request?.id, error: error.message })); }
});
console.log(JSON.stringify({ event: 'ready', browser: await call('Browser.getVersion') }));
