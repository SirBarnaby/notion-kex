#pragma once

#include "notion_kex/record.hpp"

#include <filesystem>
#include <string>

namespace notion_kex {

struct NativeZoomOutcome {
  bool installed = false;
  bool changed = false;
  std::string message;
};

// A cheap launcher probe. It never hashes the archive: it trusts the optional
// size/write-time fields written by the native host and checks the backup only
// for existence and path ownership. A false result means "run the full
// transaction", not "the app is broken".
struct QuickStatus {
  bool recordExists = false;
  bool installed = false;
  bool fastPath = false;
  std::string message;
  std::string detail;
};

QuickStatus QuickInspect(const RecordPaths& paths);

// Resolves a pending journal under the per-archive mutex. This is the only
// recovery entry point; passive status never calls it.
void RecoverNativeZoom(const RecordPaths& paths);

// Mutating entry points. Each acquires the per-archive mutex and recovers a
// pending journal before doing anything else. Failures throw with a stable
// message; callers decide whether to show a message box.
NativeZoomOutcome InstallNativeZoom(const RecordPaths& paths,
                                    const std::filesystem::path& notionExe);
NativeZoomOutcome UninstallNativeZoom(const RecordPaths& paths,
                                      const std::filesystem::path& notionExe);
NativeZoomOutcome RepairNativeZoom(const RecordPaths& paths,
                                   const std::filesystem::path& notionExe);

// Read-only validation. It does not recover a pending journal and does not
// write, copy or replace anything.
NativeZoomOutcome ValidateNativeZoom(const RecordPaths& paths,
                                     const std::filesystem::path& notionExe);

}  // namespace notion_kex
