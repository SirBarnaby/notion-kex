#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace notion_kex {

struct LauncherOptions {
  std::filesystem::path notionExe;
  std::filesystem::path notionData;
  std::filesystem::path launcherRoot;
  bool noLaunch = false;
};

struct LauncherResult {
  int exitCode = 0;
  std::string message;
};

// Starts Notion normally. Throws when the executable is missing.
void StartNotion(const std::filesystem::path& notionExe);

// Fast-path launch: a size/write-time probe decides whether a full repair is
// needed. Repairs run synchronously with Notion closed. Failures are returned
// rather than thrown so the CLI can show one message box.
LauncherResult RunLauncher(const LauncherOptions& options);

// Hidden watcher started by the zoom payload just before Notion quits. It
// waits for the updater and Notion to finish, then repairs if the patch is
// stale. It never closes Notion and never launches it.
LauncherResult RunWatcher(const LauncherOptions& options);

// Hidden check used by the startup shortcut. It repairs a stale patch when
// Notion is closed and never launches Notion.
LauncherResult RunSignInCheck(const LauncherOptions& options);

void AppendLauncherLog(const std::filesystem::path& launcherRoot,
                       std::string_view text);

}  // namespace notion_kex
