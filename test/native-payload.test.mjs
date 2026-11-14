import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../native/pinch.js', import.meta.url), 'utf8');
const settle = () => new Promise(resolve => setImmediate(resolve));
function fixture({ fail = false, mainFrame = true, hostname = 'app.notion.com' } = {}) {
  const handlers = {}, ipc = {}, calls = [], messages = [];
  const location = { protocol: 'https:', hostname, origin: `https://${hostname}` };
  const window = { addEventListener: (name, callback) => { handlers[name] = callback; }, postMessage: data => messages.push(data) };
  let rejectNext = fail;
  vm.runInNewContext(source, {
    process: { isMainFrame: mainFrame }, document: { readyState: 'complete' }, location, window, console: { warn() {} },
    require: name => { assert.equal(name, 'electron'); return { ipcRenderer: {
      on: (name,callback) => { ipc[name] = callback; },
      invoke: async (name,version,enabled,reset) => {
        assert.equal(name,'notion-kex:configure-pinch');assert.equal(version,1);
        calls.push({enabled,reset});
        if(rejectNext){rejectNext=false;throw Error('native failure');}
        return enabled;
      },
    } }; },
  });
  const send = (data, overrides = {}) => handlers.message?.({ source: window, origin: location.origin, data, ...overrides });
  return { calls, messages, handlers, ipc, send };
}

test('failed native initialization reports unavailable and a later preference retries it', async () => {
  const f = fixture({ fail: true }); await settle();
  assert.equal(f.messages.at(-1).available, false);
  assert.equal('enabled' in f.messages.at(-1), false);
  f.send({ type: 'notion-kex:pinch', version: 1, enabled: true }); await settle();
  assert.deepEqual(f.calls, [{enabled:true,reset:false},{enabled:true,reset:false}]);
  assert.equal(f.messages.at(-1).available, true);
  assert.equal(f.messages.at(-1).enabled, true);
});

test('native controls reject foreign frames, origins and malformed messages', async () => {
  const f = fixture(); await settle();
  const disable = { type: 'notion-kex:pinch', version: 1, enabled: false };
  f.send(disable, { source: {} });
  f.send(disable, { origin: 'https://elsewhere.example' });
  for (const data of [null, {}, { ...disable, version: 2 }, { ...disable, enabled: 'false' }]) f.send(data);
  await settle(); assert.deepEqual(f.calls, [{enabled:true,reset:false}]);
  assert.equal(Object.keys(fixture({ mainFrame: false }).handlers).length, 0);
  assert.equal(Object.keys(fixture({ hostname: 'notion.example' }).handlers).length, 0);
});

test('queued preference changes finish in order and reset uses the fixed native IPC contract', async () => {
  const f = fixture(); await settle();
  for (const enabled of [false, true]) f.send({ type: 'notion-kex:pinch', version: 1, enabled });
  await settle(); assert.deepEqual(f.calls, [{enabled:true,reset:false},{enabled:false,reset:false},{enabled:true,reset:false}]);
  f.ipc['notion-kex:reset-pinch']({}, 2); await settle(); assert.equal(f.calls.length, 3);
  f.ipc['notion-kex:reset-pinch']({}, 1); await settle();
  assert.deepEqual(f.calls.at(-1), {enabled:true,reset:true});
  assert.equal(f.messages.at(-1).enabled, true);
});

test('main shortcut hook only resets the top-level Notion page and leaves menu processing intact', () => {
  let created, input;
  const sends = [];
  let url = 'https://app.notion.com/test';
  const contents = { on: (name,callback) => { assert.equal(name,'before-input-event'); input=callback; }, getURL:()=>url, mainFrame:{send:(...args)=>sends.push(args)} };
  vm.runInNewContext(fs.readFileSync(new URL('../native/pinch-main.js',import.meta.url),'utf8'), { URL, require:()=>({ ipcMain:{handle(){}},app:{ on:(name,callback)=>{assert.equal(name,'web-contents-created');created=callback;} },webContents:{getAllWebContents:()=>[contents]} }) });
  created({},contents);
  const event = { preventDefault:()=>assert.fail('Native menu processing must continue') };
  const key = {type:'keyDown',control:true,code:'Digit0'};
  for(const extra of [{isAutoRepeat:true},{isComposing:true},{alt:true},{shift:true},{meta:true},{control:false},{code:'Digit1'},{type:'keyUp'}]) input(event,{...key,...extra});
  url='https://notion.example/test';input(event,key);assert.equal(sends.length,0);
  url='https://app.notion.com/test';input(event,key);assert.deepEqual(sends,[['notion-kex:reset-pinch',1]]);
});

test('main process applies persistent limits only for typed requests from the Notion main frame', async () => {
  let configure;
  const calls=[];
  const frame={};let url='https://app.notion.com/test';
  const contents={mainFrame:frame,getURL:()=>url,setVisualZoomLevelLimits:async(...limits)=>calls.push(limits)};
  vm.runInNewContext(fs.readFileSync(new URL('../native/pinch-main.js',import.meta.url),'utf8'),{URL,require:()=>({ipcMain:{handle:(name,callback)=>{assert.equal(name,'notion-kex:configure-pinch');configure=callback;}},app:{on(){}},webContents:{getAllWebContents:()=>[]}})});
  const event={sender:contents,senderFrame:frame};
  for(const args of [[2,true,false],[1,'true',false],[1,true,undefined]])await assert.rejects(configure(event,...args),/Invalid native pinch request/);
  await assert.rejects(configure({...event,senderFrame:{}},1,true,false),/Invalid native pinch request/);
  url='https://notion.example/test';await assert.rejects(configure(event,1,true,false),/Invalid native pinch request/);
  assert.equal(calls.length,0);
  url='https://app.notion.com/test';
  assert.equal(await configure(event,1,true,false),true);
  assert.equal(await configure(event,1,false,false),false);
  assert.equal(await configure(event,1,true,true),true);
  assert.deepEqual(calls,[[1,5],[1,1],[1,1],[1,5]]);
});

test('pending resets cannot enable zoom on a replacement document or supersede newer settings', async () => {
  for(const scenario of ['navigation','newer setting']){
    let configure,release;
    const calls=[],frame={};let url='https://app.notion.com/test';
    const gate=new Promise(resolve=>{release=resolve;});
    const contents={mainFrame:frame,getURL:()=>url,setVisualZoomLevelLimits:async(...limits)=>{calls.push(limits);if(calls.length===1)await gate;}};
    vm.runInNewContext(fs.readFileSync(new URL('../native/pinch-main.js',import.meta.url),'utf8'),{URL,require:()=>({ipcMain:{handle:(_name,callback)=>{configure=callback;}},app:{on(){}},webContents:{getAllWebContents:()=>[]}})});
    const event={sender:contents,senderFrame:frame};
    const pending=configure(event,1,true,true);
    if(scenario==='navigation'){url='https://elsewhere.example/';contents.mainFrame={};}
    else assert.equal(await configure(event,1,false,false),false);
    release();await assert.rejects(pending,/Stale native pinch request/);
    assert.deepEqual(calls,scenario==='navigation'?[[1,1]]:[[1,1],[1,1]]);
  }
});
