/* Shared by the content script, native settings popup and offline checks. */
(() => {
  const defaults = Object.freeze({ enabled: true, responsive: true, narrowAt: 1100, gutter: 24, smoothCaret: true, caretDuration: 100, touchpadZoom: true });
  const number = (value, fallback, low, high) => typeof value === 'number' && Number.isFinite(value) ? Math.min(high, Math.max(low, value)) : fallback;
  function normalize(value = {}) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) value = {};
    return {
      enabled: typeof value.enabled === 'boolean' ? value.enabled : defaults.enabled,
      responsive: typeof value.responsive === 'boolean' ? value.responsive : defaults.responsive,
      narrowAt: number(value.narrowAt, defaults.narrowAt, 640, 1600),
      gutter: number(value.gutter, defaults.gutter, 16, 64),
      smoothCaret: typeof value.smoothCaret === 'boolean' ? value.smoothCaret : defaults.smoothCaret,
      caretDuration: number(value.caretDuration, defaults.caretDuration, 40, 180),
      touchpadZoom: typeof value.touchpadZoom === 'boolean' ? value.touchpadZoom : defaults.touchpadZoom,
    };
  }
  function compact(width, config) { return config.enabled && config.responsive && width > 0 && width < config.narrowAt; }
  // One compact margin serves both sides of a page; very narrow panes use at most 20 px.
  function gutter(width, config) { return width < 520 ? Math.min(config.gutter, 20) : config.gutter; }
  globalThis.NotionKexPolicy = Object.freeze({ defaults, normalize, compact, gutter, storageKey: 'notion-kex.settings' });
})();
