#pragma once

#include "notion_kex/record.hpp"

#include <string>

namespace notion_kex {

enum class StatusCode {
  Installed,
  NotInstalled,
  ArchiveChanged,
  InvalidRecord,
  RecoveryRequired,
  InvalidRecovery,
};

const char* StatusCodeName(StatusCode code);

struct StatusResult {
  StatusCode code = StatusCode::NotInstalled;
  bool installed = false;
  std::string message;
  std::string detail;
  RecordPaths paths;
  std::string currentHash;
  std::string expectedHash;
  std::string originalHash;
  std::string backupPath;
  std::string recoveryState;
};

// Read-only status inspection. It never repairs, deletes, writes, launches or
// watches anything. A pending journal is reported as RecoveryRequired and is
// left exactly as found.
StatusResult Inspect(const RecordPaths& paths);

}  // namespace notion_kex
