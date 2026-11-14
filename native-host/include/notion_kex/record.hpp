#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace notion_kex {

// Paths that define the on-disk native zoom contract. They mirror the
// PowerShell implementation in tools/NativeZoom.ps1.
struct RecordPaths {
  std::filesystem::path asarPath;
  std::filesystem::path recordRoot;

  std::filesystem::path recordPath() const;
  std::filesystem::path pendingPath() const;
};

RecordPaths MakeRecordPaths(const std::filesystem::path& notionExe,
                            const std::filesystem::path& notionData);
RecordPaths MakeRecordPathsFromAsar(const std::filesystem::path& asarPath,
                                    const std::filesystem::path& notionData);

// The committed record written by tools/NativeZoom.ps1 to native-zoom.json.
struct NativeRecord {
  std::string asarPath;
  std::string backupPath;
  std::string originalHash;
  std::string patchedHash;
  std::string payloadHash;
  std::string installedAt;

  // Optional fast-path metadata written by the native host. PowerShell ignores
  // extra fields, so records remain interchangeable in both directions.
  std::optional<std::uint64_t> patchedSize;
  std::optional<std::uint64_t> patchedWriteTimeUtc;
};

// The interruption journal written to native-zoom-pending.json.
struct PendingTransaction {
  std::string asarPath;
  std::string previousHash;
  std::string nextHash;
  std::string rollbackPath;
  std::optional<NativeRecord> previousRecord;
  NativeRecord newRecord;
};

enum class RecordFileState {
  Missing,
  Present,
  Error,
};

struct RecordResult {
  bool ok = false;
  std::string error;

  static RecordResult Success();
  static RecordResult Failure(std::string message);
};

bool IsValidSha256(const std::string& value);
std::string NormalizeSha256(const std::string& value);

// Reports whether a record or journal exists. Missing is not an error.
RecordFileState GetRecordFileState(const std::filesystem::path& path,
                                   std::string& error);

// Reads and schema-checks a record or journal. These functions do not mutate
// the filesystem and do not validate the referenced backup contents.
RecordResult ReadNativeRecord(const std::filesystem::path& path,
                              NativeRecord& out);
RecordResult ReadPendingTransaction(const std::filesystem::path& path,
                                    PendingTransaction& out);

// Validates path ownership, backup location, backup existence and hashes.
// A pending transaction is validated without committing or cleaning it up.
RecordResult ValidateNativeRecord(const NativeRecord& record,
                                  const RecordPaths& paths);
RecordResult ValidatePendingTransaction(const PendingTransaction& pending,
                                        const RecordPaths& paths);

}  // namespace notion_kex
