# Patch notes

Notion KEX adds to the official Notion desktop app for Windows. Run the newest `Notion KEX.exe` to update; your settings are kept.

## 1.2.0 · November 2026

### New

- **One file**: `Notion KEX.exe` installs, launches, repairs and uninstalls Notion KEX. No PowerShell window, no zip to extract.
- **Double-click to install or update**: running a downloaded `Notion KEX.exe` asks before it installs, and waits for you to quit Notion instead of failing.
- **Repairs itself after Notion updates**: after an app update, pinch zoom comes back the next time Notion fully closes, or at the next sign-in. KEX never closes Notion for you.
- **Taskbar-friendly shortcuts**: your existing Notion shortcuts open Notion through KEX and keep Notion's icon and taskbar group.

### Changed

- Checked against **Notion 7.42.3**.
- Uninstalling after an upgrade from the older zip package now leaves your Start menu shortcut opening Notion, instead of pointing at a removed script.

## 1.0 – 1.1 · October 2026

### New

- **Touchpad pinch zoom**: pinch with two fingers to zoom around the pointer, from 100% to 500%. <kbd>Ctrl</kbd>+<kbd>0</kbd> resets.
- **Columns that fit**: column groups use the same compact width as the rest of the page.
- **Full-width blocks**: tabs, headings, lists, quotes, callouts and code blocks fill the page width.
- **No edge-hover sidebar** while KEX is on. <kbd>Ctrl</kbd>+<kbd>\</kbd> and the sidebar button still open it.
- **Blinking smooth caret**: the gliding caret now blinks when you stop typing.

### First release

- **Room to work**: slim, even page margins in narrow windows, with an adjustable margin and breakpoint.
- **Smooth caret**: the caret glides to where you type.
- **Settings inside Notion**: *View* > *Extensions* > *Notion KEX*.
