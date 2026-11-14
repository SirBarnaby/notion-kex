import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { writeAsar, readAsar } from './asar-fixture.mjs';

const root = path.resolve('.scratch/native-zoom-runtime');
const source = path.join(process.env.LOCALAPPDATA, 'Programs/Notion');
const resources = path.join(root, 'resources');
fs.mkdirSync(resources, { recursive: true });
for (const name of fs.readdirSync(source).filter(name => /\.(?:exe|dll|pak|bin|dat)$/.test(name) && !/uninstall/i.test(name))) {
  const destination = path.join(root, name);
  if (!fs.existsSync(destination) || fs.statSync(destination).size !== fs.statSync(path.join(source, name)).size) fs.copyFileSync(path.join(source, name), destination);
}
fs.mkdirSync(path.join(root, 'locales'), { recursive: true });
fs.copyFileSync(path.join(source, 'locales/en-US.pak'), path.join(root, 'locales/en-US.pak'));
const exe = path.join(root, 'Notion.exe'), asar = path.join(resources, 'app.asar');
const html = '<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"><title>KEX native zoom fixture</title><style>html,body{margin:0;width:4000px;height:4000px;background:repeating-linear-gradient(90deg,#eee 0 99px,#aaa 100px)}</style><div id="target" style="position:absolute;left:400px;top:300px">Native zoom anchor</div>';
const members = {
  'package.json': JSON.stringify({ name: 'notion-kex-native-fixture', version: '1.0.0', main: '.webpack/main/index.js' }),
  '.webpack/main/index.js': `const {app,BrowserWindow,WebContentsView,session,Menu,ipcMain}=require("electron"); const path=require('path');
    app.setName('Notion KEX Native Fixture');
    app.setPath('userData',path.resolve(process.resourcesPath,'../profile'));
    app.setPath('sessionData',path.resolve(process.resourcesPath,'../profile'));
    app.commandLine.appendSwitch('disable-background-networking');
    app.whenReady().then(async()=>{const isolated=session.fromPartition('persist:kex-native-fixture');
      await isolated.protocol.handle('https',()=>new Response(${JSON.stringify(html)},{headers:{'content-type':'text/html'}}));
      const window=new BrowserWindow({show:false,opacity:0,focusable:false,skipTaskbar:true,x:40,y:40,width:900,height:700});
      const view=new WebContentsView({webPreferences:{preload:path.join(app.getAppPath(),'.webpack/renderer/tab_browser_view/preload.js'),session:isolated,contextIsolation:true,sandbox:true,nodeIntegration:false,backgroundThrottling:false}});
      const contents=view.webContents;
      view.setBounds({x:0,y:0,width:900,height:672});view.setVisible(false);
      window.setIgnoreMouseEvents(true); window.showInactive();
      let menuResets=0,layoutZoomEvents=0,lastInput;
      Menu.setApplicationMenu(Menu.buildFromTemplate([{label:'View',submenu:[{label:'Actual Size',accelerator:'CmdOrCtrl+0',click(){menuResets++;contents.setZoomFactor(1)}}]}]));
      ipcMain.handle('fixture:zoom-status',()=>({menuResets,layoutZoomEvents,lastInput,electron:process.versions.electron}));
      ipcMain.handle('fixture:native-key',event=>{event.sender.sendInputEvent({type:'keyDown',keyCode:'0',modifiers:['control']});event.sender.sendInputEvent({type:'keyUp',keyCode:'0',modifiers:[]})});
      ipcMain.handle('fixture:refresh-preferences',async event=>{event.sender.setImageAnimationPolicy('animateOnce');await event.sender.executeJavaScript('true')});
      contents.on('before-input-event',(_event,input)=>{lastInput=input});
      contents.on('did-finish-load',()=>contents.setZoomFactor(1));
      contents.on('zoom-changed',(_event,direction)=>{layoutZoomEvents++;contents.setZoomFactor(contents.getZoomFactor()+(direction==='in'?.1:-.1))});
      contents.on('preload-error',(_event,_path,error)=>console.error('PRELOAD',error));
      contents.on('console-message',(_event,details)=>console.error('RENDERER',details.message));
      window.on('closed',()=>app.quit()); await contents.loadURL('https://app.notion.com/kex-native-fixture');
      window.contentView.addChildView(view);view.setVisible(true);
    });`,
  '.webpack/renderer/tab_browser_view/preload.js': 'const {contextBridge,ipcRenderer}=require("electron");let pinchState;window.addEventListener("message",event=>{if(event.data?.type==="notion-kex:pinch-state")pinchState=event.data});contextBridge.exposeInMainWorld("__electronApi",{});contextBridge.exposeInMainWorld("__isElectron",true);contextBridge.exposeInMainWorld("__kexFixture",{pinchState:()=>pinchState,zoomStatus:()=>ipcRenderer.invoke("fixture:zoom-status"),nativeKey:()=>ipcRenderer.invoke("fixture:native-key"),refreshPreferences:()=>ipcRenderer.invoke("fixture:refresh-preferences")});',
  'sentinel.bin': Buffer.from([0, 1, 127, 255, 77, 42]),
  'empty.c': '',
  'same-offset.bin': Buffer.from([17, 23, 61]),
};
writeAsar(asar, members);
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const originalHash = hash(asar), exeHash = hash(exe);
const results = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

function nativeSetup(action) {
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.resolve('tools/NativeZoom.ps1'), '-Action', action, '-NotionData', path.join(root, 'profile'), '-NotionExe', exe], { encoding: 'utf8', windowsHide: true, env: { ...process.env, PSModulePath: path.join(process.env.SystemRoot, 'System32/WindowsPowerShell/v1.0/Modules') } });
  if (result.status !== 0) throw Error(result.stdout + result.stderr);
  return result.stdout.trim();
}

async function runtime(run) {
  const child = spawn(exe, ['--remote-debugging-pipe', '--disable-gpu'], { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'], windowsHide: true });
  let sequence = 0, buffer = '', stderr = '';
  const requests = new Map();
  child.stderr.on('data', bytes => { stderr = (stderr + bytes.toString()).slice(-4000); });
  child.stdio[4].on('data', bytes => {
    buffer += bytes.toString(); let at;
    while ((at = buffer.indexOf('\0')) >= 0) {
      const message = JSON.parse(buffer.slice(0, at)); buffer = buffer.slice(at + 1);
      if (requests.has(message.id)) { const request = requests.get(message.id); requests.delete(message.id); clearTimeout(request.timeout); message.error ? request.reject(Error(`${request.method}: ${JSON.stringify(message.error)}`)) : request.resolve(message.result); }
    }
  });
  const call = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++sequence;
    requests.set(id, { method, resolve, reject, timeout: setTimeout(() => { requests.delete(id); reject(Error(`Timeout: ${method} ${JSON.stringify(params)}; ${stderr}`)); }, 15000) });
    child.stdio[3].write(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }) + '\0');
  });
  try {
    let target;
    for (let attempt = 0; attempt < 100 && !target; attempt++) {
      const list = await call('Target.getTargets'); target = list.targetInfos.find(item => item.type === 'page' && item.url === 'https://app.notion.com/kex-native-fixture');
      if (!target) await pause(100);
    }
    if (!target) throw Error('Isolated fixture page was not created: ' + stderr);
    const { sessionId } = await call('Target.attachToTarget', { targetId: target.targetId, flatten: true });
    const evaluate = async expression => { const response = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, sessionId); if (response.exceptionDetails) throw Error(JSON.stringify(response.exceptionDetails)); return response.result.value; };
    const pinch = async scaleFactor => {
      const point = await evaluate('({x:Math.min(400,visualViewport.width/2),y:Math.min(300,visualViewport.height/2)})');
      return call('Input.synthesizePinchGesture', { ...point, scaleFactor, relativeSpeed: 800, gestureSourceType: 'mouse' }, sessionId);
    };
    const viewport = () => evaluate('({scale:visualViewport.scale,x:visualViewport.offsetLeft,y:visualViewport.offsetTop})');
    for (let attempt=0; attempt<100 && !await evaluate('document.readyState === "complete"'); attempt++) await pause(50);
    await pause(100);
    await run({ evaluate, pinch, viewport, call: (method, params) => call(method, params, sessionId), browser: await call('Browser.getVersion') });
  } finally {
    try { await call('Browser.close'); } catch { child.kill(); }
    await Promise.race([new Promise(resolve => child.once('exit', resolve)), pause(2000)]);
    if (child.exitCode === null) child.kill();
    for (const request of requests.values()) { clearTimeout(request.timeout); request.reject(Error('Fixture closed')); }
  }
}

try {
  await runtime(async ({ viewport, pinch, browser }) => {
    await pinch(1.6); const actual = await viewport();
    assert.equal(actual.scale, 1);
    results.push({ check: 'unmodified runtime disables native pinch', passed: true, browser: browser.product });
  });
  console.log(nativeSetup('Install'));
  const patched = readAsar(asar);
  for (const [name, bytes] of Object.entries(members)) if (!['.webpack/main/index.js','.webpack/renderer/tab_browser_view/preload.js'].includes(name)) assert.deepEqual(patched[name], Buffer.from(bytes));
  assert.equal(hash(exe), exeHash);
  results.push({ check: 'only the two declared JS entries change; executable and all other members are preserved', passed: true });
  const patchedHash = hash(asar); nativeSetup('Install'); assert.equal(hash(asar), patchedHash);
  await runtime(async ({evaluate,viewport,pinch}) => {
    let state;
    for(let attempt=0;attempt<100;attempt++){
      state=await evaluate('window.__kexFixture.pinchState()');
      if(state?.available&&state.enabled)break;
      await pause(20);
    }
    assert.equal(state?.enabled,true,'Initial native acknowledgement must precede the preference refresh.');
    await evaluate('window.__kexFixture.refreshPreferences()');
    await pinch(1.6);const actual=await viewport();
    assert.ok(actual.scale>1.5&&actual.scale<1.7, `Cold startup after host preference refresh, without a settings message: ${JSON.stringify(actual)}`);
    results.push({check:'cold startup survives host preference refresh without settings interaction',passed:true});
  });
  await runtime(async ({ evaluate, viewport, pinch, call, browser }) => {
    await evaluate(`(() => {
      const values = {}, listeners = [];
      window.chrome = { runtime: {}, storage: {
        local: { get: (_key, callback) => callback(values), set: next => {
          const changes = {}; for (const [key,newValue] of Object.entries(next)) { changes[key] = { oldValue:values[key],newValue }; values[key] = newValue; }
          for (const listener of listeners) listener(changes,'local');
        } }, onChanged: { addListener: listener => listeners.push(listener), removeListener() {} }
      } };
    })()`);
    await evaluate(fs.readFileSync('extension/policy.js', 'utf8'));
    await evaluate(fs.readFileSync('extension/kex.js', 'utf8'));
    const setPreference = async settings => {
      await evaluate(`chrome.storage.local.set({'notion-kex.settings':${JSON.stringify(settings)}})`);
      const enabled=settings.enabled!==false&&settings.touchpadZoom!==false;
      const expected = enabled ? 'native' : 'disabled';
      let state;
      for (let attempt=0;attempt<100;attempt++) {
        state=await evaluate('window.__kexFixture.pinchState()');
        if(state?.available&&state.enabled===enabled&&await evaluate('document.documentElement.dataset.notionKexPinch')===expected)break;
        await pause(20);
      }
      assert.equal(state?.enabled,enabled,'Wait for the native acknowledgement, not the optimistic settings marker.');
      assert.equal(await evaluate('document.documentElement.dataset.notionKexPinch'), expected);
    };
    await setPreference({ touchpadZoom:true });
    const before = await viewport(); await pinch(1.6); const after = await viewport();
    assert.ok(after.scale > 1.5 && after.scale < 1.7, JSON.stringify(after));
    assert.ok(Math.abs(before.x + 400 / before.scale - after.x - 400 / after.scale) < 3, JSON.stringify(after));
    assert.ok(Math.abs(before.y + 300 / before.scale - after.y - 300 / after.scale) < 3, JSON.stringify(after));
    assert.equal((await evaluate('window.__kexFixture.zoomStatus()')).layoutZoomEvents, 0);
    results.push({ check: 'native pointer-anchored visual zoom', passed: true, browser: browser.product, before, after });
    await evaluate('window.__kexFixture.refreshPreferences()');
    assert.ok(Math.abs((await viewport()).scale-after.scale)<0.01,'Preference refresh must retain the current pinch level.');
    results.push({check:'later preference refresh preserves both pinch support and the current view scale',passed:true});
    await evaluate('window.__kexFixture.nativeKey()');
    await pause(100); assert.equal((await viewport()).scale, 1,JSON.stringify(await evaluate('window.__kexFixture.zoomStatus()')));
    results.push({ check: 'Electron keyboard input reaches the main reset hook and preload IPC', passed: true, native: await evaluate('window.__kexFixture.zoomStatus()') });
    await setPreference({ touchpadZoom:false });
    await evaluate('window.__kexFixture.refreshPreferences()');
    await pinch(1.6); assert.equal((await viewport()).scale, 1);
    await setPreference({ touchpadZoom:true });
    await pinch(1.6); assert.ok((await viewport()).scale > 1.5);
    await setPreference({ enabled:false });
    assert.equal((await viewport()).scale, 1);
    await pinch(1.6); assert.equal((await viewport()).scale, 1);
    results.push({ check: 'real extension storage path and master switch control native pinch', passed: true });
    await setPreference({ enabled:true });
    await pinch(3); await pinch(2); assert.ok(Math.abs((await viewport()).scale - 5) < 0.01);
    await pinch(0.25); await pinch(0.5); assert.ok(Math.abs((await viewport()).scale - 1) < 0.01);
    results.push({ check: 'native visual zoom stays within 100%-500%', passed: true });
  });
  console.log(nativeSetup('Uninstall')); assert.equal(hash(asar), originalHash);
  results.push({ check: 'native patch removal restores the exact original archive', passed: true });
  fs.mkdirSync('test-results', { recursive: true });
  fs.writeFileSync('test-results/native-zoom.json', JSON.stringify({ isolated: true, userProfileAccessed: false, results }, null, 2));
  console.log(JSON.stringify(results, null, 2));
} catch (error) { console.error(error); process.exitCode = 1; }
