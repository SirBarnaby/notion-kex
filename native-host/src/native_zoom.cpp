#include "notion_kex/native_zoom.hpp"

#include "file_util.hpp"
#include "notion_kex/asar.hpp"
#include "notion_kex/embedded.hpp"
#include "notion_kex/hash.hpp"
#include "path_util.hpp"
#include "process.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace notion_kex {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kClosedMessage =
    "Quit Notion completely from its tray menu before changing its native "
    "pinch support.";
constexpr std::string_view kBusyMessage =
    "Another native zoom installation is running.";
constexpr std::string_view kAlreadyMatchesMessage =
    "Native touchpad zoom already matches this package.";
constexpr std::string_view kInstalledMessage =
    "Native Chromium touchpad zoom installed with a verified original archive "
    "backup.";
constexpr std::string_view kSupportedMessage =
    "Native touchpad zoom is supported by this app archive.";

struct MemberSpec {
  const char* path;
  const char* payloadPath;
  const char* marker;
  std::array<const char*, 3> contracts;
  std::size_t contractCount;
};

constexpr std::array<MemberSpec, 2> kMembers = {{
    {".webpack/renderer/tab_browser_view/preload.js",
     "native/pinch.js",
     "/* Notion KEX native pinch v1 */",
     {"contextBridge", "__electronApi", "__isElectron"},
     3},
    {".webpack/main/index.js",
     "native/pinch-main.js",
     "/* Notion KEX native pinch reset v1 */",
     {"require(\"electron\")", nullptr, nullptr},
     1},
}};

std::vector<std::string> MemberPaths() {
  std::vector<std::string> paths;
  paths.reserve(kMembers.size());
  for (const MemberSpec& member : kMembers) {
    paths.emplace_back(member.path);
  }
  return paths;
}

std::string HashOrThrow(const fs::path& path, std::string_view label) {
  std::string hash;
  std::string error;
  const FileHashState state = HashFile(path, hash, error);
  if (state == FileHashState::Missing) {
    throw std::runtime_error(std::string(label) + " is missing.");
  }
  if (state == FileHashState::Error) {
    throw std::runtime_error(error);
  }
  return hash;
}

std::optional<std::string> OptionalHash(const fs::path& path) {
  std::string hash;
  std::string error;
  const FileHashState state = HashFile(path, hash, error);
  if (state == FileHashState::Missing) {
    return std::nullopt;
  }
  if (state == FileHashState::Error) {
    throw std::runtime_error(error);
  }
  return hash;
}

nlohmann::ordered_json RecordToJson(const NativeRecord& record) {
  nlohmann::ordered_json json;
  json["asarPath"] = record.asarPath;
  json["backupPath"] = record.backupPath;
  json["originalHash"] = record.originalHash;
  json["patchedHash"] = record.patchedHash;
  json["payloadHash"] = record.payloadHash;
  json["installedAt"] = record.installedAt;
  if (record.patchedSize.has_value()) {
    json["patchedSize"] = *record.patchedSize;
  }
  if (record.patchedWriteTimeUtc.has_value()) {
    json["patchedWriteTimeUtc"] = *record.patchedWriteTimeUtc;
  }
  return json;
}

void WriteRecordFile(const fs::path& path, const NativeRecord& record) {
  detail::WriteJsonAtomic(path, RecordToJson(record));
}

std::optional<NativeRecord> ReadValidRecord(const RecordPaths& paths) {
  std::string error;
  const RecordFileState state = GetRecordFileState(paths.recordPath(), error);
  if (state == RecordFileState::Missing) {
    return std::nullopt;
  }
  if (state == RecordFileState::Error) {
    throw std::runtime_error(error);
  }

  NativeRecord record;
  const RecordResult parsed = ReadNativeRecord(paths.recordPath(), record);
  if (!parsed.ok) {
    throw std::runtime_error(parsed.error);
  }
  const RecordResult valid = ValidateNativeRecord(record, paths);
  if (!valid.ok) {
    throw std::runtime_error(valid.error);
  }
  return record;
}

// Mirrors Recover-KexNativeTransaction in tools/NativeZoom.ps1. The pending
// journal always wins; a conflicting archive change retains every piece of
// evidence and refuses to guess.
void RecoverTransaction(const RecordPaths& paths) {
  std::string error;
  const RecordFileState state = GetRecordFileState(paths.pendingPath(), error);
  if (state == RecordFileState::Missing) {
    return;
  }
  if (state == RecordFileState::Error) {
    throw std::runtime_error(error);
  }

  PendingTransaction pending;
  const RecordResult parsed =
      ReadPendingTransaction(paths.pendingPath(), pending);
  if (!parsed.ok) {
    throw std::runtime_error(parsed.error);
  }
  if (!detail::EqualsPath(detail::FromUtf8(pending.asarPath), paths.asarPath)) {
    throw std::runtime_error("Invalid pending native zoom transaction.");
  }
  const RecordResult valid = ValidatePendingTransaction(pending, paths);
  if (!valid.ok) {
    throw std::runtime_error(valid.error);
  }

  const std::optional<std::string> actual = OptionalHash(paths.asarPath);
  if (actual.has_value() && *actual == pending.nextHash) {
    WriteRecordFile(paths.recordPath(), pending.newRecord);
  } else if (actual.has_value() && *actual == pending.previousHash) {
    if (pending.previousRecord.has_value()) {
      WriteRecordFile(paths.recordPath(), *pending.previousRecord);
    } else {
      detail::RemoveFile(paths.recordPath());
    }
  } else {
    throw std::runtime_error(
        "The app changed during an interrupted native install. Its files and "
        "recovery backups were retained.");
  }

  detail::RemoveFile(paths.pendingPath());
  detail::RemoveFile(detail::FromUtf8(pending.rollbackPath));
}

struct PreparedInstall {
  AsarArchive archive;
  std::vector<std::vector<std::uint8_t>> replacements;
  std::optional<NativeRecord> record;
  std::optional<std::string> currentHash;
  std::string payloadHash;
};

std::string PayloadHash() {
  return embedded::PayloadHash();
}

const embedded::File& EmbeddedPayload(const MemberSpec& member) {
  const embedded::File* file = embedded::Find(member.payloadPath);
  if (file == nullptr) {
    throw std::runtime_error("The embedded native payload is incomplete.");
  }
  return *file;
}

std::string MemberText(const std::vector<std::uint8_t>& bytes,
                       const std::string& memberPath) {
  if (!detail::IsValidUtf8(bytes)) {
    throw std::runtime_error("Notion's native entry changed (" + memberPath +
                             "). No native changes made.");
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

PreparedInstall PrepareInstall(const RecordPaths& paths,
                               const fs::path& notionExe) {
  detail::AssertNotionClosed(notionExe, kClosedMessage);
  detail::AssertElectronFuse(notionExe);

  PreparedInstall prepared;
  prepared.record = ReadValidRecord(paths);
  prepared.currentHash = OptionalHash(paths.asarPath);
  prepared.payloadHash = PayloadHash();

  if (prepared.record.has_value() && prepared.currentHash.has_value() &&
      *prepared.currentHash == prepared.record->patchedHash &&
      prepared.payloadHash == prepared.record->payloadHash) {
    return prepared;
  }

  const bool alreadyPatched =
      prepared.record.has_value() && prepared.currentHash.has_value() &&
      *prepared.currentHash == prepared.record->patchedHash;
  const fs::path basePath =
      alreadyPatched ? detail::FromUtf8(prepared.record->backupPath)
                     : paths.asarPath;
  prepared.archive = ParseAsarFile(basePath);

  prepared.replacements.reserve(kMembers.size());
  for (const MemberSpec& member : kMembers) {
    const std::vector<std::uint8_t> original =
        GetAsarMember(prepared.archive, member.path);
    const std::string source = MemberText(original, member.path);
    if (source.find(member.marker) != std::string::npos) {
      throw std::runtime_error(
          "An unrecorded native zoom patch exists. It will not be "
          "overwritten.");
    }
    for (std::size_t index = 0; index < member.contractCount; ++index) {
      const std::string contract = member.contracts[index];
      if (source.find(contract) == std::string::npos) {
        throw std::runtime_error(
            "Notion's native entry changed (" + std::string(member.path) +
            ", " + contract + "). No native changes made.");
      }
    }

    const embedded::File& payload = EmbeddedPayload(member);
    std::vector<std::uint8_t> replacement;
    replacement.reserve(original.size() + 1 + payload.size);
    replacement.insert(replacement.end(), original.begin(), original.end());
    replacement.push_back(0x0A);
    replacement.insert(replacement.end(), payload.data,
                       payload.data + payload.size);
    prepared.replacements.push_back(std::move(replacement));
  }
  return prepared;
}

bool AlreadyMatches(const PreparedInstall& prepared) {
  return prepared.record.has_value() && prepared.currentHash.has_value() &&
         *prepared.currentHash == prepared.record->patchedHash &&
         prepared.payloadHash == prepared.record->payloadHash;
}

class TempPathGuard {
 public:
  explicit TempPathGuard(fs::path path) : path_(std::move(path)) {}
  ~TempPathGuard() {
    if (!path_.empty()) {
      try {
        detail::RemoveFile(path_);
      } catch (...) {
      }
    }
  }
  TempPathGuard(const TempPathGuard&) = delete;
  TempPathGuard& operator=(const TempPathGuard&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

fs::path MakeSiblingTemp(const fs::path& target) {
  return fs::path(target.wstring() + L".notion-kex-" +
                  detail::FromUtf8(detail::NewGuidString()).wstring() +
                  L".tmp");
}

void WritePendingFile(const RecordPaths& paths,
                      const std::optional<std::string>& previousHash,
                      const std::optional<NativeRecord>& previousRecord,
                      const NativeRecord& newRecord,
                      const fs::path& rollbackPath) {
  nlohmann::ordered_json pending;
  pending["asarPath"] = detail::ToUtf8(paths.asarPath);
  if (previousHash.has_value()) {
    pending["previousHash"] = *previousHash;
  } else {
    pending["previousHash"] = nullptr;
  }
  pending["nextHash"] = newRecord.patchedHash;
  pending["rollbackPath"] = detail::ToUtf8(rollbackPath);
  if (previousRecord.has_value()) {
    pending["previousRecord"] = RecordToJson(*previousRecord);
  } else {
    pending["previousRecord"] = nullptr;
  }
  pending["newRecord"] = RecordToJson(newRecord);
  detail::WriteJsonAtomic(paths.pendingPath(), pending);
}

NativeZoomOutcome InstallPrepared(const RecordPaths& paths,
                                  const fs::path& notionExe,
                                  PreparedInstall prepared) {
  TempPathGuard temp(MakeSiblingTemp(paths.asarPath));

  AsarArchive after = prepared.archive;
  for (std::size_t index = 0; index < kMembers.size(); ++index) {
    const std::vector<std::uint8_t> candidate = SetAsarMember(
        after, kMembers[index].path, prepared.replacements[index]);
    detail::WriteFileBytes(temp.path(), candidate);
    after = ParseAsarFile(temp.path());
  }

  AssertAsarEdits(prepared.archive, after, MemberPaths());
  for (std::size_t index = 0; index < kMembers.size(); ++index) {
    if (AsarMemberHash(after, kMembers[index].path) !=
        detail::HashBytesVector(prepared.replacements[index])) {
      throw std::runtime_error("Native entry verification failed.");
    }
  }

  detail::CreateDirectories(paths.recordRoot);

  const bool baseIsArchive =
      !prepared.record.has_value() || !prepared.currentHash.has_value() ||
      *prepared.currentHash != prepared.record->patchedHash;
  fs::path backupPath;
  if (baseIsArchive) {
    backupPath =
        paths.recordRoot /
        detail::FromUtf8("app-asar-before-native-" + detail::NewGuidString() +
                         ".bak");
    detail::CopyFileTo(paths.asarPath, backupPath);
  } else {
    backupPath = detail::FromUtf8(prepared.record->backupPath);
  }

  NativeRecord newRecord;
  newRecord.asarPath = detail::ToUtf8(paths.asarPath);
  newRecord.backupPath = detail::ToUtf8(backupPath);
  newRecord.originalHash =
      HashOrThrow(backupPath, "The original native zoom backup");
  newRecord.patchedHash = HashOrThrow(temp.path(), "The patched app archive");
  newRecord.payloadHash = prepared.payloadHash;
  newRecord.installedAt = detail::UtcNowIso();

  const detail::FileInfo patchedInfo = detail::GetFileInfo(temp.path());
  if (patchedInfo.exists) {
    newRecord.patchedSize = patchedInfo.size;
    newRecord.patchedWriteTimeUtc = patchedInfo.writeTime;
  }

  if (newRecord.originalHash != detail::HashBytesVector(prepared.archive.bytes)) {
    throw std::runtime_error(
        "The original app changed while its backup was being created.");
  }

  const std::vector<std::uint8_t> rollback =
      detail::ReadFileBytes(paths.asarPath, detail::kMaxAsarBytes,
                            "The app archive");
  const fs::path rollbackPath =
      paths.recordRoot /
      detail::FromUtf8("native-rollback-" + detail::NewGuidString() + ".bak");
  detail::WriteFileBytes(rollbackPath, rollback);

  WritePendingFile(paths, prepared.currentHash, prepared.record, newRecord,
                   rollbackPath);

  try {
    if (!prepared.currentHash.has_value() ||
        HashOrThrow(paths.asarPath, "The app archive") !=
            *prepared.currentHash) {
      throw std::runtime_error(
          "The app changed while native zoom was being prepared.");
    }
    detail::AssertNotionClosed(notionExe, kClosedMessage);
    detail::ReplaceFileWith(temp.path(), paths.asarPath);
    // ReplaceFile preserves the destination's timestamps, so the fast-path
    // metadata must be captured from the archive after the swap.
    const detail::FileInfo installedInfo = detail::GetFileInfo(paths.asarPath);
    if (installedInfo.exists) {
      newRecord.patchedSize = installedInfo.size;
      newRecord.patchedWriteTimeUtc = installedInfo.writeTime;
    }
    // Keep the journal consistent with the post-replacement metadata so
    // recovery cannot reintroduce stale timestamps.
    WritePendingFile(paths, prepared.currentHash, prepared.record, newRecord,
                     rollbackPath);
    WriteRecordFile(paths.recordPath(), newRecord);
  } catch (...) {
    // If the swap landed but the record write failed, restore the archive
    // before recovering so the journal can resolve cleanly.
    try {
      const std::optional<std::string> current = OptionalHash(paths.asarPath);
      if (current.has_value() && *current == newRecord.patchedHash) {
        detail::WriteFileBytes(temp.path(), rollback);
        detail::ReplaceFileWith(temp.path(), paths.asarPath);
      }
    } catch (...) {
    }
    RecoverTransaction(paths);
    throw;
  }

  RecoverTransaction(paths);
  NativeZoomOutcome outcome;
  outcome.installed = true;
  outcome.changed = true;
  outcome.message = std::string(kInstalledMessage);
  return outcome;
}

}  // namespace

QuickStatus QuickInspect(const RecordPaths& paths) {
  QuickStatus result;

  std::string error;
  if (detail::CheckRegularPath(paths.asarPath, error) != detail::PathCheck::Ok) {
    result.detail = error;
    return result;
  }
  error.clear();
  if (detail::CheckRegularPath(paths.recordRoot, error) !=
      detail::PathCheck::Ok) {
    result.detail = error;
    return result;
  }

  error.clear();
  const RecordFileState pendingState =
      GetRecordFileState(paths.pendingPath(), error);
  if (pendingState == RecordFileState::Error) {
    result.detail = error;
    return result;
  }
  if (pendingState == RecordFileState::Present) {
    result.recordExists = true;
    result.installed = false;
    result.fastPath = false;
    result.message =
        "Native touchpad zoom: an interrupted install requires recovery.";
    return result;
  }

  error.clear();
  const RecordFileState recordState =
      GetRecordFileState(paths.recordPath(), error);
  if (recordState == RecordFileState::Error) {
    result.detail = error;
    return result;
  }
  if (recordState == RecordFileState::Missing) {
    result.message = "Native touchpad zoom: not installed.";
    return result;
  }
  result.recordExists = true;

  NativeRecord record;
  const RecordResult parsed = ReadNativeRecord(paths.recordPath(), record);
  if (!parsed.ok) {
    result.detail = parsed.error;
    return result;
  }
  if (!detail::EqualsPath(detail::FromUtf8(record.asarPath), paths.asarPath)) {
    result.detail =
        "The native zoom record belongs to a different Notion installation.";
    return result;
  }
  if (!IsValidSha256(record.originalHash) ||
      !IsValidSha256(record.patchedHash) ||
      !IsValidSha256(record.payloadHash)) {
    result.detail = "Invalid native zoom record hash.";
    return result;
  }

  const fs::path backup = detail::FromUtf8(record.backupPath);
  if (backup.empty() || !detail::IsUnderDirectory(backup, paths.recordRoot) ||
      !detail::IsBackupLeafName(backup)) {
    result.detail = "Unexpected native zoom backup path.";
    return result;
  }
  error.clear();
  if (detail::CheckRegularPath(backup, error) != detail::PathCheck::Ok) {
    result.detail = error;
    return result;
  }
  if (!detail::FileExists(backup)) {
    result.detail = "The original native zoom backup is missing or changed.";
    return result;
  }

  if (!record.patchedSize.has_value() ||
      !record.patchedWriteTimeUtc.has_value()) {
    result.message = "Native touchpad zoom: full verification required.";
    return result;
  }

  result.fastPath = true;
  const detail::FileInfo info = detail::GetFileInfo(paths.asarPath);
  if (!info.exists) {
    result.message =
        "Native touchpad zoom: app changed; rerun Install.cmd with Notion "
        "closed.";
    return result;
  }

  const bool archiveMatches = info.size == *record.patchedSize &&
                              info.writeTime == *record.patchedWriteTimeUtc;
  const bool payloadMatches = record.payloadHash == PayloadHash();
  result.installed = archiveMatches && payloadMatches;
  if (result.installed) {
    result.message = "Native touchpad zoom: installed (100%-500%).";
  } else if (archiveMatches) {
    result.message =
        "Native touchpad zoom: the launcher will refresh the native patch.";
  } else {
    result.message =
        "Native touchpad zoom: app changed; rerun Install.cmd with Notion "
        "closed.";
  }
  return result;
}

void RecoverNativeZoom(const RecordPaths& paths) {
  std::string error;
  if (detail::CheckRegularPath(paths.asarPath, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  error.clear();
  if (detail::CheckRegularPath(paths.recordRoot, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  detail::MutexGuard guard(
      detail::MakeMutexName("Local\\NotionKexNativeZoom", paths.asarPath),
      std::string(kBusyMessage));
  RecoverTransaction(paths);
}

NativeZoomOutcome InstallNativeZoom(const RecordPaths& paths,
                                    const fs::path& notionExe) {
  std::string error;
  if (detail::CheckRegularPath(paths.asarPath, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  error.clear();
  if (detail::CheckRegularPath(paths.recordRoot, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }

  detail::MutexGuard guard(
      detail::MakeMutexName("Local\\NotionKexNativeZoom", paths.asarPath),
      std::string(kBusyMessage));
  RecoverTransaction(paths);

  PreparedInstall prepared = PrepareInstall(paths, notionExe);
  if (AlreadyMatches(prepared)) {
    // The archive and payload are verified, so any missing or stale fast-path
    // metadata can be refreshed in place. Records written by the PowerShell
    // installer lack these fields entirely.
    if (prepared.record.has_value()) {
      NativeRecord updated = *prepared.record;
      const detail::FileInfo info = detail::GetFileInfo(paths.asarPath);
      if (info.exists &&
          (updated.patchedSize != info.size ||
           updated.patchedWriteTimeUtc != info.writeTime)) {
        updated.patchedSize = info.size;
        updated.patchedWriteTimeUtc = info.writeTime;
        WriteRecordFile(paths.recordPath(), updated);
      }
    }
    NativeZoomOutcome outcome;
    outcome.installed = true;
    outcome.changed = false;
    outcome.message = std::string(kAlreadyMatchesMessage);
    return outcome;
  }
  return InstallPrepared(paths, notionExe, std::move(prepared));
}

NativeZoomOutcome RepairNativeZoom(const RecordPaths& paths,
                                   const fs::path& notionExe) {
  return InstallNativeZoom(paths, notionExe);
}

NativeZoomOutcome ValidateNativeZoom(const RecordPaths& paths,
                                     const fs::path& notionExe) {
  std::string error;
  if (detail::CheckRegularPath(paths.asarPath, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  error.clear();
  if (detail::CheckRegularPath(paths.recordRoot, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }

  detail::MutexGuard guard(
      detail::MakeMutexName("Local\\NotionKexNativeZoom", paths.asarPath),
      std::string(kBusyMessage));

  PreparedInstall prepared = PrepareInstall(paths, notionExe);
  NativeZoomOutcome outcome;
  if (AlreadyMatches(prepared)) {
    outcome.installed = true;
    outcome.changed = false;
    outcome.message = std::string(kAlreadyMatchesMessage);
    return outcome;
  }
  outcome.installed = false;
  outcome.changed = false;
  outcome.message = std::string(kSupportedMessage);
  return outcome;
}

NativeZoomOutcome UninstallNativeZoom(const RecordPaths& paths,
                                      const fs::path& notionExe) {
  std::string error;
  if (detail::CheckRegularPath(paths.asarPath, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  error.clear();
  if (detail::CheckRegularPath(paths.recordRoot, error) !=
      detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }

  detail::MutexGuard guard(
      detail::MakeMutexName("Local\\NotionKexNativeZoom", paths.asarPath),
      std::string(kBusyMessage));
  RecoverTransaction(paths);
  detail::AssertNotionClosed(notionExe, kClosedMessage);

  const std::optional<NativeRecord> record = ReadValidRecord(paths);
  if (!record.has_value()) {
    NativeZoomOutcome outcome;
    outcome.message = "Native touchpad zoom: no installation record.";
    return outcome;
  }

  const std::optional<std::string> currentHash = OptionalHash(paths.asarPath);
  NativeZoomOutcome outcome;
  if (currentHash.has_value() && *currentHash == record->patchedHash) {
    TempPathGuard temp(MakeSiblingTemp(paths.asarPath));
    detail::CopyFileTo(detail::FromUtf8(record->backupPath), temp.path());
    detail::AssertNotionClosed(notionExe, kClosedMessage);
    if (HashOrThrow(paths.asarPath, "The app archive") != *currentHash) {
      throw std::runtime_error(
          "The app changed while native zoom removal was being prepared.");
    }
    detail::ReplaceFileWith(temp.path(), paths.asarPath);
    if (HashOrThrow(paths.asarPath, "The app archive") !=
        record->originalHash) {
      throw std::runtime_error(
          "Native zoom restoration verification failed.");
    }
    outcome.message =
        "Native touchpad zoom removed; the original app archive is restored.";
  } else {
    if (currentHash.has_value()) {
      const AsarArchive archive = ParseAsarFile(paths.asarPath);
      for (const MemberSpec& member : kMembers) {
        const std::vector<std::uint8_t> bytes =
            GetAsarMember(archive, member.path);
        const std::string source = MemberText(bytes, member.path);
        if (source.find(member.marker) != std::string::npos) {
          throw std::runtime_error(
              "The app was edited after native zoom installation. It will not "
              "be overwritten; the backup and record are retained.");
        }
      }
    }
    outcome.message =
        "A newer or replaced app archive was retained; native zoom backup "
        "remains available.";
  }

  detail::RemoveFile(paths.recordPath());
  return outcome;
}

}  // namespace notion_kex
