#include "notion_kex/launcher.hpp"

#include "file_util.hpp"
#include "notion_kex/native_zoom.hpp"
#include "notion_kex/record.hpp"
#include "notion_kex/status.hpp"
#include "path_util.hpp"
#include "process.hpp"

#include <windows.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace notion_kex {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kQuitForUpdateMessage =
    "Quit Notion from its tray menu, then open Notion again. Its update "
    "replaced the zoom patch.";
constexpr std::string_view kRepairVerificationMessage =
    "Zoom repair verification failed.";
constexpr std::string_view kNotionMissingMessage =
    "Notion was not found. Install the official Windows desktop app first, or "
    "pass --notion-exe.";

fs::path InstallationRecord(const LauncherOptions& options) {
  return options.notionData / "Notion KEX" / "install.json";
}

void LaunchNotion(const LauncherOptions& options) {
  StartNotion(options.notionExe);
}

LauncherResult Failure(const LauncherOptions& options,
                       const std::exception& exception) {
  AppendLauncherLog(options.launcherRoot, exception.what());
  return LauncherResult{1, exception.what()};
}

}  // namespace

void StartNotion(const fs::path& notionExe) {
  if (!detail::FileExists(notionExe)) {
    throw std::runtime_error(std::string(kNotionMissingMessage));
  }
  detail::LaunchProcess(notionExe, L"", notionExe.parent_path());
}

void AppendLauncherLog(const fs::path& launcherRoot, std::string_view text) {
  if (launcherRoot.empty()) {
    return;
  }
  try {
    detail::CreateDirectories(launcherRoot);
    const fs::path logPath = launcherRoot / "launcher.log";
    const detail::FileInfo info = detail::GetFileInfo(logPath);
    if (info.exists && info.size > 256ull * 1024ull) {
      const fs::path oldPath = launcherRoot / "launcher.old.log";
      detail::RemoveFile(oldPath);
      try {
        detail::MoveFileTo(logPath, oldPath);
      } catch (...) {
        detail::RemoveFile(logPath);
      }
    }

    const std::string line =
        detail::UtcNowIso() + " " + std::string(text) + "\r\n";
    HANDLE handle = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return;
    }
    LARGE_INTEGER zero = {};
    SetFilePointerEx(handle, zero, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written,
              nullptr);
    CloseHandle(handle);
  } catch (...) {
  }
}

LauncherResult RunLauncher(const LauncherOptions& options) {
  try {
    if (!detail::FileExists(InstallationRecord(options))) {
      if (!options.noLaunch) {
        LaunchNotion(options);
        return LauncherResult{
            0, "Notion KEX is not installed; Notion was started normally."};
      }
      return LauncherResult{0, "Notion KEX is not installed."};
    }

    const RecordPaths paths =
        MakeRecordPaths(options.notionExe, options.notionData);
    const QuickStatus quick = QuickInspect(paths);
    if (quick.installed) {
      if (!options.noLaunch) {
        LaunchNotion(options);
        return LauncherResult{0, "Native zoom is current; Notion started."};
      }
      return LauncherResult{0, "Native zoom is current."};
    }

    if (detail::IsNotionRunning(options.notionExe)) {
      throw std::runtime_error(std::string(kQuitForUpdateMessage));
    }

    InstallNativeZoom(paths, options.notionExe);
    const StatusResult status = Inspect(paths);
    if (!status.installed) {
      throw std::runtime_error(std::string(kRepairVerificationMessage));
    }

    if (!options.noLaunch) {
      LaunchNotion(options);
      return LauncherResult{0, "Native zoom repaired; Notion started."};
    }
    return LauncherResult{0, "Native zoom repaired."};
  } catch (const std::exception& exception) {
    return Failure(options, exception);
  }
}

LauncherResult RunWatcher(const LauncherOptions& options) {
  try {
    if (!detail::FileExists(InstallationRecord(options))) {
      return LauncherResult{0, "Notion KEX is not installed."};
    }

    const RecordPaths paths =
        MakeRecordPaths(options.notionExe, options.notionData);
    std::optional<detail::MutexGuard> guard;
    try {
      guard.emplace(
          detail::MakeMutexName("Local\\NotionKexWatch", paths.asarPath),
          "Another Notion KEX watcher is already running.");
    } catch (const std::exception&) {
      return LauncherResult{0, "Another watcher is already running."};
    }
    (void)guard;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(30);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!detail::IsAnyNotionRunning() && !detail::IsUpdaterRunning()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (detail::IsAnyNotionRunning() || detail::IsUpdaterRunning()) {
      AppendLauncherLog(options.launcherRoot,
                        "Watcher timed out waiting for Notion to close.");
      return LauncherResult{
          0, "Watcher timed out; the next launch or sign-in check will "
             "repair."};
    }

    // Let the updater release its file handles before touching the archive.
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    const QuickStatus quick = QuickInspect(paths);
    if (quick.installed) {
      return LauncherResult{0, "Native zoom is current."};
    }

    InstallNativeZoom(paths, options.notionExe);
    AppendLauncherLog(options.launcherRoot,
                      "Native zoom repaired after Notion closed.");
    return LauncherResult{0, "Native zoom repaired after Notion closed."};
  } catch (const std::exception& exception) {
    return Failure(options, exception);
  }
}

LauncherResult RunSignInCheck(const LauncherOptions& options) {
  try {
    if (!detail::FileExists(InstallationRecord(options))) {
      return LauncherResult{0, "Notion KEX is not installed."};
    }
    if (detail::IsAnyNotionRunning() || detail::IsUpdaterRunning()) {
      return LauncherResult{
          0, "Notion is running; native zoom repair is deferred."};
    }

    const RecordPaths paths =
        MakeRecordPaths(options.notionExe, options.notionData);
    const QuickStatus quick = QuickInspect(paths);
    if (quick.installed) {
      return LauncherResult{0, "Native zoom is current."};
    }

    InstallNativeZoom(paths, options.notionExe);
    AppendLauncherLog(options.launcherRoot,
                      "Native zoom repaired during the sign-in check.");
    return LauncherResult{0,
                          "Native zoom repaired during the sign-in check."};
  } catch (const std::exception& exception) {
    return Failure(options, exception);
  }
}

}  // namespace notion_kex
