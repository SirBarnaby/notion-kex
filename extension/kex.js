(() => {
  'use strict';
  const policy = globalThis.NotionKexPolicy;
  const api = globalThis.chrome;
  const version = '1.1.2';
  let settings = policy.normalize();
  let disposed = false;
  let scanFrame = 0;
  let caretFrame = 0;
  let caretTimer = 0;
  let composing = false;
  let pointerDown = false;
  let dragging = false;
  let snap = true;
  let caret;
  let suspended = true;
  let pointerEditor = false;
  let caretActivityAt = performance.now();
  // A caret unchanged for settleMs is rechecked every idleCheckMs instead of every frame. Events and edits inside
  // its editor still request an immediate check, and any change returns it to per-frame checks.
  const settleMs = 300, idleCheckMs = 250;
  const layouts = new Map(); // layout -> { frame, recognized }
  const sidebars = new Map();
  const removers = [];
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  const highContrast = matchMedia('(forced-colors: active)');
  const listen = (target, type, callback, options) => {
    target.addEventListener(type, callback, options);
    removers.push(() => target.removeEventListener(type, callback, options));
  };
  // Only real changes are written, so repeated scans never queue attribute mutations or style invalidation.
  const setData = (element, name, value) => {
    if (value === undefined) delete element.dataset[name];
    else if (element.dataset[name] !== value) element.dataset[name] = value;
  };
  const mark = (name, value) => { if (document.documentElement) setData(document.documentElement, name, value); };
  const transparent = (color) => color === 'transparent' || color === 'rgba(0, 0, 0, 0)';
  const paintColor = (anchor, caretColor) => caretColor === 'auto' || transparent(caretColor) ? getComputedStyle(anchor).color : caretColor;

  function updateSidebar(element) {
    // Native previews keep the nav's target width at zero; button/shortcut opening expands it.
    // Read the inline target, not the animated width, so normal opening is visible immediately.
    setData(element, 'notionKexSidebarCollapsed', settings.enabled && element.style.width === '0px' && element.querySelector('.notion-sidebar') ? 'true' : undefined);
  }

  function clearFrame(frame) {
    setData(frame, 'notionKexCompact', undefined);
    frame.style.removeProperty('--notion-kex-gutter');
  }
  function updateLayout(layout) {
    const entry = layouts.get(layout);
    if (!entry) return;
    if (!layout.isConnected) { layouts.delete(layout); resize.unobserve(layout); clearFrame(entry.frame); return; }
    const width = layout.getBoundingClientRect().width;
    // A future Notion layout gets its native rendering until a matching adapter exists.
    entry.recognized = getComputedStyle(layout).gridTemplateColumns.includes('[content-start]');
    if (entry.recognized && policy.compact(width, settings)) {
      setData(entry.frame, 'notionKexCompact', 'true');
      entry.frame.style.setProperty('--notion-kex-gutter', `${policy.gutter(width, settings)}px`);
    } else clearFrame(entry.frame);
  }
  function markLayout() {
    let matched = false, pending = !layouts.size;
    for (const { recognized } of layouts.values()) { matched ||= recognized === true; pending ||= recognized === undefined; }
    mark('notionKexLayout', matched ? 'matched' : pending ? 'waiting' : 'unsupported');
  }
  // ResizeObserver reports each new layout's first size and later changes after layout, so DOM churn never forces a layout read.
  const resize = new ResizeObserver((entries) => { entries.forEach(({ target }) => updateLayout(target)); markLayout(); scheduleCaret(true); });
  // Observing again makes ResizeObserver report every layout once more, for example after a settings change.
  const refreshLayouts = () => { for (const layout of layouts.keys()) { resize.unobserve(layout); resize.observe(layout); } };
  // Removed layouts and sidebars are released at once; this touches only the few tracked elements.
  function prune() {
    for (const layout of layouts.keys()) if (!layout.isConnected) updateLayout(layout);
    for (const [element, observer] of sidebars) {
      if (element.isConnected) updateSidebar(element);
      else { observer.disconnect(); sidebars.delete(element); setData(element, 'notionKexSidebarCollapsed', undefined); }
    }
    markLayout();
  }
  // Discovery queries the whole document, so it runs only when a page, a sidebar or its panel is inserted.
  const watched = '.notion-page-content, nav.notion-sidebar-container, .notion-sidebar';
  function scan() {
    scanFrame = 0;
    if (disposed) return;
    for (const element of document.querySelectorAll('nav.notion-sidebar-container')) {
      if (!sidebars.has(element)) {
        const observer = new MutationObserver(() => updateSidebar(element));
        observer.observe(element, { attributes: true, attributeFilter: ['style'] });
        sidebars.set(element, observer);
      }
      updateSidebar(element);
    }
    for (const page of document.querySelectorAll('.notion-frame .notion-page-content')) {
      const layout = page.closest('.layout');
      const frame = layout?.closest('.notion-frame');
      if (layout && frame && !layouts.has(layout)) { layouts.set(layout, { frame, recognized: undefined }); resize.observe(layout); }
    }
    mark('notionKexVersion', version);
    markLayout();
  }
  function scheduleScan() { if (!disposed && !scanFrame) scanFrame = requestAnimationFrame(scan); }

  function hideCaret(reason = 'native') {
    cancelAnimationFrame(caretFrame); caretFrame = 0;
    clearTimeout(caretTimer);
    if (caret) {
      caret.styles?.disconnect();
      caret.edits?.disconnect();
      for (const animation of [caret.paint, caret.motion, caret.blink, ...caret.suppression]) animation?.cancel();
      setData(caret.frame, 'notionKexCaretActive', undefined);
      caret = undefined;
    }
    mark('notionKexCaret', reason);
  }
  function suspendCaret() { suspended = true; hideCaret(); }
  function styleIdentity(element) {
    return JSON.stringify([element.getAttribute('class'), [...element.style].filter(name => !name.startsWith('--notion-kex-')).map(name => [name, element.style.getPropertyValue(name), element.style.getPropertyPriority(name)])]);
  }

  function caretRect(range) {
    let rect = [...range.getClientRects()].find((r) => r.height > 0);
    if (rect) return { left: rect.left, top: rect.top, height: rect.height };
    // Browsers can omit the collapsed rect at a text node boundary. Measure a neighbour without changing selection.
    const node = range.startContainer;
    const offset = range.startOffset;
    if (node.nodeType !== Node.TEXT_NODE || !node.length) return undefined;
    const probe = range.cloneRange();
    const backwards = offset > 0;
    if (backwards) { probe.setStart(node, offset - (offset > 1 && /[\uDC00-\uDFFF]/.test(node.data[offset - 1]) ? 2 : 1)); probe.setEnd(node, offset); }
    else { probe.setStart(node, 0); probe.setEnd(node, node.data.codePointAt(0) > 0xffff ? 2 : 1); }
    const rects = [...probe.getClientRects()];
    rect = backwards ? rects.at(-1) : rects[0];
    if (!rect?.height) return undefined;
    const rtl = getComputedStyle(node.parentElement).direction === 'rtl';
    return { left: backwards !== rtl ? rect.right : rect.left, top: rect.top, height: rect.height };
  }

  function drawCaret() {
    caretFrame = 0;
    if (disposed || suspended || !settings.enabled || !settings.smoothCaret || pointerDown || dragging || composing || reduced.matches || highContrast.matches || document.hidden || !document.hasFocus()) return hideCaret(composing ? 'composing' : reduced.matches ? 'reduced' : 'native');
    const selection = getSelection();
    if (!selection?.isCollapsed || selection.rangeCount !== 1) return hideCaret();
    const range = selection.getRangeAt(0);
    const anchor = range.startContainer.nodeType === Node.ELEMENT_NODE ? range.startContainer : range.startContainer.parentElement;
    const host = anchor?.closest('[contenteditable="true"], [contenteditable="plaintext-only"]');
    const active = document.activeElement;
    if (!anchor?.isContentEditable || !host?.closest('.notion-frame .layout') || anchor.closest('[role="dialog"], [aria-hidden="true"], [inert]') || !(host === active || host.contains(active) || active?.isContentEditable && active.contains(host))) return hideCaret();
    if (!anchor.checkVisibility?.({ opacityProperty: true, visibilityProperty: true, contentVisibilityAuto: true })) return hideCaret();
    for (let element = anchor; element; element = element.parentElement) {
      const style = getComputedStyle(element), matrix = new DOMMatrixReadOnly(style.transform === 'none' ? undefined : style.transform);
      if (!matrix.is2D || matrix.a <= 0 || matrix.d <= 0 || Math.abs(matrix.b) > .000001 || Math.abs(matrix.c) > .000001 || style.rotate !== 'none' && style.rotate !== '0deg' || style.perspective !== 'none' || style.scale.split(' ').some(value => Number(value) < 0)) return hideCaret();
    }
    let rect;
    try { rect = caretRect(range); } catch { return hideCaret(); }
    if (!rect || ![rect.left, rect.top, rect.height].every(Number.isFinite) || rect.height < 1) return hideCaret();
    const bounds = host.getBoundingClientRect();
    if (!host.offsetWidth || !host.offsetHeight || !bounds.width || !bounds.height) return hideCaret();
    const scaleX = bounds.width / host.offsetWidth, scaleY = bounds.height / host.offsetHeight;
    rect = { left: (rect.left - bounds.left) / scaleX, top: (rect.top - bounds.top) / scaleY, height: rect.height / scaleY };
    if (rect.left < 0 || rect.left > host.offsetWidth || rect.top < 0 || rect.top + rect.height > host.offsetHeight + 1) return hideCaret();
    rect.left += host.scrollLeft; rect.top += host.scrollTop;
    if (caret && (caret.host !== host || caret.anchor !== anchor || caret.active !== active)) hideCaret();
    for (let element = anchor; element !== host; element = element.parentElement) {
      const style = getComputedStyle(element);
      if (style.backgroundImage !== 'none' || !transparent(style.backgroundColor)) return hideCaret();
    }
    if (caret && caret.backgroundImage !== host.style.backgroundImage) return hideCaret();
    if (!caret) {
      // Background paint is clipped and owned by the editable element; no node or editor attribute is added.
      if (getComputedStyle(host).backgroundImage !== 'none' || !host.animate) return hideCaret();
      const color = getComputedStyle(anchor).caretColor;
      if (transparent(color)) return hideCaret();
      caret = { host, anchor, active, frame: host.closest('.notion-frame'), suppression: [], backgroundImage: host.style.backgroundImage, color: paintColor(anchor, color) };
      try {
        // Only edits inside its own editor can move a caret painted in that editor's background.
        caret.edits = new MutationObserver(() => scheduleCaret());
        caret.edits.observe(host, { childList: true, characterData: true, subtree: true });
        const styles = new Map();
        caret.styles = new MutationObserver(() => { if ([...styles].some(([element, identity]) => styleIdentity(element) !== identity)) suspendCaret(); });
        for (let element = anchor; element; element = element.parentElement) {
          styles.set(element, styleIdentity(element));
          caret.styles.observe(element, { attributes: true, attributeFilter: ['class', 'style'] });
        }
        for (const element of new Set([host, anchor, active])) {
          const animation = element.animate([{ caretColor: 'transparent' }], { duration: 1, fill: 'both' });
          caret.suppression.push(animation); animation.finish();
        }
      } catch { return hideCaret(); }
      snap = true;
    }
    if (![host, anchor, active].every((element) => transparent(getComputedStyle(element).caretColor))) return hideCaret();
    const previous = caret.previous;
    const moved = !previous || previous.node !== range.startContainer || previous.offset !== range.startOffset;
    const geometryChanged = !previous || previous.left !== rect.left || previous.top !== rect.top || previous.height !== rect.height;
    const repaint = moved || geometryChanged || snap;
    try {
      if (repaint) {
        let colorChanged = false;
        if (caret.paint) {
          caret.paint.effect.target = null;
          const suppression = caret.suppression.map(animation => [animation.effect, animation.effect.target]);
          for (const [effect] of suppression) effect.target = null;
          const background = getComputedStyle(host).backgroundImage;
          const nativeColor = getComputedStyle(anchor).caretColor;
          const color = paintColor(anchor, nativeColor);
          for (const [effect, target] of suppression) effect.target = target;
          caret.paint.effect.target = host;
          if (background !== 'none') return hideCaret();
          if (transparent(nativeColor)) return hideCaret();
          colorChanged = color !== caret.color; caret.color = color;
        }
        if (!caret.paint || previous.height !== rect.height || colorChanged) {
          caret.paint?.cancel();
          caret.paint = host.animate([{ backgroundImage: `linear-gradient(${caret.color}, ${caret.color})`, backgroundRepeat: 'no-repeat', backgroundAttachment: 'local', backgroundOrigin: 'border-box', backgroundClip: 'border-box', backgroundSize: `2px ${rect.height}px` }], { duration: 1, fill: 'both' });
          caret.paint.finish();
        }
        const target = `${rect.left}px ${rect.top}px`;
        const start = caret.motion ? getComputedStyle(host).backgroundPosition : target;
        caret.motion?.cancel();
        const jump = !previous || Math.abs(previous.top - rect.top) > rect.height * 1.6;
        caret.motion = host.animate([{ backgroundPosition: start }, { backgroundPosition: target }], { duration: snap || jump || !moved ? 0 : settings.caretDuration, easing: 'cubic-bezier(.2,.8,.2,1)', fill: 'both' });
        if (moved || !previous || previous.height !== rect.height) armBlink(caret, rect.height);
        caret.previous = { ...rect, node: range.startContainer, offset: range.startOffset }; snap = false;
        caret.changedAt = performance.now();
      }
    } catch { return hideCaret(); }
    setData(caret.frame, 'notionKexCaretActive', 'true');
    const own = new Set([caret.paint, caret.motion, caret.blink, ...caret.suppression]);
    const foreignBackground = host.getAnimations().some(animation => !own.has(animation) && !animation.effect.pseudoElement && animation.effect.getKeyframes().some(frame => Object.keys(frame).some(name => name.startsWith('background'))));
    if (caret.paint.playState === 'idle' || foreignBackground || getComputedStyle(host).backgroundImage !== `linear-gradient(${caret.color}, ${caret.color})`) {
      return hideCaret();
    }
    mark('notionKexCaret', 'smooth');
    if (performance.now() - caret.changedAt < settleMs) scheduleCaret();
    else caretTimer = setTimeout(scheduleCaret, idleCheckMs);
  }
  // Each blink phase is one zero-length effect that waits in its delay. Blink wakes once per phase instead of
  // servicing an endless animation every frame. The phase follows the last real editing activity.
  const blinkRestMs = 650, blinkOnMs = 539, blinkCycleMs = 1100;
  function armBlink(state, height) {
    const elapsed = performance.now() - caretActivityAt;
    const cycle = (elapsed - blinkRestMs) % blinkCycleMs;
    const visible = elapsed < blinkRestMs || cycle < blinkOnMs;
    const wait = elapsed < blinkRestMs ? blinkRestMs + blinkOnMs - elapsed : (visible ? blinkOnMs : blinkCycleMs) - cycle;
    const on = `2px ${height}px`, off = `0px ${height}px`;
    state.blink?.cancel();
    const blink = state.host.animate([{ backgroundSize: visible ? on : off }, { backgroundSize: visible ? off : on }], { id: 'notion-kex-blink', duration: 0, delay: Math.max(1, wait), fill: 'both' });
    blink.onfinish = () => { if (caret === state && state.blink === blink) armBlink(state, height); };
    state.blink = blink;
  }
  function scheduleCaret(instant = false) {
    snap ||= instant;
    if (disposed || caretFrame) return;
    clearTimeout(caretTimer);
    caretFrame = requestAnimationFrame(drawCaret);
  }
  function configure(value) {
    settings = policy.normalize(value); scheduleScan(); refreshLayouts(); hideCaret(); scheduleCaret(true);
    mark('notionKexPinch', settings.enabled && settings.touchpadZoom ? 'waiting' : 'disabled');
    if (location.protocol === 'https:') window.postMessage({ type: 'notion-kex:pinch', version: 1, enabled: settings.enabled && settings.touchpadZoom }, location.origin);
  }
  listen(window, 'message', event => {
    const data = event.data;
    if (event.source === window && event.origin === location.origin && data?.type === 'notion-kex:pinch-state' && data.version === 1) {
      if (data.available === false) mark('notionKexPinch', 'unavailable');
      else if (typeof data.enabled === 'boolean') mark('notionKexPinch', data.enabled ? 'native' : 'disabled');
    }
  });
  const changed = (changes, area) => { if (area === 'local' && changes[policy.storageKey]) configure(changes[policy.storageKey].newValue); };
  try {
    api.storage.local.get(policy.storageKey, (result) => { if (!disposed && !api.runtime.lastError) configure(result[policy.storageKey]); });
    api.storage.onChanged.addListener(changed);
  } catch { /* Default settings work when an older host lacks storage. */ }
  const observer = new MutationObserver((entries) => {
    let removed = false, added = false;
    for (const entry of entries) {
      for (const node of entry.removedNodes) {
        removed = true;
        // Removing or reparenting the caret's editor ends its editing session.
        if (caret && (node === caret.host || node.contains(caret.host))) suspendCaret();
      }
      if (!added) for (const node of entry.addedNodes) if (node.nodeType === Node.ELEMENT_NODE && (node.matches(watched) || node.querySelector(watched))) { added = true; break; }
    }
    if (caret && !caret.host.isConnected) suspendCaret();
    if (removed) prune();
    if (added) scheduleScan();
  });
  observer.observe(document, { childList: true, subtree: true });
  for (const event of ['selectionchange', 'keyup', 'focusin']) listen(document, event, () => scheduleCaret());
  for (const event of ['input', 'keydown']) listen(document, event, (event) => {
    if (event.isTrusted && event.target.isContentEditable && !['Shift', 'Control', 'Alt', 'Meta'].includes(event.key)) { suspended = false; caretActivityAt = performance.now(); }
    scheduleCaret();
  }, true);
  listen(document, 'pointerdown', (event) => { pointerDown = true; pointerEditor = event.isTrusted && !!event.target.closest('[data-content-editable-leaf="true"]'); suspendCaret(); }, true);
  listen(document, 'pointerup', (event) => { pointerDown = false; if (event.isTrusted && pointerEditor && !dragging) { suspended = false; caretActivityAt = performance.now(); } pointerEditor = false; scheduleCaret(true); }, true);
  listen(document, 'pointercancel', () => { pointerDown = false; pointerEditor = false; scheduleCaret(true); }, true);
  listen(document, 'dragstart', () => { dragging = true; suspendCaret(); }, true);
  listen(document, 'dragend', () => { dragging = false; pointerDown = false; pointerEditor = false; suspendCaret(); }, true);
  listen(document, 'scroll', () => scheduleCaret(true), { capture: true, passive: true });
  listen(window, 'resize', () => scheduleCaret(true), { passive: true });
  listen(document, 'compositionstart', () => { composing = true; hideCaret('composing'); }, true);
  listen(document, 'compositionend', () => { composing = false; scheduleCaret(true); }, true);
  listen(window, 'blur', () => { dragging = false; pointerDown = false; pointerEditor = false; suspendCaret(); });
  listen(window, 'focus', () => scheduleCaret(true));
  listen(document, 'focusout', () => { hideCaret(); scheduleCaret(true); });
  listen(document, 'visibilitychange', () => document.hidden ? hideCaret() : scheduleCaret(true));
  listen(reduced, 'change', () => { hideCaret(); scheduleCaret(true); });
  listen(highContrast, 'change', () => { hideCaret(); scheduleCaret(true); });
  listen(window, 'pageshow', () => { scheduleScan(); refreshLayouts(); scheduleCaret(true); });
  listen(window, 'pagehide', (event) => {
    if (event.persisted) { hideCaret(); return; }
    disposed = true; observer.disconnect(); resize.disconnect();
    cancelAnimationFrame(scanFrame); hideCaret();
    layouts.forEach(({ frame }) => clearFrame(frame));
    sidebars.forEach((observer, element) => { observer.disconnect(); setData(element, 'notionKexSidebarCollapsed', undefined); });
    sidebars.clear();
    removers.forEach((remove) => remove());
    try { api.storage.onChanged.removeListener(changed); } catch { /* Host already closed. */ }
  }, { once: true });
  scheduleScan();
})();
