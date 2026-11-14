# Notion KEX

Responsive page margins, a gliding and blinking typing caret, and browser-style touchpad pinch zoom inside the official Windows Notion desktop app.

## Install on another computer

1. Install and run the official Notion desktop app once, then **Quit** it from the tray menu.
2. Extract `Notion-KEX-1.1.2.zip` to an ordinary folder and run `Install.cmd`.
3. Focus the Notion desktop window and **press and release Alt** to open its application menu. Choose **Debug → Extensions Enabled** once, then Quit and reopen Notion.
4. Open **View → Extensions → Notion KEX** to change the margins, caret motion, touchpad zoom, or disable the patch.

The installer uses PowerShell included with Windows. Node.js, a debugger, administrator rights, and another browser are not needed to use the patch. A custom installation can be selected with `tools/Setup.ps1 -NotionExe <path> -NotionData <profile folder>`.

The application menu is separate from a page's three-dot menu and the workspace menu. If **Debug** is missing, fully Quit Notion from its tray icon and run `Install.cmd` again before reopening it. The installer enables the ordinary Debug-menu preference while Notion is closed; the extension permission still needs your click inside Notion.

To update an existing KEX installation, Quit Notion and run the new package's `Install.cmd`. An already enabled native extension permission does not need to be enabled again.

## How it survives updates

First installation on a new Windows computer needs the native permission step above; it is not fully one click. Once that permission is enabled, later KEX updates use `Install.cmd`. The portable package contains no accounts or computer-specific settings.

The margins and caret live in an unpacked extension in `%APPDATA%\Notion\extensions\Notion KEX`, loaded by Notion's existing extension system. These files and settings remain in the user profile when the application directory is replaced.

Touchpad zoom also needs a small native integration. The installer appends KEX's two zoom scripts to the existing page preload and main entry inside `resources\app.asar`, preserving the original code and every other member. It verifies a backup before replacing the archive and records hashes for safe restoration. Notion.exe, application fuses, the updater and secure settings are not changed. Builds enforcing embedded archive integrity are refused.

**After a Notion application update, Quit Notion and rerun `Install.cmd` to restore native pinch zoom.** `Status.cmd` detects a replaced archive. The installer checks the new app's contracts before changing it; it never restores an older Notion version over an update.

The native extension permission is a separate Notion setting. Enable it on each computer through Notion's own Debug menu; encrypted permission files and account profiles must never be copied between computers. The installer does not alter secure settings.

This is resilient to replacement of the application files, not a promise that every future Notion redesign is compatible. The installer checks for the native loader contract. The layout adapter checks for Notion's named grid before applying changes. If the grid disappears or the caret cannot be measured, Notion keeps its native layout or caret. The sidebar adapter recognizes the current native zero-width navigation container; an unknown width contract keeps native rendering. Notion's cloud-delivered editor can change independently of desktop updates.

## Behavior

- Page panes narrower than 1100 px use a requested margin of 24 px, or 20 px in especially narrow panes. Wide panes retain Notion's own layout. The requested margin and breakpoint are adjustable.
- Both page margins stay compact, including beside the sidebar. There is no added drag gutter. While KEX is enabled, the floating sidebar preview stays hidden on edge hover. Open the native sidebar with its button or **Ctrl + \**; its controls and normal opening animation remain native. Turning KEX off restores native hover behavior.
- Full-bleed covers and named grid lines are retained. The expanded right margin used for a native side panel is preserved.
- Two, three, and larger column groups use the same compact content width. Column ratios, divider spacing, and nested column layouts remain native; wide panes retain Notion's original width caps.
- Tabs, paragraphs, headings, lists, quotes, callouts and code containers fill the available content width on both sides. Their internal padding remains native. Explicit image/video sizes are retained.
- Caret motion is limited to ordinary collapsed text selections. The gliding caret is painted as part of the selected editable element, clipped by its own bounds. It has no separate page-level element or viewport position. The browser carries this paint when the text moves or scrolls and stops painting it when the owner is removed. Cloning a block does not copy its animation effects. Neither text nor editable DOM attributes are modified.
- Restored selections and moved blocks use native rendering until a fresh click, keystroke or text input resumes editing. Style/theme changes also end the old session. This avoids reviving stale selections after a move. An intentionally hidden native caret is never replaced by visible custom paint.
- Native caret suppression is verified before custom paint appears. Highlighted descendants, existing background images, competing background animations, rotations, skew, reflection and unsupported geometry use the native caret so application styling stays intact. Translation, ordinary positive scaling, zoom, wrapping and RTL text are supported. Idle checks do not restart blinking or rebuild animations.
- The idle caret alternates visible and hidden phases. Replacing unchanged editor text does not restart its idle delay; actual editing activity supplies the blink clock.
- Pointer selection, block dragging, IME composition, reduced motion, high contrast, blur, and unsupported geometry use the native caret.
- Pinch with two fingers to zoom smoothly around the pointer from 100% to 500%, using Chromium's own visual viewport. Pinch inward to return to 100%, or press **Ctrl + 0**. Existing Ctrl + / − layout zoom remains available. The Touchpad pinch zoom setting and master switch disable pinch and return the visual viewport to 100%.
- Pinch support is stored in the native view's preferences. Chromium preference refreshes retain it after startup; no initial settings toggle is needed.
- The patch does not change text, intercept typing, replace the editor, or make network requests. Settings are local to each computer.

## Verify and remove

`Status.cmd` checks installed extension files and the native archive record. In Notion's extension menu, KEX must be loaded rather than listed as disabled. Runtime markers include `data-notion-kex-version`, `data-notion-kex-layout`, `data-notion-kex-caret` and `data-notion-kex-pinch`; these contain no page text.

Quit Notion and run `Uninstall.cmd` to remove KEX. It restores the exact backed-up archive when the installed archive still matches KEX's recorded hash, and retains a newer unpatched app after an update. An unexpectedly edited archive containing KEX code is retained with its record and backup for manual recovery. Settings changed by the installer are restored when they still have the installed value. Backups stay under `%APPDATA%\Notion\Notion KEX`; they belong to that computer and are not part of the portable package.

## Development

`npm test` runs policy and native payload checks. `npm run test:browser`, `test:columns`, `test:gutters` and `test:interactions` run isolated headless browser checks. `npm run test:native-zoom` uses a workspace copy of the installed Electron runtime with a generated app and isolated profile; it never loads the signed-in Notion app. `test/installer.ps1` and `test/native-installer.ps1` exercise generated archives and interruption recovery. Run native-runtime and installer fixtures sequentially because process-close checks deliberately reject unidentified Notion processes.

`npm run build` generates checksums and the portable ZIP. Browser and live-app evidence belongs under `test-results/` and `recon/`; these folders, user profiles, credentials, captured pages, and Notion binaries are excluded from the ZIP.

The current native integration was checked against Notion **7.41.0**, Electron **44.9.1**, Chromium **152.0.7977.167** on this PC. Physical touchpad input and the signed-in editor still need a user check after installation. See `docs/research.md` and `docs/verification.md` for evidence and limits.
