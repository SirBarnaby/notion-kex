# Notion KEX: approach and evidence

Research date: 20 October 2026.

### 6 November: an idle caret no longer keeps the page busy (1.1.2)

A SonarQube Community scan (Sonar way profile, local server) of `extension/`, `native/` and `tools/` reported one bug, a promise chain in the pinch preload without a rejection path, and no vulnerabilities, hotspots, code smells or duplication. SonarQube Community does not analyze PowerShell, so the scripts were checked with PSScriptAnalyzer 1.24; it reported naming and `Write-Host` style warnings only.

Profiling the content script in a headless Chromium fixture with 400 blocks showed the remaining cost was idle time. While a smooth caret sat still, KEX revalidated it on every animation frame and its endless main-thread blink animation was serviced every frame too. Chromium services an active main-thread animation each frame even when its value does not change, but a pending `delay` is woken by a timer.

Version 1.1.2 keeps the same checks but stops repeating them when nothing changes. An unchanged caret is checked per frame for 300 ms, then every 250 ms. Events, edits inside its own editor (a per-caret MutationObserver) and its layout's ResizeObserver still request an immediate check, and any change returns to per-frame checks. Each blink phase is one zero-length Web Animation that waits in its delay, so the page wakes about twice per blink cycle. Phase timing still follows the last trusted editing activity. Layout discovery runs only when a page or sidebar is inserted, and layout sizes come from ResizeObserver after layout instead of forced reads on every DOM change. The redundant start gutter left from the removed control lane is gone; both margins use one value as before.

Measured in the fixture (median of three runs, Chromium 153): idle main-thread work fell from 24.2 to 6.7 ms per second, KEX frame requests from 60 to 4 per second, and style recalculations from 60 to 17 per second. Typing cost is unchanged.

### 28 October: startup pinch state survives preference refreshes (1.1.1)

The user reported that native pinch initially failed until the setting was toggled. The existing-tools search and upstream investigation were repeated. No suitable maintained desktop replacement was established; the relevant upstream change is already in the installed Electron 44.9.1 API. KEX needs to call that API at the correct ownership level.

Version 1.1.0 used `webFrame.setVisualZoomLevelLimits`, then cached a successful `applied` flag. [Electron's renderer implementation](https://raw.githubusercontent.com/electron/electron/v44.9.1/shell/renderer/api/electron_api_web_frame.cc) changes only the current Blink WebView defaults. [Chromium's WebView preference application](https://raw.githubusercontent.com/chromium/chromium/152.0.7977.167/third_party/blink/renderer/core/exported/web_view_impl.cc) reapplies the browser's stored default page-scale limits. KEX's renderer-only call had left those stored limits at 1–1. An identical startup setting then echoed the stale success flag; off/on forced a fresh renderer call and recovered.

The isolated fixture reproduces the failure deterministically: load and attach a hidden WebContentsView, observe KEX's initial enabled acknowledgement, trigger a normal preference refresh through `setImageAnimationPolicy`, then attempt the first pinch without sending a setting message. Version 1.1.0 stays at scale 1. A restrictive viewport meta tag and a restored non-default layout zoom did not explain the failure; they were not adopted as causes. The precise preference-refresh event on the user's signed-in page remains unobserved.

Version 1.1.1 sends a fixed, typed request from the guarded preload to KEX's main payload. The handler validates version, booleans, the sender's main frame and the top-level Notion HTTPS origin, then calls the public [webContents.setVisualZoomLevelLimits API](https://raw.githubusercontent.com/electron/electron/v44.9.1/lib/browser/api/web-contents.ts). That API [stores the range in WebContentsPreferences](https://raw.githubusercontent.com/electron/electron/v44.9.1/shell/browser/api/electron_api_web_contents.cc) and updates the current renderer, so subsequent preference application retains it. No fixed startup delay, polling loop or repeated gesture interception is introduced. The same path handles disable and reset in the existing serialized preload queue.

The regression now passes, and later preference refreshes retain both pinch availability and an existing user pinch level. Tests wait for the native acknowledgement rather than the extension's optimistic disabled marker. This change affects only zoom ownership and release identities; caret rendering, notes, layout policy, permissions and the existing archive backup/restoration machinery are retained.

### 28 October: working blink phases and native touchpad zoom (1.1.0)

The tools search was repeated before implementation across GitHub, Reddit, Windhawk, Nexus and Notion extension listings. No maintained native Notion 7.41.0 solution for pointer-centred touchpad pinch was established. [Notion-enhancer](https://notion-enhancer.github.io/) retains its compatibility warning, and browser-only Notion extensions do not establish support inside this desktop build. KEX is extended rather than adding another app wrapper or gesture-to-keyboard utility.

The blink failure was reproducible without the user's page. Effect-level `step-end` easing held the entire animation at its visible start, so the hidden keyframes were never sampled. Linear effect timing with a step on the visible-to-hidden segment reaches both phases. Replacing the same text node every 120 ms exposed a separate starvation bug: each new node restarted the 650 ms idle delay. The blink phase now comes from the time since actual trusted editing activity, retaining it across rerenders.

[Electron's visual zoom API](https://www.electronjs.org/docs/latest/api/web-frame#webframesetvisualzoomlevellimitsminimumlevel-maximumlevel) enables Chromium's native pinch viewport. The extension's isolated world cannot call this privileged API, so a small payload is appended to Notion's existing sandboxed page preload. The copied-runtime fixture demonstrated that setting limits during early preload was reset by navigation; waiting for DOMContentLoaded makes pinch effective. Limits are 1–5. A strictly typed, same-window/same-origin message carries only the extension's enable preference; no privileged API is exposed to page code.

Notion's main bundle supplies a custom Ctrl+0 menu handler that resets its layout zoom. The preload cannot reliably observe a key consumed by that accelerator. A second small main-entry payload listens to [before-input-event](https://www.electronjs.org/docs/latest/api/web-contents#event-before-input-event), validates the top-level Notion HTTPS URL, and sends one fixed IPC reset message to that main frame. It does not prevent the key event or change the existing menu. The preload clamps visual scale to 1 and then restores the enabled range. Composition, repeats, extra modifiers and other keys are excluded.

The installed 7.41.0 EXE already has embedded ASAR integrity validation disabled; all executable bytes and fuse settings are preserved. REA fuse evidence: `ev_f836ad6c0445e0c3a96a95e58fd2eb699d8f4185d59a62b0e046f4809e52d66f`. Selective page-preload analysis: `ev_f58e90d4c3affff8b582dd6f3866cc743229e2028688189f2bfecfb2288fa467`, summary `ev_4eaabbf7f08cfd761bb68b6ca90d6e7e507c4f57bd8c4478ae6a42ca1075a3e9`. This is bounded reconnaissance, not whole-program coverage. Native-module integrity observations were retained; no unpacked module is changed.

The installer validates contracts and package hashes, preserves every unrelated packed member and all existing unpacked metadata, and replaces the archive only while the target Notion installation is closed. A flushed pending journal allows recovery on either side of an interrupted archive/record commit. Unknown app changes retain their recovery files. Each app version gets its own verified original backup, and removal never replaces a newer Notion version with an older backup. Native pinch requires reapplication after an app update; the extension remains in the profile. No proprietary Notion bytes or profiles are distributed.

### 28 October redesign: text-owned gliding caret (1.0.6)

The user reported a detached cursor after typing in a block and then moving that block, and explicitly required retaining gliding movement. The earlier viewport overlay was architecturally independent of its text. More event coverage cannot make its position and lifetime belong to that text. The exact signed-in Notion event sequence was not captured; the redesign addresses that independent-ownership failure directly.

The existing-tools search was repeated before implementation. [Notion-enhancer](https://notion-enhancer.github.io/) retains its compatibility warning. [SmoothType](https://github.com/jsz-05/SmoothType) includes a `notion.js`, but its [published source](https://raw.githubusercontent.com/jsz-05/SmoothType/main/notion.js) consists of commented-out code using hypothetical selectors; it does not establish a working maintained Notion 7.41.0 implementation. [Smooth-cursorify](https://github.com/gwennlbh/smooth-cursorify) styles custom carets in other editors rather than supplying this native desktop behavior. GitHub, Reddit, Windhawk, Nexus and extension searches did not identify a suitable replacement. [Chromium's native caret-animation support](https://developer.chrome.com/release-notes/140#css_caret-animation_property) can customize blinking, but does not expose gliding position. Native-only fading was considered; the user required preserving glide.

KEX now uses Web Animations on the selected editable element's own background. Its gradient is a narrow caret, its background position interpolates during typing, and its size supplies blinking. `background-clip: border-box` bounds the paint to its owner; `background-attachment: local` carries it with the owner's own scrolling. There is no detached DOM element, fixed position, text insertion, pseudo-element placeholder replacement, CSS selector that can retarget a clone, or style/class marker inside the editor. Web Animation effects target a particular element object and are not copied by `cloneNode`. Canceling the effects restores the application's underlying styles.

Owner removal/reparenting and drag operations revoke the editing session; dragend and selectionchange cannot alone rearm it. A fresh trusted text click, keystroke or input can resume smoothing. Ancestor class/style observation invalidates stale style ownership, excluding only KEX's own layout variables. Application backgrounds and competing animations retain native caret rendering. Repaints temporarily detach KEX effects synchronously to read underlying application background/color, then restore them or fall back. Non-axis-aligned or reflected transforms anywhere between the selected anchor and document root also use native rendering.

The key regression test freezes all requestAnimationFrame tracking and disconnects MutationObservers before moving the selected block, then checks actual caret pixels. Additional pixel checks cover clipping after an owner resize, removal, own scrolling and actual mid-glide rendering; clone checks verify the absence of copied effects. This distinguishes browser-owned paint lifetime from a successful software position refresh. See `verification.md` for results and limits.

### 28 October follow-up: stale caret lifecycle (1.0.5)

The existing-tools search was repeated before changing code. [Notion-enhancer's official site](https://notion-enhancer.github.io/) still warns that recent Notion changes impaired its functioning; desktop 7.41.0 compatibility for this request is not established. [Smooth-cursorify](https://github.com/gwennlbh/smooth-cursorify) documents custom-cursor styling for other editors, does not list Notion as supported, and does not establish a maintained compatible fix for this desktop extension. Searches across GitHub, Reddit, Windhawk and Nexus did not identify a suitable native replacement for the requested behavior. KEX is therefore extended rather than replaced.

The previous overlay lifetime depended on input/focus/selection/scroll/resize events. A CSS transform moves a text block without resizing its page grid; hiding or restyling the selected leaf can also leave those events silent. Four isolated checks reproduce the resulting stale overlay or duplicate caret on 1.0.4. Version 1.0.5 revalidates only while the overlay is visible and stops validation when it falls back to the native caret. [Element.checkVisibility](https://developer.mozilla.org/en-US/docs/Web/API/Element/checkVisibility) supplies rendered-box, visibility, opacity and skipped-content checks; selection ownership and viewport/pane geometry remain separate checks. Unsupported visibility APIs retain the native caret.

The continuous check must not continuously reset blinking or the movement transition. Selected node/offset and measured geometry identify actual changes; layout movement snaps to its selected text while selection movement uses the configured easing. Native suppression is still checked each frame because a style change can expose the native caret while the selection is idle. No editable-tree markers, editor-state hooks, typing interception or page text are introduced.

Portability remains a native unpacked extension plus `Install.cmd`. First installation on each Windows computer requires enabling the native permission in Notion's Debug menu; the package cannot truthfully be described as fully one click. Later installs reuse that computer's permission. Secure settings and account profiles are not part of the portable package.

### 24 October follow-up: compact margins and explicit sidebar opening (1.0.4)

The user rejected the extra leading control lane. Version 1.0.4 removes its allowance: both margins follow the original compact policy. It suppresses the floating native sidebar preview instead. The single-caret fixes from 1.0.3 remain.

Existing [Notion Sidebar Tamer](https://github.com/cyanxiao/notion-sidebar-tamer) targets this behavior. Its [published 0.5 release](https://chromewebstore.google.com/detail/notion-sidebar-tamer/kkjflddbknmcnjodgeobojmdacilodlb) was last updated in June 2023. Its source hides the sidebar according to a Notion local-storage record. The listing does not establish compatibility with desktop 7.40.0. The older [Notion-no-sidebar userscript](https://github.com/Roznoshchik/Notion-no-sidebar) adds a separate toggle and repositions native page elements. Neither is installed alongside KEX.

KEX follows the existing approach of suppressing a collapsed preview, using the current renderer's DOM contract instead of copying account storage or adding a new toggle. Public login-page JavaScript was inspected in a fresh headless Edge process, without a user profile or signed-in session. In `mainApp-4fc1d8e062f5b745.js`, the native `nav.notion-sidebar-container` animates its inline target width to the sidebar width when expanded, and zero when collapsed. Hover can reveal its `.notion-sidebar` child while the nav's target stays zero. The animation component in `ClientFramework-8ecff10e4ee51215.js` applies the target as an ordinary inline style with a CSS transition.

KEX watches only the recognized nav's inline style and marks a zero-width target outside the editable tree. A scoped selector hides that container and disables its subtree's hit testing, even if a desktop-generated peek sets the child visible. Opening through the existing button or keyboard shortcut gives the nav a positive target width, immediately removing suppression without waiting for the width transition. No mouse or keyboard events are canceled, no private React stores are hooked, and no local-storage records are read or written. Unknown width contracts retain native rendering. Anonymous assets are development evidence under `.scratch/sidebar-recon`, excluded from distribution.

### 23 October follow-up: block-control lane and caret ownership

The leading lane described here was superseded by 1.0.4 at the user's request.

The compact page margin also determines the position of Notion's block controls, which extend outside the text column. KEX now recognizes native sidebar markers and reserves a leading control lane, with extra clearance when the page reaches the screen edge. It changes the page grid, without blocking mouse events or replacing Notion's sidebar/drag behavior. An inset pane still contains its controls, but does not need the screen-edge allowance. The trailing compact margin is retained.

The old caret rule targeted contenteditable elements alone. Explicit caret styling on a formatted descendant could override inheritance, leaving a visible native insertion caret beside KEX's overlay. The revised scoped rule includes descendants, and the renderer checks computed suppression on both the selection and focus owners before showing its caret. If a native style wins, the overlay stays hidden. See the [`caret-color` contract](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/caret-color).

Native HTML drag/drop has its own lifecycle: a successful drag start can cancel the pointer sequence before any pointer-up. KEX tracks `dragstart`/`dragend` separately from pointer selection so the typing overlay stays suspended throughout the move. It does not cancel the native events. See [MDN drag operations](https://developer.mozilla.org/en-US/docs/Web/API/HTML_Drag_and_Drop_API/Drag_operations).

## Existing options

- [Notion Full width](https://www.notion.com/help/customize-and-style-your-content) changes a page setting. It does not provide responsive per-pane margins or smooth caret motion. The captured full-width page still had 96 px gutters.
- [Notion-enhancer](https://notion-enhancer.github.io/) is an existing desktop/browser customization project. Its official site currently warns that recent Notion updates impaired its functioning. Compatibility with this installed version is not established, so it is not used as the desktop loader.
- [Tailored Notion](https://chromewebstore.google.com/detail/tailored-notion/hapcnpnopbmifahnnehfcncojejoiefn) advertises thinner full-width padding in a browser extension. Its current listing does not establish native Notion 7.37.0 compatibility or smooth caret support.
- [Notion style tweaks](https://github.com/EliWimmer/notion-style-tweaks) and [Notion-enhancer tweaks](https://github.com/notion-enhancer/tweaks) demonstrate browser styling approaches. The latter repository is archived. Neither supplies the complete verified desktop behavior requested here.
- Bounded searches on GitHub, Reddit, Windhawk and Nexus did not identify a complete compatible native solution for both requested features. This is a search result, not proof that none exists.

KEX therefore uses Notion's existing extension system with a small purpose-specific extension, rather than introducing another desktop wrapper or a new injection framework. [Electron's documented extension support](https://www.electronjs.org/docs/latest/api/extensions) includes content scripts, scripting, local storage and per-session loading; it does not guarantee arbitrary Chrome extension compatibility.

## Shipped artifact observations

Installed Notion: 7.37.0. `app.asar` SHA256: `505e04560b9ebcb839f05b8742aae4f2f28788e43139ccbdef7f5c05f35e0e24`.

The bundled extension module reads `app.getPath('userData')/extensions`, scans child directories for a manifest, and calls the application's session extension loader on each permitted extension at startup. Production gates are the ordinary `isChromeExtensionsEnabled` preference and the secure `isChromeExtensionsAllowed` preference. The Debug menu includes a native “Extensions Enabled” checkbox for the latter. KEX preserves that permission boundary and does not copy or decrypt secure settings.

REA fuse evidence: `ev_8c44f1b4b30fc6a94485bc3d149c5c9a5dca9c70c19bf387c7816f3faa8cc6a0`. RunAsNode and Node CLI Inspector arguments are disabled; the patch does not change them. Browser debugging through a private pipe was observed for local testing and is not required at runtime.

REA's whole main-bundle semantic analysis timed out after its bounded run. It also recorded integrity mismatches in unpacked native modules, which can occur after signing. No native modules were changed or executed by the analyzer. Conclusions above are based on direct bounded inspection of the bundled extension module and live layout observation, not a claim of complete REA semantic coverage.

## Live layout observation

In `recon/out/layout-1.json`, the page pane was 716 px wide. Its `.layout.layout-wide` grid was 701 px wide with named columns `[full-start] 96px [content-start] 509px [content-end] 96px [full-end]`. Notion's rule used `--margin-width: 96px` and `--content-width: 1fr`.

The patch changes those variables only on a recognized page grid at narrow pane sizes. It does not replace the grid, change page content, or apply padding rules to every div.

## Native editor cleanup

The first enabled live test showed that Notion removes extra attributes and classes inside `.whenContentEditable`. A temporary, restored probe traced the removal to Notion's `ClientFramework` code. Both an inner layout marker and a caret class were removed within the editor; a marker on the surrounding `.notion-frame` survived.

KEX therefore keeps all layout and caret state on the outer frame, outside the editable tree. CSS scopes stop at nested frames, so each pane retains its own width policy. The caret adapter accepts Notion's outer contenteditable root as the active element while the selection is inside a nested leaf. The browser fixture now reproduces the observed cleanup, and the corrected implementation was retested inside the installed Notion editor.

## Column group width caps (1.0.1)

The reported multi-column inset was reproduced using the current renderer on a [public Notion page](https://notioner.notion.site/9d9c6a658f14412196a2be51e467c9ef), in a fresh headless browser without the user's profile. At a 716 px viewport, `.notion-column_list-block` had inline `width: 100%; max-width: 524px; align-self: center`. Its fixed maximum width remained even when KEX widened the surrounding content grid, leaving the group centered at the original 96 px inset.

Version 1.0.1 sets only that group's maximum width to 100% inside an active compact frame. The public renderer then produced a 668 px group at a 24 px inset. Native column width calculations and divider spacing remain intact; nested groups resolve against their own parent column. The scoped rule stops at nested frames and turns off with compact margins. No live desktop interaction or access to the user's signed-in session was used for this iteration.

## Shared text-container width policy (1.0.2)

The subsequent tabs screenshot exposed the same cap outside column groups. On a [public Notion tabs page](https://j-ann.notion.site/codenames-how-to), the current renderer produced `.notion-tab-block` with `width: 100%; max-width: 458px; padding-inline: 8px` at a 650 px viewport. With 1.0.1, the content grid had 24 px margins but the block still ended at x=482, leaving 168 px on the right. Paragraphs, headings and lists on the public documentation page retained the same obsolete cap.

Version 1.0.2 replaces the column-only exception with a shared rule for known fluid text/container types. Their maximum width resolves against their current parent; their native width, padding, borders, column proportions and overflow behavior remain intact. Resizable media and unknown block types are excluded. The same public tabs block then spanned x=24 through x=626, with its original 8 px internal padding. Explicitly sized video retained its original width. This iteration used fresh headless browsers and did not attach to or control the user's Notion session.
