# Notion KEX verification

Local Windows PC, 20 October 2026.

## Version 1.1.2: idle cost and scan fixes

6 November 2026, checked in a Linux cloud container with headless Chromium 153 and the same fixtures; the signed-in Notion app was not loaded.

- SonarQube Community, local server, Sonar way: 427 lines in `extension/`, `native/`, `tools/`. One bug (`native/pinch.js` floating promise, fixed with `void`), 0 vulnerabilities, 0 hotspots, 0 code smells, 0% duplication. PSScriptAnalyzer 1.24 on `tools/` and `test/`: 0 errors, style warnings only (`Write-Host` in the interactive installer, unapproved verbs, two tests assigning `$profile`).
- Two new interaction cases: an idle caret requests fewer than 10 frames per second and owns no endless animation, and text inserted before an idle caret (both a new element and an in-place text edit) moves it within 150 ms. The test helper `holdBlink` now finds the blink by its `notion-kex-blink` id.
- Final checks: 10 policy/native payload, 10 general, 27 interaction (three runs in a row), 8 column and 9 gutter cases pass. The native-runtime and PowerShell installer fixtures need the Windows Notion runtime and Windows PowerShell 5.1 and were not run here. The native pinch payload changed only by `void`, so Install.cmd re-applies it from the recorded original backup.

## Version 1.1.1: pinch works before the first settings toggle

28 October 2026. The user reported that 1.1.0 pinch activated only after toggling its setting. This iteration uses the same isolated copied Electron 44.9.1 / Chromium 152.0.7977.167 runtime and generated profile; it does not read or control the signed-in page.

**Debug Summary.** The confirmed defect is renderer-only zoom state being treated as durable view state. The preload's old setter changed Blink defaults but not the browser-owned WebContentsPreferences. A later preference refresh restored 1–1 limits, while identical setting messages reported cached success. The precise triggering refresh in the user's session was not captured. The deterministic fixture invokes the normal preference refresh API after the initial native acknowledgement and attempts the first pinch without any setting interaction. That check fails at scale 1 on 1.1.0 and passes near 1.6 on 1.1.1.

The fix routes the serialized preload request through a narrow main-frame IPC handler to Electron's public WebContents zoom API. Version, types, sender frame and top-level Notion origin are validated before applying a fixed 1–5 or 1–1 range. This updates both persistent view preferences and current rendering. The regression belongs in `test/native-zoom.mjs`, where the earlier fixture had omitted post-initialization preference refreshes; it now also uses Notion's hidden-then-attached WebContentsView structure.

- Nine native-runtime cases pass, including cold-start pinch after a preference refresh, retaining an existing pinch level after later refreshes, pointer anchoring, reset IPC, actual extension storage controls, disabled-state refreshes, limits and exact archive restoration.
- Nine policy/native payload cases pass, including failed acknowledgement/retry, origin/type guards, request ordering, and the new main-handler validation. Native setting checks wait for the real acknowledgement, including when the extension immediately marks a requested disable.
- Restrictive viewport metadata alone and a non-default restored layout zoom still allowed native pinch in diagnostic probes. They were ruled out as direct explanations. Invisible-window requestAnimationFrame scheduling proved unsuitable as a test barrier; the fixture uses a completed renderer command after the host preference update.

**Post-Fix Quality.** Scope is the two native payloads, their two proof test files, release identities and accompanying documentation, compared against `.scratch/startup-111-before`. No Git repository or external publication is involved. Physical touchpad behavior in the signed-in application remains a user check after installing and reopening.

## Version 1.1.0: blinking and native touchpad zoom

28 October 2026. Target application: Notion 7.41.0. The generated native fixture runs in a workspace copy of Electron 44.9.1 / Chromium 152.0.7977.167 with its own profile and intercepted local test page. It never loads the actual Notion app, account or notes. The test window is transparent, inactive, nonfocusable and ignores mouse input.

- Both blink regressions failed before their fixes. An actual running animation now reaches visible and hidden phases; pixel checks confirm the hidden phase. Replacing identical editor text and restoring the same selection every 120 ms no longer starves the idle blink. The existing glide and element-owned paint checks remain green (25 interaction cases total).
- Native zoom tests establish the unmodified-runtime baseline (scale 1), then approximately 1.6x pointer-centred visual zoom after installation. The same fixture supplies Notion-style did-finish-load layout zoom and a zoom-changed handler; pinch produces no layout-zoom event. The real extension policy, storage callback and message path control enable/disable and reset. Native bounds clamp to 1–5. Evidence: `test-results/native-zoom.json`.
- Electron sendInputEvent reaches before-input-event and the fixed main-frame IPC resets visual zoom to 1. A custom Ctrl+0 menu accelerator is installed in the fixture, but an inactive synthetic window does not exercise the native Windows menu accelerator dispatch (menuResets remains 0). CDP keyboard dispatch bypasses the main hook, so it is not used as proof. Physical Ctrl+0 in the signed-in app remains a user check. Production observes the documented event before menu handling and never prevents it.
- Eight policy/native payload checks pass, including failed-native acknowledgement and retry, source/origin/type guards, ordered setting changes, fixed reset IPC, and the restricted main shortcut hook. The settings fixture checks saving and resetting touchpad zoom along with existing controls.
- Windows PowerShell 5.1 installer fixtures pass native install/reinstall, app-update reapplication, exact current-version restoration, unrelated preference retention, integrity-fuse refusal before changes, interrupted commit recovery before/after the swap, preservation of conflicting app changes, and refusal to use an edited backup.
- Two-entry archive rebuild was checked in a copy of the installed 7.41.0 archive under Windows PowerShell 5.1, then independently verified using Node. Both appended payloads match; all other packed bytes and unrelated metadata across 205 packed members and 289 header entries are preserved, including a zero-byte member sharing an offset. The installed archive remained unchanged during these checks. Evidence: `.scratch/asar-twoentry-review-bc305d6c294948768c97f0dcf38944c4`.
- Scoped quality/efficiency/reuse/test/comment review addressed interrupted commits, empty-member bounds, failure acknowledgements, rerender blink phase and uninstall's concurrent-app recheck. No executable, fuse or unpacked native module is modified. The source directory has no Git repository.
- Final checks pass: 8 policy/native payload checks; 10 general, 25 interaction, 8 column and 9 gutter browser cases; 7 native-runtime cases; installer/recovery fixtures; and an exact 20-file source-only ZIP inventory with byte comparisons and extracted-package installation. Native-runtime and installer fixtures are run sequentially to avoid transient closing-process ambiguity.
- Real installation initially stopped before changes when PowerShell 5.1 could not inspect the hidden AppData ancestor. The regular-path check now uses Get-Item -Force without relaxing its reparse-point rejection. An installer fixture with a hidden parent and the extracted portable package passes this regression.
- After the user confirmed Notion was closed, installed 1.1.0 and verified all extension hashes and the native record. Original archive SHA256: `4AB8615C1198F81E822564F2769770084DA03B5CE64C7A49D672DEE3B8978864`; patched archive: `B08567B8B6D7B34F55CE84809451F6D30A19FD3FA77DD267892175F9A001B3C7`. The original archive backup matches. Notion.exe, state.blob, Preferences, Local State and notion.db retained their hashes; unrelated state.json settings and other disabled extensions were structurally unchanged. The prior extension is backed up, and Notion was left closed. Evidence: `test-results/installed-1.1.0.json`.

The blink correction preserves text-owned paint, so it does not recreate the detached overlay. Native pinch is an additional app-archive patch and must be reapplied after a Notion update. Physical touchpad input, native Windows menu accelerator dispatch and the user's exact signed-in editor interactions are not established by these fixtures.

## Version 1.0.6: text-owned gliding caret

28 October 2026. This replaces 1.0.5's viewport overlay after the user's report of detachment following typing and block movement. Both reliable ownership and gliding movement are required. All application interaction in this iteration uses isolated synthetic headless Edge fixtures; the signed-in Notion page is not opened, read or controlled.

- The standalone ownership probe passes: element-owned paint moves with its text without position tracking, is absent from a clone and disappears when the owner is removed (`.scratch/owned-caret-probe.mjs`).
- Twenty-three interaction cases pass. They include actual mid-animation caret pixels, three cycles of typing and native block dragging, removal and cloning, clipping after resizing, own scrolling, wrapped text, 1.25x zoom, RTL, formatted text, native suppression failure, selection and drag lifecycle, theme changes, application background classes and animations, unsupported transforms, synthetic pointer events, restored selections and intentionally hidden native carets.
- The ownership and clipping checks stop all fixture requestAnimationFrame callbacks and disconnect MutationObservers before moving/resizing/removing the owner. Caret pixels still follow the browser's owner paint and clipping. After removal the fixture also clears native selection to distinguish custom paint from a legitimate native caret relocated by Chromium. Pixel captures preserve caret styling; Playwright's default screenshot mode mutates inline caret styles and would itself invalidate an editing session.
- The existing general browser suite (10, including IME), column suite (8), gutter suite (9) and policy checks (4) pass: 54 policy/browser checks total. JavaScript syntax passes. There is no configured lint or typecheck command.
- Scoped reuse, quality, efficiency, test and comment reviews examined only this redesign against `.scratch/caret-106-before`. Review findings on transformed descendants, native app backgrounds/animations, color changes and trusted pointer intent were addressed with fallbacks and regression coverage. Animation cleanup cancels only KEX-owned effects and disconnects its active style observer.

### Debug Summary

**Problem:** a cursor independent of the selected text can be stranded by a block move despite more tracking checks.

**Root cause and change:** the earlier renderer owned a fixed element under the document root and stored viewport coordinates. Version 1.0.6 removes that element and its CSS entirely. The caret is now background paint on the actual selected editable element. Browser painting owns its bounds, scroll behavior and disappearance. Block movement also revokes the editing session until fresh trusted editing intent. Ordinary character movement retains its configured glide.

**Prevention:** pixel-based ownership checks with JavaScript tracking stopped; clone/removal, repeated type-and-drag, clipping and actual interpolation checks. The earlier fixtures tested successful tracking paths and therefore did not establish paint ownership.

**Confidence:** high for the physical ownership invariant and covered fixtures. The user's exact signed-in Notion drag sequence has not been observed during this iteration. The design removes KEX's independent viewport cursor; it is not a claim that every possible Notion caret or rendering bug is impossible.

### Post-Fix Quality

**Scope:** caret rendering, its existing browser/interaction tests, release identities and documentation. **Simplify/review:** scoped read-only reuse, quality, efficiency, tests and comments reviews; final targeted manual review. No unrelated layout behavior was changed. **Re-verification:** 23 interaction checks and 10 general checks pass after the final native-caret ownership guards; layout suites remain green. **Residuals:** live desktop verification remains explicit. **Publication:** local distribution only; this source directory is not a Git repository.

## Version 1.0.5: stale floating caret

28 October 2026. The installed application is Notion **7.41.0**. This iteration uses isolated headless Edge fixtures and does not attach to, control, launch or restart the signed-in application.

- Four new interaction checks fail against a separate copy of 1.0.4 and pass with 1.0.5: idle text movement without selection/resize events, a hidden selected editor, loss of native caret suppression during idle, and a selected editing leaf becoming noneditable. The existing seven interaction cases pass on both versions. Evidence: `.scratch/repro-105-old/test-results/interactions.json` and `test-results/interactions.json`.
- The visible overlay now revalidates selection ownership, content visibility, current geometry and native caret suppression every animation frame. Layout changes follow immediately, while selection changes retain smooth motion. Invalid ownership hides the overlay, restores native rendering and stops its loop. Idle validation does not reset blinking or rewrite unchanged overlay styles.
- Four policy/manifest checks and 38 headless browser checks pass: 10 general, 8 column, 9 gutter and 11 interaction cases. The final caret checks were rerun after the review cleanup. JavaScript syntax and package version identities also pass. The sandbox blocked the normal Node test runner's subprocess; the same policy tests pass using `--test-isolation=none`. No lint or typecheck scripts are configured.
- The PowerShell installer fixture passes install, reinstall, simulated application replacement and reversible uninstall. A separate file-only update fixture verifies installed hashes and preserves a profile-state sentinel byte for byte.
- Installed 1.0.5 through the verified file-only updater, retaining a backup of 1.0.4. Status confirms matching installed hashes. Notion 7.41.0 retains the native loader contract; `state.json` and `app.asar` retain their recorded pre-update hashes. Secure settings were not written. The application was neither launched nor stopped; the running script is replaced on the user's next full reopen.
- Scoped code reuse, quality, efficiency, test and comment reviews found no unresolved correctness issue. The efficiency pass removed an unnecessary idle computed-style read. The comment pass removed two comments that repeated code. Proof tests were retained. The source directory is not a Git repository; no commit or remote publication is involved.
- These fixtures establish the reproduced failure modes, not the exact trigger on the user's page or compatibility with every future cloud editor change. Verification in the signed-in desktop remains for the user after reopening Notion.

### Debug Summary

**Problem:** the smooth caret can stay floating or coexist with a native caret after the selected editor changes without a selection/input event.

**Root cause:** `drawCaret` previously ran only after a limited event set. CSS transforms/visibility/editability/restyling can change the selection's visual target without any of those events or a layout-size change. Its independent blink timer and CSS animation keep drawing the old overlay. Native caret suppression can also cease to apply while the overlay is idle. Previous tests covered the initial suppression decision and event-driven changes, leaving this idle lifecycle untested.

**Fix and prevention:** validate the visible overlay throughout its lifetime and add the four failing-before/passing-after interaction cases to the existing interaction suite. Preserve native fallback, blink timing, text content and existing layout behavior.

**Confidence:** high for the reproduced cases; the user's live intermittent trigger has not been observed in this iteration.

### Post-Fix Quality

**Scope:** only the caret change, its four interaction checks, release identities and accompanying documentation. Original caret/test sources are retained in `.scratch/caret-105-before` for comparison.

**Simplify:** reviewed reuse, quality, efficiency, test value and comments. Applied one efficiency improvement and removed two redundant comment lines. No proof tests removed or weakened.

**Review:** scoped read-only reviews and targeted manual review; no unresolved findings. **Re-verification:** all four regression cases pass after the final production edit. **Residuals:** live desktop observation remains explicitly unverified.

## Version 1.0.4: compact margins without sidebar hover

24 October 2026. The extra leading drag gutter from 1.0.3 is removed. The current sidebar contract was checked through anonymous public renderer assets in isolated headless Edge. No desktop interaction or connection to the user's Notion session was used.

- The revised interaction fixture first failed on the old 1.0.3 gutter, pinned-page spacing and native hover restoration/remount behavior (`.scratch/interactions-104-red.log`). All seven interaction cases pass after the change.
- At 390, 650 and 960 px, both margins remain 20/24 px and the accessible native drag control remains at the compact page edge. Edge movement does not expose navigation, and native drag/drop still reorders blocks. A separately simulated Electron peek request also stays hidden despite changing native visibility and accessibility state.
- The existing sidebar button and Ctrl + Backslash expand/collapse the native nav; sidebar items remain clickable. Disabling KEX restores hover. A remounted sidebar is recognized, and an unknown width contract keeps its native rendering. Sidebar suppression remains active when compact margins and caret smoothing are separately disabled.
- The formatted-text, selection, unhideable-caret and drag-cancellation checks remain green. Evidence: `test-results/interactions.json` and `test-results/compact-no-sidebar-hover.png`. Actual interaction in the signed-in desktop app remains for the user after reopening it.
- Four policy/manifest checks and 34 headless browser checks passed: 10 general, 8 column, 9 gutter and 7 interaction cases.
- The Windows PowerShell installer fixture passed install/reinstall, simulated application replacement and reversible uninstall. The separate file-only update fixture preserved its state sentinel.
- Installed 1.0.4 into the existing native extension directory using the verified file-only updater. Installation status confirms matching package hashes on Notion 7.40.0. Both `state.json` and `app.asar` retained their pre-update hashes; secure settings were not touched. Notion was neither launched nor stopped. The user must fully Quit and reopen it to replace the running extension scripts.

## Version 1.0.3: block controls and a single typing caret

23 October 2026. The installed application is now Notion **7.40.0**. This iteration used isolated headless Edge fixtures; it did not attach to, control, launch or restart Notion.

- Reproduced a drag handle at x=-16 in a 390 px compact page and a second visible native caret on formatted text while the KEX overlay was active. A third negative case reproduced the overlay appearing even when native caret suppression was impossible.
- Added a sidebar-aware leading gutter. It reserves space for the native control pair beside both collapsed and pinned sidebars, with extra clearance at the screen edge. Trailing margins and pages without a sidebar retain the compact policy. Native drag/drop, sidebar buttons and intentional edge hover are preserved.
- Expanded native caret suppression to formatted descendants, then verified suppression on the selection anchor, editable leaf and focused editing root before painting the overlay. Unhideable native carets fall back to one native cursor. Pointer selection and block dragging suspend the overlay, including Chromium's pointer-cancellation sequence during native drag/drop.
- Four policy/manifest checks and 33 headless browser checks passed: 10 existing browser checks, 8 column checks, 9 gutter checks and 6 interaction checks. The interaction fixtures cover native block reorder, intentional sidebar opening, inset pages, formatted text, an inline-important native caret, pointer selection and drag cancellation.
- Evidence: `test-results/interactions.json`, `test-results/drag-lane.png`, and the existing browser/column/gutter reports. The installer fixture checks install/reinstall, application replacement and reversible uninstall. A separate file-only update fixture verifies that already enabled extension files can be refreshed without writing running profile settings. Distribution remains a native unpacked extension and a portable installer; screenshots and user profiles are excluded.
- Live application drag-handle geometry and caret behavior remain for the user's check after reopening Notion. The fixture's protected sidebar lane is not a claim that every future Notion hover target has identical dimensions.
- The installed profile was updated through the verified file-only path because Notion was running. `state.json` retained its SHA256, and Notion 7.40.0's `app.asar` retained `5DB8ED212BD934AE8C0398663B1E34C311154D19E9DBD4267BBB9D40217E39F8`. The current application still contains the native extension-loader contract. Reopening Notion is left to the user.

## Version 1.0.2: both page edges and text containers

- Reproduced the screenshot's asymmetry on a public Notion tabs page: with 1.0.1 the grid had 24 px gutters but the tabs block kept its 458 px cap and ended at x=482 in a 650 px viewport. With 1.0.2 it spans x=24 to x=626, leaving 24 px on each side. The native 8 px block padding is retained.
- Verified paragraphs, headings and lists against the current public Notion renderer. They fill the widened content area; explicitly sized media retains its width.
- Nine headless gutter cases verify both edges at 390/640/650/960/1440 px, tab switching, nested text containers, RTL, configurable gutters, native media sizes, full-bleed covers, reserved right-panel space and restoring native caps when disabled.
- Ten existing browser checks, eight column cases, three policy checks and the installer fixture pass. No desktop control, user-session attachment or Notion restart was used. Updated installed files take effect on the user's next restart.
- Evidence: `test-results/gutters.json`, `test-results/gutters-640.png`, `test-results/public-tabs-widths.json`, `test-results/public-text-widths.json`. Public-page checks initially timed out during parallel loading and passed when rerun serially.

## Version 1.0.1: columns

- Reproduced the fixed pixel width cap on a current public Notion page in a fresh headless Edge session, then verified the real patch code changed its column group from 524 px / 96 px inset to 668 px / 24 px inset at a 716 px viewport.
- Eight column regression cases cover 390, 650, 716, 960, 1099, 1100 and 1440 px viewports and restoring native caps when disabled. Each width checks two, three and five columns, native ratios/dividers and contained table scrolling. Compact widths also check nested groups and an explicitly narrower group.
- The ten existing browser checks and three policy checks were rerun headlessly. This iteration did not control, close, restart or attach to the user's Notion app. The installed extension files are refreshed for the user's next restart.
- Evidence: `test-results/columns.json`, `test-results/columns-*.png`, `test-results/public-columns.json`. These are excluded from the portable package.

## Passed

- Three policy/manifest tests: pane breakpoints, malformed settings, bounded configuration, Notion-only matches and local-storage-only permission.
- Browser regression checks in installed Edge 143 using the named grid observed in Notion 7.37.0: widths 390, 716, 960 and 1440; selected text; IME composition; reduced motion; focus leaving the editor; disabling the patch; SPA remount; unrecognized layout fallback; nested pane independence. The fixture reproduces Notion's removal of extra attributes/classes inside its editable tree. The editor's text remained unchanged.
- Installer fixture: fresh install, repeat install, replacement of the simulated application package, unchanged application files, unrelated preference/history retention and reversible uninstall.
- The installed Notion `app.asar` retained SHA256 `505e04560b9ebcb839f05b8742aae4f2f28788e43139ccbdef7f5c05f35e0e24` after installing KEX.
- Live Notion layout reconnaissance through a temporary private pipe. The observed 716 px pane and its native grid are recorded in `recon/out/layout-1.json`.

## Version 1.0.0: passed inside the installed Notion app

The user enabled the native permission through the application menu (press and release Alt → Debug → Extensions Enabled). The loader then reported Notion KEX loaded and ready. After correcting the native editor-cleanup issue described in `research.md`, these checks passed through a temporary private debugging pipe to the actual installed Notion 7.37.0 renderer:

- The ordinary 662 px page grid retained 24 px margins and 614 px content width after settling.
- Actual Notion renderer viewport emulation: 390 px viewport / 375 px grid → 20 px margins; 960 / 945 → 24 px; 1440 / 1425 → native centered 720 px content with 352.5 px margins. Viewport emulation was cleared afterward.
- A collapsed selection in a real text leaf produced the overlay and a 100 ms transform transition, with Notion's outer editable root focused. A range selection restored the native caret; collapsing it resumed the overlay. The page's text remained unchanged, and the prior selection/focus were restored.
- Emulated reduced motion hid the overlay and restored the native caret. The emulation was cleared afterward.
- A saved 26 px margin applied immediately, survived a full Notion restart, and was then restored to the default 24 px. Disabling KEX restored native 96 px margins.
- Repeated full restarts logged the extension loaded and ready without re-enabling its permission.
- The private test connection was closed, and Notion was reopened normally. Its native extension log again recorded KEX loaded and ready at 19:10:07 local time.

## Limits

A real Notion auto-update was not installed for this test. Update survival was verified through application-file separation and the installer replacement fixture; future editor/loader changes can still require a KEX update.

The settings controls passed save/reset/keyboard checks in the local Edge fixture, and the same storage API was verified in the native extension context. The native settings window was not visually verified: Windows window capture failed with `FrameArrived timed out` and `window capture timed out`. Full native accessibility-text capture was rejected by automatic approval review, so no further accessibility-text extraction was attempted. This did not block the text-free native runtime checks above.

## Artifacts

`test-results/browser.json`, grid screenshots and the settings screenshot are local development evidence. Native screenshots may include the user's page and are excluded from distribution. `dist/Notion-KEX-1.0.6.zip` includes the extension, installer, checksums and documentation only.
