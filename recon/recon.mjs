#!/usr/bin/env node
// Notion KEX recon: layout-only. Launches the installed Notion with a DevTools
// pipe (falls back to a localhost port), then for each open Notion tab records
// the page layout STRUCTURE (class names, inline layout styles, computed
// padding/width, matching CSS rules). It never reads page text, cookies or storage.
// Usage: node recon.mjs [pipe|port]   (default: pipe, then port)
import { spawn, execSync } from "node:child_process";
import { writeFileSync, mkdirSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "out"); mkdirSync(OUT, { recursive: true });
const NOTION = join(process.env.LOCALAPPDATA, "Programs", "Notion", "Notion.exe");
const log = (...a) => { console.log(...a); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function notionRunning() {
  try { return execSync('tasklist /FI "IMAGENAME eq Notion.exe" /NH', { encoding: "utf8" }).includes("Notion.exe"); } catch { return false; }
}

class Cdp {
  constructor(send) { this.id = 0; this.wait = new Map(); this.send = send; this.events = []; }
  onMessage(txt) {
    const m = JSON.parse(txt);
    if (m.id && this.wait.has(m.id)) { const { res, rej } = this.wait.get(m.id); this.wait.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); }
    else this.events.push(m);
  }
  call(method, params = {}, sessionId) {
    const id = ++this.id;
    const msg = { id, method, params }; if (sessionId) msg.sessionId = sessionId;
    return new Promise((res, rej) => { this.wait.set(id, { res, rej }); this.send(JSON.stringify(msg)); setTimeout(() => { if (this.wait.has(id)) { this.wait.delete(id); rej(new Error("timeout " + method)); } }, 15000); });
  }
}

async function viaPipe() {
  log("[pipe] launching", NOTION);
  const child = spawn(NOTION, ["--remote-debugging-pipe"], { stdio: ["ignore", "ignore", "ignore", "pipe", "pipe"], detached: true });
  let buf = "";
  const cdp = new Cdp((s) => child.stdio[3].write(s + "\0"));
  child.stdio[4].on("data", (d) => { buf += d.toString("utf8"); let i; while ((i = buf.indexOf("\0")) >= 0) { const m = buf.slice(0, i); buf = buf.slice(i + 1); cdp.onMessage(m); } });
  child.on("exit", (c) => log("[pipe] Notion exited", c));
  return cdp;
}

async function viaPort(port = 9339) {
  log("[port] launching", NOTION, "port", port);
  spawn(NOTION, ["--remote-debugging-port=" + port], { stdio: "ignore", detached: true }).unref();
  let ver;
  for (let i = 0; i < 40 && !ver; i++) { await sleep(500); try { ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch {} }
  if (!ver) throw new Error("port did not open");
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const cdp = new Cdp((s) => ws.send(s));
  ws.onmessage = (e) => cdp.onMessage(e.data);
  return cdp;
}

const DUMP = String.raw`(() => {
  const pick = (el) => { const cs = getComputedStyle(el); return {
    tag: el.tagName, cls: el.className && el.className.baseVal === undefined ? String(el.className) : "", 
    style: (el.getAttribute("style")||"").slice(0, 600),
    w: Math.round(el.getBoundingClientRect().width), x: Math.round(el.getBoundingClientRect().left),
    pl: cs.paddingLeft, pr: cs.paddingRight, ml: cs.marginLeft, mr: cs.marginRight, maxw: cs.maxWidth,
    display: cs.display, gtc: cs.gridTemplateColumns, gcol: cs.gridColumn, ctype: cs.containerType, cname: cs.containerName }; };
  const pc = document.querySelector(".notion-page-content");
  const chain = []; for (let e = pc; e && e !== document.body; e = e.parentElement) chain.push(pick(e));
  const kids = pc ? [...pc.children].slice(0, 8).map(pick) : [];
  const sib = []; const fr = document.querySelector(".notion-frame");
  if (fr) fr.querySelectorAll(".layout, .layout-content, .layout-full, .layout-wide, [class*='layout']").forEach((e, i) => { if (i < 25) sib.push(pick(e)); });
  const rules = []; const want = /layout|notion-page-content|full-width|margin-width|content-width|page-width|--margin|--content/;
  const walk = (list, media) => { for (const r of list) { try {
    if (r.cssRules && !r.selectorText) walk(r.cssRules, (r.conditionText || r.media && r.media.mediaText || "") );
    else if (r.selectorText && (want.test(r.selectorText) || want.test(r.style.cssText))) rules.push({ media, sel: r.selectorText.slice(0, 300), css: r.style.cssText.slice(0, 500) });
  } catch {} } };
  for (const s of document.styleSheets) { try { walk(s.cssRules, ""); } catch { rules.push({ sheet: s.href, blocked: true }); } }
  const ed = [...document.querySelectorAll("[contenteditable=true]")].slice(0, 12).map((e) => ({ cls: String(e.className).slice(0,200), leaf: e.getAttribute("data-content-editable-leaf"), caret: getComputedStyle(e).caretColor, ph: !!e.getAttribute("placeholder"), parentCls: String(e.parentElement && e.parentElement.className).slice(0,200) }));
  return { url: location.origin + location.pathname.replace(/[^/]+$/, "<page>"), iw: innerWidth, ih: innerHeight, dpr: devicePixelRatio,
    htmlCls: document.documentElement.className, bodyCls: document.body.className, chain, kids, layoutEls: sib, rules: rules.slice(0, 300), editables: ed };
})()`;

async function main() {
  if (notionRunning()) { log("Notion is running: please quit it fully (tray icon > Quit) and re-run."); process.exit(2); }
  const mode = process.argv[2] || "pipe";
  let cdp;
  if (mode === "pipe") {
    cdp = await viaPipe();
    try { const v = await cdp.call("Browser.getVersion"); log("[pipe] OK", JSON.stringify(v)); writeFileSync(join(OUT, "pipe-ok.txt"), JSON.stringify(v)); }
    catch (e) { log("[pipe] FAILED:", e.message); writeFileSync(join(OUT, "pipe-failed.txt"), e.message); log("Quit Notion, then run: node recon.mjs port"); process.exit(3); }
  } else cdp = await viaPort();

  let pages = [];
  for (let i = 0; i < 60; i++) {
    const { targetInfos } = await cdp.call("Target.getTargets");
    writeFileSync(join(OUT, "targets.json"), JSON.stringify(targetInfos.map((t) => ({ type: t.type, url: t.url.replace(/(notion\.(so|com)\/).*/, "$1…"), title: t.type === "page" && /notion\.(so|com)/.test(t.url) ? "<page>" : t.title })), null, 1));
    pages = targetInfos.filter((t) => t.type === "page" && /^https:\/\/(app\.notion\.com|www\.notion\.so)\//.test(t.url));
    if (pages.length) break; await sleep(1000);
  }
  log("notion page targets:", pages.length);
  await sleep(6000); // let the page render
  let n = 0;
  for (const t of pages) {
    const { sessionId } = await cdp.call("Target.attachToTarget", { targetId: t.targetId, flatten: true });
    const r = await cdp.call("Runtime.evaluate", { expression: DUMP, returnByValue: true }, sessionId);
    writeFileSync(join(OUT, `layout-${n}.json`), JSON.stringify(r.result.value, null, 1));
    try { const shot = await cdp.call("Page.captureScreenshot", { format: "png" }, sessionId); writeFileSync(join(OUT, `shot-${n}.png`), Buffer.from(shot.data, "base64")); } catch (e) { log("shot failed", e.message); }
    n++;
  }
  log("wrote", n, "dumps to", OUT, "- Notion stays open; this script exits now.");
  process.exit(0);
}
main().catch((e) => { console.error(e); process.exit(1); });
