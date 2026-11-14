# Notion KEX native host for Windows

`native-host/` builds **Notion KEX.exe**, the fully native Windows host for
Notion KEX. It installs the embedded extension, applies the native Chromium
pinch-zoom patch, launches Notion through a repair-aware fast path, repairs the
patch after a full quit or an app update, and manages shortcuts and the taskbar
identity. Production launch, detection, repair and uninstall never invoke
PowerShell.

The host embeds the extension and both native payloads, so the installed
`Notion KEX.exe` is self-contained. The legacy PowerShell installer and
`native/pinch-main.js` remain valid and are not modified by this component;
the native main-process payload lives in `native-host/payloads/pinch-main.js`.

## Status

Implemented and covered by generated fixtures:

- Native ASAR parsing, member replacement and structural verification.
- Backup, pending-journal, rollback, recovery and concurrent-change protection.
- Extension install, health inspection and preference-preserving uninstall.
- Native zoom install, reinstall, update repair, validate and uninstall.
- Launcher fast path, full repair, update watcher and sign-in repair check.
- Desktop, Start menu, taskbar and startup shortcut handling with restore.
- CLI modes, JSON status, redirected-output handling and GUI message boxes.
- An 83-case fixture suite: 79 passed, 0 failed, 4 skipped in the current
  evidence run. The four skips are the sign-in and watcher cases that
  deliberately defer while the live Notion process is running.

The original safety stop is **not** claimed to be resolved. See
[Limitations](#limitations).

## Safety model

The host never closes, restarts or updates Notion behind the user's back.

- A full native zoom transaction runs only while `Notion.exe` from the target
  installation is closed. If it is running, the operation fails with a clear
  message instead of touching the archive.
- The launcher repairs synchronously only when Notion is already closed.
- The update watcher waits for Notion **and** its updater to exit, with a
  30-minute bound. It never closes either process and never launches Notion.
- The sign-in check repairs only when Notion and its updater are closed.
- The first Notion session after an app update can lack pinch zoom until the
  user fully quits. That is intentional: the archive is never changed while the
  signed app is running.
- Every profile write is preceded by validation of the app archive, extension
  loader contracts, Electron fuse state, path ownership and recovery material.
- A conflicting archive change during recovery retains every backup and
  journal file and refuses to guess.

## Layout

```text
native-host/
  CMakeLists.txt
  README.md
  cmake/embed_files.cmake        embeds extension and payload bytes
  include/notion_kex/
    asar.hpp                     ASAR parse/edit/verify API
    embedded.hpp                 embedded inventory and payload hash
    hash.hpp                     SHA-256 API
    launcher.hpp                 launch, watcher and sign-in entry points
    native_zoom.hpp              quick probe and transaction entry points
    record.hpp                   record/journal formats, paths, validation
    setup.hpp                    extension lifecycle
    shortcuts.hpp                shell links, launcher copy, cleanup
    status.hpp                   passive status state machine
  payloads/pinch-main.js         native main payload with watcher hook
  src/
    asar.cpp                     bounded ASAR engine
    embedded.cpp                 embedded file table and payload hash
    file_util.*                  atomic JSON, file replacement, paths
    launcher.cpp                 fast path, watcher, sign-in check
    native_zoom.cpp              transactions, recovery, uninstall
    notion_kex_cli.cpp           Notion KEX.exe modes and GUI behavior
    process.cpp                  process enumeration, fuse scan, mutexes
    setup.cpp                    extension install/uninstall/status
    shortcuts.cpp                Shell Link, AUMID, launcher lifecycle
    status.cpp                   passive native status
    cli.cpp                      retained passive kex-status.exe
  third_party/nlohmann/json.hpp  pinned nlohmann/json 3.11.3
test/
  native-host-full.cpp           full fixture suite
  native-host-full.ps1           configure/build/test/evidence runner
  native-host-status.cpp         original passive status suite
  native-host-status.ps1         original passive status runner
test-results/
  native-host-full.json          per-case fixture evidence (generated)
  native-host-full-build.json    toolchain, hashes, protected files (generated)
```

`native-host/build/` is generated and ignored by `.gitignore`.

## Defaults and overrides

| Setting | Default |
| --- | --- |
| `Notion.exe` | `%LOCALAPPDATA%\Programs\Notion\Notion.exe` |
| Notion profile | `%APPDATA%\Notion` |
| Launcher folder | `%LOCALAPPDATA%\NotionKEX` |
| Installed launcher | `%LOCALAPPDATA%\NotionKEX\Notion KEX.exe` |
| Extension folder | `<profile>\extensions\Notion KEX` |
| Native metadata | `<profile>\Notion KEX` |

Every mode accepts `--notion-exe`, `--notion-data` and `--launcher-root` to
override these paths. The fixture suite always supplies explicit paths and
never uses the real known folders.

## CLI modes

```text
Notion KEX.exe [mode] [options]

launch              Repair if needed, then start Notion (default).
status              Report extension, native zoom and launcher state.
install             Install the extension, native zoom and shortcuts.
uninstall           Restore shortcuts and remove Notion KEX.
repair              Reapply the native zoom patch with Notion closed.
validate            Check whether this app archive is supported.
watch               Hidden update watcher used by the zoom payload.
signin-check        Hidden startup repair check.
shortcut-install    Install or refresh launcher shortcuts only.
shortcut-uninstall  Restore launcher shortcuts only.
```

Options:

```text
--notion-exe <path>     Notion.exe path.
--notion-data <path>    Notion profile directory.
--launcher-root <path>  Notion KEX launcher directory.
--json                  Machine-readable status output.
--no-launch             Do not start Notion after install/launch.
--no-shortcuts          Leave desktop, startup and pinned shortcuts unchanged.
--quiet                 Suppress success output.
--help                  Show help.
```

`--no-shortcuts` exists for automated verification. A production `uninstall`
should restore shortcuts before removing the launcher; using `--no-shortcuts`
leaves existing shortcuts pointing at the removed launcher.

`Notion KEX.exe` is a GUI-subsystem executable. When launched from Explorer
without a console it shows message boxes for errors and interactive results.
When stdout is a console, a pipe or a regular file, it writes UTF-8 text
instead. `--json` always writes machine-readable output and never shows a
message box.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Success. For `status`, the extension is healthy and native zoom is installed. |
| 1 | Operation failed, or `status` found an unhealthy/incomplete installation. |
| 2 | Usage error or unknown mode. |

## Install lifecycle

`install` performs these steps in order:

1. Copy the running executable to the launcher folder. An existing launcher is
   replaced atomically; a running old image is renamed first.
2. Validate the Notion executable, archive, extension loader contracts and
   Electron fuse state.
3. Recover any pending native zoom journal under the per-archive mutex.
4. Validate that the native patch is supported by this exact archive.
5. Back up `state.json` on first install and record the two preferences that
   the installer may change.
6. Install the seven embedded extension files plus the ownership marker.
7. Enable Chromium extensions and the Debug menu in Notion's stored
   preferences, and remove `Notion KEX` from `disabledExtensions`.
8. Apply the native zoom transaction.
9. Repoint existing Notion shortcuts, create the desktop shortcut and create
   the startup sign-in shortcut.
10. Optionally start Notion unless `--no-launch` is present.

The one-time runtime permission is still manual: focus the Notion desktop
window, press and release Alt, then choose **Debug → Extensions Enabled**. A
stored extension alone does not prove that Notion's native permission is on.

`uninstall` reverses the extension and native zoom changes, restores the
original shortcut files from the manifest, restores the two preferences only
when they still match what the installer wrote, removes the extension folder
and launcher folder, and leaves notes, other extensions and secure Notion
settings untouched.

## Native zoom on-disk contract

For a Notion installation at `Notion.exe`, the archive is
`<Notion.exe directory>\resources\app.asar`. The metadata root is
`<Notion data directory>\Notion KEX`.

Committed record, `native-zoom.json`:

```json
{
  "asarPath": "C:\\...\\resources\\app.asar",
  "backupPath": "C:\\...\\Notion KEX\\app-asar-before-native-<32 hex>.bak",
  "originalHash": "<64 hex>",
  "patchedHash": "<64 hex>",
  "payloadHash": "<64 hex>",
  "installedAt": "<UTC timestamp>",
  "patchedSize": 30270723,
  "patchedWriteTimeUtc": 134000000000000000
}
```

`patchedSize` and `patchedWriteTimeUtc` are optional fast-path fields written
by the native host. PowerShell ignores extra fields, so records remain
interchangeable in both directions. The native host refreshes missing **or
stale** metadata after a full verification; this repairs records written by the
legacy PowerShell installer.

Interruption journal, `native-zoom-pending.json`:

```json
{
  "asarPath": "C:\\...\\resources\\app.asar",
  "previousHash": "<64 hex or null>",
  "nextHash": "<64 hex>",
  "rollbackPath": "C:\\...\\Notion KEX\\native-rollback-<32 hex>.bak",
  "previousRecord": null,
  "newRecord": { "...": "same shape as native-zoom.json" }
}
```

Validation mirrors `tools/NativeZoom.ps1`:

- Metadata files are limited to 1 MiB, state to 64 MiB, JSON nesting to 64
  levels and the ASAR header to 16 MiB.
- The archive is limited to 512 MiB before hashing.
- Hashes are 64 ASCII hex characters. Reading accepts uppercase hex and
  normalizes it to lowercase; comparisons are case-insensitive.
- `asarPath` must resolve to the requested archive.
- `backupPath` and `rollbackPath` must be under the requested metadata root and
  use the exact leaf patterns above.
- No existing component of a checked path may be a symbolic link, junction or
  other reparse point.
- The committed record's backup must exist and hash to `originalHash`.
- A pending journal's `newRecord.patchedHash` must equal `nextHash`; its new
  record backup, optional previous record backup and rollback file must all
  validate.
- Extra JSON fields are ignored, matching PowerShell's property access.
- A UTF-8 BOM is accepted.

### Transaction

`InstallNativeZoom` and `RepairNativeZoom` share one transaction:

1. Acquire `Local\NotionKexNativeZoom-<sha256(archive path)>`.
2. Resolve any pending journal.
3. Verify that Notion is closed and the Electron fuse is compatible.
4. Read the valid committed record and the current archive hash.
5. If the archive already matches the record and the embedded payload hash,
   refresh stale fast-path metadata and return without rewriting the archive.
6. Parse the ASAR, reject an unrecorded marker, verify the loader contracts and
   build the replacement members in a sibling temporary file.
7. Re-parse the temporary archive and verify that only the two intended members
   changed and that each replacement hashes correctly.
8. Create a verified original backup if this is a new patch, or reuse the
   existing verified backup for a payload refresh.
9. Write a rollback copy of the current archive, then atomically write the
   pending journal.
10. Re-hash the archive, re-check that Notion is closed, and replace the
    archive with `ReplaceFileW`.
11. Capture the post-replacement size and write time, rewrite the journal with
    those values, write the committed record, and recover the journal.
12. On failure after the swap, restore the rollback archive before recovery so
    the journal can resolve cleanly.

Recovery always resolves the journal before any other work:

- Archive matches `nextHash`: commit `newRecord`.
- Archive matches `previousHash`: restore `previousRecord`, or remove the
  committed record when there was no previous record.
- Anything else: retain the journal, rollback and backups, and fail with a
  conflict message.
- After a successful resolution, remove the journal and rollback file.

Passive `status`, `validate` and `QuickInspect` never recover a journal. Only
`launch`, `repair`, `install`, `uninstall`, `watch`, `signin-check` and the
explicit `RecoverNativeZoom` entry point do.

### Electron fuse and payload checks

Before any native change, the host scans `Notion.exe` for the Electron fuse
sentinel `dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX` and requires:

- `wire[0] == 1`
- `wire[1] >= 6`
- `wire[6] == '0'` (embedded ASAR integrity enforcement disabled)

Otherwise it reports `Unsupported Electron fuse wire.` or
`This app enforces embedded ASAR integrity...` and makes no changes.

The payload hash is SHA-256 over the preload payload hash and the main payload
hash joined by a single LF, preload first. It matches
`tools/NativeZoom.ps1` and lets a payload update refresh an existing patch
without replacing the original backup.

## Launch, watcher and sign-in repair

`launch` (the default mode) uses a cheap fast-path probe:

- Record, backup ownership and optional size/write-time metadata must be
  valid. The archive is not hashed on the fast path.
- If the probe says current, Notion starts immediately.
- If the probe is uncertain, the host runs the full transaction, but only when
  Notion is closed.
- If Notion is running and the patch is stale, the launch fails with the
  "quit completely from the tray" message. The host never closes Notion.

The native main payload starts `Notion KEX.exe --watch` from
`before-quit-for-update` and `before-quit`. The watcher:

1. Exits immediately when Notion KEX is not installed or another watcher holds
   `Local\NotionKexWatch-<sha256>`.
2. Waits up to 30 minutes for Notion and its updater to exit.
3. Waits 750 ms for updater file handles to close.
4. Repairs only if the fast path is stale.
5. Never closes or launches Notion.

The startup shortcut runs `Notion KEX.exe --signin-check`. It repairs a stale
patch when Notion and its updater are closed and otherwise defers. The first
session after an update may therefore lack pinch until a full quit.

## Extension lifecycle

The embedded extension contains `kex.css`, `kex.js`, `manifest.json`,
`policy.js`, `settings.css`, `settings.html` and `settings.js`.

`InstallExtension` validates everything before the first profile write:

- Notion is closed.
- The profile path is a real directory under the expected Notion data root.
- The app archive exists, is within the size bound and contains the expected
  loader contracts.
- `state.json` exists and has the supported
  `appState.preferences` object shape.
- The extension folder is absent, empty, or owned by the Notion KEX marker.
- No path component is a reparse point.
- The native patch is supported by this archive.

It then writes the extension files, sets `isChromeExtensionsEnabled` and
`isDebugMenuEnabled` to true, removes `Notion KEX` from `disabledExtensions`,
and records the previous preference values plus a `state.json` backup.

`UninstallExtension` restores those two preferences only when their current
values still match what the installer wrote, leaves `disabledExtensions`
enabled when other extensions remain, removes the extension folder, and
retains notes and unrelated settings. `status` is passive and reports missing
or changed extension files without repairing them.

## Shortcuts and taskbar

- Existing Notion shortcuts in the desktop, Start menu and taskbar folders are
  repointed to the launcher. A shortcut with arguments is left alone.
- A `Notion KEX.lnk` desktop shortcut is created if missing.
- A `Notion KEX.lnk` startup shortcut runs `--signin-check`.
- Shortcuts use Notion's icon (`Notion.exe,0`) and the AppUserModelID
  `com.electron.notion`, preserving taskbar grouping and identity.
- Every changed or created shortcut is recorded in
  `<launcherRoot>\shortcuts.json`; uninstall restores original `.lnk` bytes from
  `shortcut-<guid>.lnk` backups and deletes the shortcuts it created.
- Launcher replacement is atomic and verified by SHA-256. Removing the launcher
  while running from it spawns a detached cleanup copy that waits for the
  parent process and then removes the folder.

## Build and test

Prerequisites:

- Windows 10 or 11, x64.
- Visual Studio 2022 Build Tools with the C++ x64 toolchain.
- CMake 3.21 or newer. The evidence was produced with the Visual Studio 2022
  CMake 3.31.6 and MSVC 19.44.
- No network access is needed: nlohmann/json is vendored.

One-shot verification from the repository root:

```powershell
.\test\native-host-full.ps1
```

The script:

1. Locates Visual Studio with `vswhere`.
2. Configures a Visual Studio 17 2022 x64 build with `BUILD_TESTING=ON`.
3. Builds `notion-kex`, `kex-native-tests` and `kex-status-tests`.
4. Runs the fixture suite with `--evidence test-results\native-host-full.json`.
5. Writes `test-results\native-host-full-build.json` with the toolchain,
   commands, results, executable hashes, source hashes and before/after hashes
   of `checksums.json`, `dist/`, `native/pinch.js`, `native/pinch-main.js` and
   the two authoritative PowerShell tools.
6. Fails if any protected file changed.

It does not run `npm run build`, does not touch `dist/`, `checksums.json` or
the live Notion installation, and does not use the real desktop, Start menu or
startup folders.

Manual equivalent:

```powershell
cmake -S native-host -B native-host\build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build native-host\build --config Release --target notion-kex kex-native-tests kex-status-tests
.\native-host\build\Release\kex-native-tests.exe --evidence test-results\native-host-full.json
```

`ctest --test-dir native-host\build -C Release --output-on-failure` runs both
the original passive status suite and the full native suite. The full test
executable also accepts `--filter <text>`, `--keep` and `--help`.

## Test coverage

The full suite uses one temporary tree per case and never touches the live
Notion installation or the real shell shortcut folders. It covers:

- install, reinstall, stale fast-path repair, payload refresh and update repair;
- uninstall with the original, replaced, edited and missing archive;
- fuse integrity, missing fuse, old fuse wire and wrong sentinel rejection;
- marker, loader-contract, invalid UTF-8, missing member and missing archive
  rejection;
- invalid, wrong-archive and malformed records;
- closed-process enforcement, path filtering and busy mutexes;
- recovery before/after the swap, previous-record restore, conflict retention,
  malformed journals and missing rollback files;
- passive status, quick probe and validate behavior with byte-for-byte fixture
  snapshots;
- extension install, health inspection, preference restore, foreign-folder
  rejection, missing/unsupported state and loader-change rejection;
- shortcut repointing, idempotent reinstall, restore, launcher copy, folder
  removal and junction rejection;
- launcher fast path, stale repair, failure logging, running-Notion deferral
  and missing-Notion handling;
- sign-in and watcher deferral, repair and busy behavior;
- CLI help, unknown mode, status JSON, install/status/uninstall, quiet mode,
  validate, repair, fuse rejection and malformed-record status;
- embedded inventory, manifest identity and payload hash;
- optional fast-path metadata parsing in string and numeric forms.

Evidence records each case's assertions, passive snapshot entry count, skip
reason and build metadata. Invalid UTF-8 in diagnostic strings is replaced
rather than crashing evidence collection.

## Limitations

- **The original safety stop/rejection reason remains unknown.** This
  implementation was completed under the user's direction, but it does not
  claim that the stop is resolved. Preserve the reason if it reappears.
- Live end-to-end validation against the real Notion installation is deferred
  because the live installation must not be modified. Fixture tests carry the
  verification burden.
- Fixture tests do not prove physical touchpad behavior, taskbar grouping on a
  real taskbar, updater races against a signed production build, or signed-in
  caret behavior.
- The sign-in and watcher cases are skipped while the live Notion process is
  running, because those modes deliberately defer or wait. The skip reasons are
  recorded in the evidence.
- The first session after a Notion update may lack pinch until a full quit.
  This is a safety property, not a bug.
- `kex-status.exe` remains available as the original passive status slice; the
  production entry point is `Notion KEX.exe`.
- The native host project version is `1.2.0`; the embedded extension package
  version is read from `extension/manifest.json` (currently `1.2.0`).

## Dependency pinning

`third_party/nlohmann/json.hpp` is the single-header release of
nlohmann/json **3.11.3**, SHA-256:

```text
9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6
```

The MIT license is vendored beside it. SHA-256 is computed with Windows
BCrypt/CNG (`bcrypt.lib`), not a third-party crypto implementation.
