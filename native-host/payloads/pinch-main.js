;/* Notion KEX native pinch reset v1 */
(() => {
  'use strict';
  const { app, webContents, ipcMain } = require('electron');
  const isNotion = contents => {
    try {
      const url = new URL(contents.getURL());
      return url.protocol === 'https:' && ['app.notion.com', 'www.notion.so', 'notion.so'].includes(url.hostname);
    } catch { return false; }
  };
  const requests = new WeakMap();
  ipcMain.handle('notion-kex:configure-pinch', async (event, version, enabled, reset) => {
    const contents = event.sender;
    if (version !== 1 || typeof enabled !== 'boolean' || typeof reset !== 'boolean' || event.senderFrame !== contents.mainFrame || !isNotion(contents)) throw Error('Invalid native pinch request.');
    const frame = event.senderFrame, request = {};
    requests.set(contents, request);
    const assertCurrent = () => {
      if (requests.get(contents) !== request || frame !== contents.mainFrame || !isNotion(contents)) throw Error('Stale native pinch request.');
    };
    // The main API stores the limits in WebPreferences, surviving later preference refreshes.
    if (reset || !enabled) await contents.setVisualZoomLevelLimits(1, 1);
    assertCurrent();
    if (enabled) await contents.setVisualZoomLevelLimits(1, 5);
    assertCurrent();
    return enabled;
  });
  const attached = new WeakSet();
  const attach = contents => {
    if (attached.has(contents)) return;
    attached.add(contents);
    contents.on('before-input-event', (_event, input) => {
      if (input.type !== 'keyDown' || input.isAutoRepeat || input.isComposing || !input.control || input.meta || input.alt || input.shift || !['Digit0', 'Numpad0'].includes(input.code)) return;
      try {
        if (isNotion(contents)) contents.mainFrame.send('notion-kex:reset-pinch', 1);
      } catch { /* A closing or navigating view has no zoom to reset. */ }
    });
  };
  app.on('web-contents-created', (_event, contents) => attach(contents));
  for (const contents of webContents.getAllWebContents()) attach(contents);
  // The only safe repair point is a full quit. Start the hidden launcher
  // watcher just before Notion leaves, including when it quits to update.
  // The watcher waits for the updater and Notion itself to finish, then
  // repairs with the same transaction the launcher uses. It never closes
  // Notion and it exits immediately when the patch is already current.
  let watchStarted = false;
  const startWatcher = () => {
    if (watchStarted) return;
    watchStarted = true;
    try {
      const { spawn } = require('child_process');
      const { existsSync } = require('fs');
      const { join } = require('path');
      const launcher = join(process.env.LOCALAPPDATA || '', 'NotionKEX', 'Notion KEX.exe');
      if (!existsSync(launcher)) return;
      spawn(launcher, ['--watch'], { detached: true, stdio: 'ignore', windowsHide: true }).unref();
    } catch { /* The next launch or sign-in check repairs instead. */ }
  };
  app.on('before-quit-for-update', startWatcher);
  app.on('before-quit', startWatcher);
})();
