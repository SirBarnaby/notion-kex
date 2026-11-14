#include "notion_kex/status.hpp"

#include "notion_kex/hash.hpp"
#include "path_util.hpp"

#include <utility>

namespace notion_kex {
namespace {

StatusResult MakeInvalidRecord(StatusResult result, std::string detail) {
  result.code = StatusCode::InvalidRecord;
  result.installed = false;
  result.message =
      "Native touchpad zoom: installation record is invalid; no changes were "
      "made.";
  result.detail = std::move(detail);
  return result;
}

StatusResult MakeInvalidRecovery(StatusResult result, std::string detail) {
  result.code = StatusCode::InvalidRecovery;
  result.installed = false;
  result.message =
      "Native touchpad zoom: recovery material is invalid; no changes were "
      "made.";
  result.detail = std::move(detail);
  return result;
}

}  // namespace

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::Installed:
      return "installed";
    case StatusCode::NotInstalled:
      return "not-installed";
    case StatusCode::ArchiveChanged:
      return "archive-changed";
    case StatusCode::InvalidRecord:
      return "invalid-record";
    case StatusCode::RecoveryRequired:
      return "recovery-required";
    case StatusCode::InvalidRecovery:
      return "invalid-recovery";
  }
  return "unknown";
}

StatusResult Inspect(const RecordPaths& paths) {
  StatusResult result;
  result.paths = paths;

  std::string error;
  const detail::PathCheck asarCheck =
      detail::CheckRegularPath(paths.asarPath, error);
  if (asarCheck == detail::PathCheck::ReparsePoint ||
      asarCheck == detail::PathCheck::Error) {
    return MakeInvalidRecord(std::move(result), std::move(error));
  }

  error.clear();
  const detail::PathCheck rootCheck =
      detail::CheckRegularPath(paths.recordRoot, error);
  if (rootCheck == detail::PathCheck::ReparsePoint ||
      rootCheck == detail::PathCheck::Error) {
    return MakeInvalidRecord(std::move(result), std::move(error));
  }

  // A pending journal always wins over the committed record. The existing
  // PowerShell Status action recovers it; this component deliberately reports
  // it without committing or cleaning anything up.
  error.clear();
  const RecordFileState pendingState =
      GetRecordFileState(paths.pendingPath(), error);
  if (pendingState == RecordFileState::Error) {
    return MakeInvalidRecovery(std::move(result), std::move(error));
  }
  if (pendingState == RecordFileState::Present) {
    PendingTransaction pending;
    RecordResult parsed = ReadPendingTransaction(paths.pendingPath(), pending);
    if (!parsed.ok) {
      return MakeInvalidRecovery(std::move(result), std::move(parsed.error));
    }
    RecordResult valid = ValidatePendingTransaction(pending, paths);
    if (!valid.ok) {
      return MakeInvalidRecovery(std::move(result), std::move(valid.error));
    }

    result.expectedHash = pending.nextHash;
    result.originalHash = pending.newRecord.originalHash;
    result.backupPath = pending.newRecord.backupPath;

    std::string currentHash;
    error.clear();
    const FileHashState hashState =
        HashFile(paths.asarPath, currentHash, error);
    if (hashState == FileHashState::Error) {
      return MakeInvalidRecovery(std::move(result), std::move(error));
    }
    if (hashState == FileHashState::Hashed) {
      result.currentHash = currentHash;
      if (currentHash == pending.nextHash) {
        result.recoveryState = "archive-matches-next";
        result.detail =
            "The archive already matches the pending patched hash; the record "
            "write did not complete.";
      } else if (currentHash == pending.previousHash) {
        result.recoveryState = "archive-matches-previous";
        result.detail =
            "The archive still matches the pending previous hash; the archive "
            "swap did not complete.";
      } else {
        result.recoveryState = "archive-conflict";
        result.detail =
            "The archive changed during the interrupted install; recovery "
            "evidence was retained.";
      }
    } else {
      result.recoveryState = "archive-missing";
      result.detail =
          "The app archive is missing while a pending transaction exists; "
          "recovery evidence was retained.";
    }

    result.code = StatusCode::RecoveryRequired;
    result.installed = false;
    result.message =
        "Native touchpad zoom: an interrupted install requires recovery; no "
        "changes were made.";
    return result;
  }

  error.clear();
  const RecordFileState recordState =
      GetRecordFileState(paths.recordPath(), error);
  if (recordState == RecordFileState::Error) {
    return MakeInvalidRecord(std::move(result), std::move(error));
  }
  if (recordState == RecordFileState::Missing) {
    result.code = StatusCode::NotInstalled;
    result.installed = false;
    result.message = "Native touchpad zoom: not installed.";
    return result;
  }

  NativeRecord record;
  RecordResult parsed = ReadNativeRecord(paths.recordPath(), record);
  if (!parsed.ok) {
    return MakeInvalidRecord(std::move(result), std::move(parsed.error));
  }
  RecordResult valid = ValidateNativeRecord(record, paths);
  if (!valid.ok) {
    return MakeInvalidRecord(std::move(result), std::move(valid.error));
  }

  result.expectedHash = record.patchedHash;
  result.originalHash = record.originalHash;
  result.backupPath = record.backupPath;

  std::string currentHash;
  error.clear();
  const FileHashState hashState = HashFile(paths.asarPath, currentHash, error);
  if (hashState == FileHashState::Error) {
    return MakeInvalidRecord(std::move(result), std::move(error));
  }
  if (hashState == FileHashState::Hashed) {
    result.currentHash = currentHash;
  }

  if (hashState == FileHashState::Hashed &&
      currentHash == record.patchedHash) {
    result.code = StatusCode::Installed;
    result.installed = true;
    result.message = "Native touchpad zoom: installed (100%-500%).";
  } else {
    result.code = StatusCode::ArchiveChanged;
    result.installed = false;
    result.message =
        "Native touchpad zoom: app changed; rerun Install.cmd with Notion "
        "closed.";
  }
  return result;
}

}  // namespace notion_kex
