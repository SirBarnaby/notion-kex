import fs from 'node:fs';
import path from 'node:path';
import { writeAsar } from './asar-fixture.mjs';
const [directory, version = '1', integrity = '0'] = process.argv.slice(2);
if (!directory) throw Error('Fixture directory required');
fs.mkdirSync(path.join(directory, 'resources'), { recursive: true });
const fuse = Buffer.concat([Buffer.from('Fixture only; never executed. dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX'), Buffer.from([1, 9]), Buffer.from(`0100${integrity}1011`)]);
fs.writeFileSync(path.join(directory, 'Notion.exe'), fuse);
writeAsar(path.join(directory, 'resources/app.asar'), {
  '.webpack/main/index.js': `// Fixture ${version}: setupExtensions isChromeExtensionsEnabled isChromeExtensionsAllowed loadExtension\nrequire("electron");`,
  '.webpack/renderer/tab_browser_view/preload.js': 'const {contextBridge}=require("electron");contextBridge.exposeInMainWorld("__electronApi",{});contextBridge.exposeInMainWorld("__isElectron",true);',
  'empty.c': '',
  'unrelated.bin': Buffer.from([0, 1, 2, 255, Number(version)]),
});
