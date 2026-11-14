#pragma once

#include <filesystem>
#include <string>

namespace notion_kex {

struct ShortcutLocations {
  std::filesystem::path desktop;
  std::filesystem::path startMenu;
  std::filesystem::path taskbar;
  std::filesystem::path startup;
  std::filesystem::path launcherRoot;
  std::filesystem::path notionExe;
  std::filesystem::path launcherExe;
};

ShortcutLocations DefaultShortcutLocations(
    const std::filesystem::path& notionExe,
    const std::filesystem::path& launcherRoot);

struct ShortcutOutcome {
  bool changed = false;
  int updated = 0;
  int created = 0;
  int restored = 0;
  std::string message;
};

// Repoints existing Notion shortcuts at the native launcher, adds a desktop
// shortcut and a sign-in check shortcut, and records every original file so
// uninstall can restore it.
ShortcutOutcome InstallShortcuts(const ShortcutLocations& locations);
ShortcutOutcome UninstallShortcuts(const ShortcutLocations& locations);

// Copies the running executable to <launcherRoot>\Notion KEX.exe. A running
// older launcher is renamed first so the replacement can land.
std::filesystem::path InstallLauncherExecutable(
    const std::filesystem::path& launcherRoot);

// Removes the launcher folder. When the running image lives inside it, a
// detached cleanup copy finishes the removal after this process exits.
void RemoveLauncherFolder(const std::filesystem::path& launcherRoot);

// Entry point for the detached cleanup copy. Waits for parentPid, removes the
// folder and exits.
int RunCleanupProcess(unsigned long parentPid,
                      const std::filesystem::path& launcherRoot);

}  // namespace notion_kex
