#include "notion_kex/record.hpp"

#include "json_util.hpp"
#include "path_util.hpp"
#include "notion_kex/hash.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

namespace notion_kex {
namespace {

bool IsAsciiHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

char ToLowerAscii(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

bool GetString(const nlohmann::json& object, const char* key, std::string& out,
               std::string& error) {
  const auto iterator = object.find(key);
  if (iterator == object.end() || !iterator->is_string()) {
    error = std::string("Native zoom metadata is missing string field '") + key +
            "'.";
    return false;
  }
  out = iterator->get<std::string>();
  return true;
}

bool TryParseOptionalU64(const nlohmann::json& value, std::uint64_t& out) {
  if (value.is_number_unsigned()) {
    out = value.get<std::uint64_t>();
    return true;
  }
  if (value.is_number_integer()) {
    const std::int64_t number = value.get<std::int64_t>();
    if (number < 0) {
      return false;
    }
    out = static_cast<std::uint64_t>(number);
    return true;
  }
  if (!value.is_string()) {
    return false;
  }
  const std::string text = value.get<std::string>();
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return false;
  }
  out = parsed;
  return true;
}

bool ParseRecordObject(const nlohmann::json& object, NativeRecord& out,
                       std::string& error) {
  if (!object.is_object()) {
    error = "Native zoom metadata is not a JSON object.";
    return false;
  }

  NativeRecord record;
  if (!GetString(object, "asarPath", record.asarPath, error) ||
      !GetString(object, "backupPath", record.backupPath, error) ||
      !GetString(object, "originalHash", record.originalHash, error) ||
      !GetString(object, "patchedHash", record.patchedHash, error) ||
      !GetString(object, "payloadHash", record.payloadHash, error) ||
      !GetString(object, "installedAt", record.installedAt, error)) {
    return false;
  }

  if (!IsValidSha256(record.originalHash) ||
      !IsValidSha256(record.patchedHash) ||
      !IsValidSha256(record.payloadHash)) {
    error = "Invalid native zoom record hash.";
    return false;
  }

  record.originalHash = NormalizeSha256(record.originalHash);
  record.patchedHash = NormalizeSha256(record.patchedHash);
  record.payloadHash = NormalizeSha256(record.payloadHash);

  // Fast-path metadata is optional and deliberately non-fatal: a malformed
  // value only disables the shortcut and falls back to full verification.
  const auto patchedSize = object.find("patchedSize");
  if (patchedSize != object.end() && !patchedSize->is_null()) {
    std::uint64_t value = 0;
    if (TryParseOptionalU64(*patchedSize, value)) {
      record.patchedSize = value;
    }
  }
  const auto patchedWriteTime = object.find("patchedWriteTimeUtc");
  if (patchedWriteTime != object.end() && !patchedWriteTime->is_null()) {
    std::uint64_t value = 0;
    if (TryParseOptionalU64(*patchedWriteTime, value)) {
      record.patchedWriteTimeUtc = value;
    }
  }

  out = std::move(record);
  return true;
}

RecordResult ValidateRecordPaths(const NativeRecord& record,
                                 const RecordPaths& paths) {
  if (!detail::EqualsPath(detail::FromUtf8(record.asarPath),
                          paths.asarPath)) {
    return RecordResult::Failure(
        "The native zoom record belongs to a different Notion installation.");
  }

  const std::filesystem::path backup = detail::FromUtf8(record.backupPath);
  if (backup.empty() || !detail::IsUnderDirectory(backup, paths.recordRoot) ||
      !detail::IsBackupLeafName(backup)) {
    return RecordResult::Failure("Unexpected native zoom backup path.");
  }

  std::string error;
  const detail::PathCheck check = detail::CheckRegularPath(backup, error);
  if (check == detail::PathCheck::ReparsePoint ||
      check == detail::PathCheck::Error) {
    return RecordResult::Failure(error);
  }

  std::string hash;
  const FileHashState state = HashFile(backup, hash, error);
  if (state == FileHashState::Missing) {
    return RecordResult::Failure(
        "The original native zoom backup is missing or changed.");
  }
  if (state == FileHashState::Error) {
    return RecordResult::Failure(error);
  }
  if (hash != record.originalHash) {
    return RecordResult::Failure(
        "The original native zoom backup is missing or changed.");
  }
  return RecordResult::Success();
}

}  // namespace

RecordResult RecordResult::Success() { return {true, {}}; }

RecordResult RecordResult::Failure(std::string message) {
  return {false, std::move(message)};
}

std::filesystem::path RecordPaths::recordPath() const {
  return recordRoot / "native-zoom.json";
}

std::filesystem::path RecordPaths::pendingPath() const {
  return recordRoot / "native-zoom-pending.json";
}

RecordPaths MakeRecordPaths(const std::filesystem::path& notionExe,
                            const std::filesystem::path& notionData) {
  return MakeRecordPathsFromAsar(
      notionExe.parent_path() / "resources" / "app.asar", notionData);
}

RecordPaths MakeRecordPathsFromAsar(const std::filesystem::path& asarPath,
                                    const std::filesystem::path& notionData) {
  RecordPaths paths;
  std::error_code error;
  paths.asarPath = std::filesystem::absolute(asarPath, error);
  if (error) {
    paths.asarPath = asarPath;
  }
  paths.asarPath = paths.asarPath.lexically_normal();

  error.clear();
  paths.recordRoot = std::filesystem::absolute(notionData / "Notion KEX", error);
  if (error) {
    paths.recordRoot = notionData / "Notion KEX";
  }
  paths.recordRoot = paths.recordRoot.lexically_normal();
  return paths;
}

bool IsValidSha256(const std::string& value) {
  if (value.size() != 64) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), IsAsciiHex);
}

std::string NormalizeSha256(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(), ToLowerAscii);
  return result;
}

RecordFileState GetRecordFileState(const std::filesystem::path& path,
                                   std::string& error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND ||
        lastError == ERROR_PATH_NOT_FOUND) {
      return RecordFileState::Missing;
    }
    error = "The native zoom metadata could not be inspected: " +
            detail::Win32Error(lastError);
    return RecordFileState::Error;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    error = "The native zoom metadata path is a directory.";
    return RecordFileState::Error;
  }
  return RecordFileState::Present;
}

RecordResult ReadNativeRecord(const std::filesystem::path& path,
                              NativeRecord& out) {
  nlohmann::json json;
  std::string error;
  if (!detail::ReadBoundedJsonFile(path, json, error)) {
    return RecordResult::Failure(std::move(error));
  }
  if (!ParseRecordObject(json, out, error)) {
    return RecordResult::Failure(std::move(error));
  }
  return RecordResult::Success();
}

RecordResult ReadPendingTransaction(const std::filesystem::path& path,
                                    PendingTransaction& out) {
  nlohmann::json json;
  std::string error;
  if (!detail::ReadBoundedJsonFile(path, json, error)) {
    return RecordResult::Failure(std::move(error));
  }
  if (!json.is_object()) {
    return RecordResult::Failure(
        "Native zoom metadata is not a JSON object.");
  }

  PendingTransaction pending;
  if (!GetString(json, "asarPath", pending.asarPath, error) ||
      !GetString(json, "previousHash", pending.previousHash, error) ||
      !GetString(json, "nextHash", pending.nextHash, error) ||
      !GetString(json, "rollbackPath", pending.rollbackPath, error)) {
    return RecordResult::Failure(std::move(error));
  }

  if (!IsValidSha256(pending.previousHash) ||
      !IsValidSha256(pending.nextHash)) {
    return RecordResult::Failure("Invalid pending native zoom transaction.");
  }
  pending.previousHash = NormalizeSha256(pending.previousHash);
  pending.nextHash = NormalizeSha256(pending.nextHash);

  const auto newRecord = json.find("newRecord");
  if (newRecord == json.end() ||
      !ParseRecordObject(*newRecord, pending.newRecord, error)) {
    if (error.empty()) {
      error = "Invalid pending native zoom transaction.";
    }
    return RecordResult::Failure(std::move(error));
  }

  const auto previousRecord = json.find("previousRecord");
  if (previousRecord != json.end() && !previousRecord->is_null()) {
    NativeRecord previous;
    if (!ParseRecordObject(*previousRecord, previous, error)) {
      return RecordResult::Failure(std::move(error));
    }
    pending.previousRecord = std::move(previous);
  }

  out = std::move(pending);
  return RecordResult::Success();
}

RecordResult ValidateNativeRecord(const NativeRecord& record,
                                  const RecordPaths& paths) {
  return ValidateRecordPaths(record, paths);
}

RecordResult ValidatePendingTransaction(const PendingTransaction& pending,
                                        const RecordPaths& paths) {
  if (!detail::EqualsPath(detail::FromUtf8(pending.asarPath),
                          paths.asarPath)) {
    return RecordResult::Failure(
        "The pending native zoom transaction belongs to a different Notion "
        "installation.");
  }
  if (pending.newRecord.patchedHash != pending.nextHash) {
    return RecordResult::Failure("Invalid pending native zoom transaction.");
  }

  RecordResult result = ValidateNativeRecord(pending.newRecord, paths);
  if (!result.ok) {
    return result;
  }
  if (pending.previousRecord.has_value()) {
    result = ValidateNativeRecord(*pending.previousRecord, paths);
    if (!result.ok) {
      return result;
    }
  }

  const std::filesystem::path rollback =
      detail::FromUtf8(pending.rollbackPath);
  if (rollback.empty() ||
      !detail::IsUnderDirectory(rollback, paths.recordRoot) ||
      !detail::IsRollbackLeafName(rollback)) {
    return RecordResult::Failure("Unexpected native rollback path.");
  }

  std::string error;
  const detail::PathCheck check = detail::CheckRegularPath(rollback, error);
  if (check == detail::PathCheck::ReparsePoint ||
      check == detail::PathCheck::Error) {
    return RecordResult::Failure(error);
  }

  std::string hash;
  const FileHashState state = HashFile(rollback, hash, error);
  if (state == FileHashState::Missing) {
    return RecordResult::Failure(
        "The native rollback backup is missing or changed.");
  }
  if (state == FileHashState::Error) {
    return RecordResult::Failure(error);
  }
  if (hash != pending.previousHash) {
    return RecordResult::Failure(
        "The native rollback backup is missing or changed.");
  }
  return RecordResult::Success();
}

}  // namespace notion_kex
