#include "notion_kex/hash.hpp"
#include "notion_kex/record.hpp"
#include "notion_kex/status.hpp"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef KEX_STATUS_VERSION
#define KEX_STATUS_VERSION "0.1.0"
#endif
#ifndef KEX_COMPILER_ID
#define KEX_COMPILER_ID "unknown"
#endif
#ifndef KEX_COMPILER_VERSION
#define KEX_COMPILER_VERSION "unknown"
#endif
#ifndef KEX_BUILD_TYPE
#define KEX_BUILD_TYPE "unknown"
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

namespace {

namespace fs = std::filesystem;
using notion_kex::FileHashState;
using notion_kex::HashBytes;
using notion_kex::HashFile;
using notion_kex::Inspect;
using notion_kex::IsValidSha256;
using notion_kex::MakeRecordPaths;
using notion_kex::MakeRecordPathsFromAsar;
using notion_kex::NativeRecord;
using notion_kex::NormalizeSha256;
using notion_kex::PendingTransaction;
using notion_kex::RecordPaths;
using notion_kex::StatusCode;
using notion_kex::StatusCodeName;
using notion_kex::StatusResult;

constexpr std::string_view kPayloadHash =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string Hash(std::string_view bytes) {
  std::string hex;
  std::string error;
  if (!HashBytes(bytes.data(), bytes.size(), hex, error)) {
    throw std::runtime_error("HashBytes failed: " + error);
  }
  return hex;
}

std::string Upper(std::string value) {
  for (char& character : value) {
    if (character >= 'a' && character <= 'f') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return value;
}

void WriteFixtureFile(const fs::path& path, std::string_view bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create directory for " + Utf8(path) +
                             ": " + error.message());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("Could not write " + Utf8(path));
  }
  if (!bytes.empty()) {
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  if (!stream) {
    throw std::runtime_error("Could not finish writing " + Utf8(path));
  }
}

std::string ReadFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Could not read " + Utf8(path));
  }
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

void WriteJson(const fs::path& path, const nlohmann::json& value) {
  WriteFixtureFile(path, value.dump(2) + "\n");
}

nlohmann::json MakeRecord(const fs::path& asarPath, const fs::path& backupPath,
                          const std::string& originalHash,
                          const std::string& patchedHash,
                          const std::string& payloadHash =
                              std::string(kPayloadHash)) {
  return nlohmann::json{
      {"asarPath", Utf8(asarPath)},
      {"backupPath", Utf8(backupPath)},
      {"originalHash", originalHash},
      {"patchedHash", patchedHash},
      {"payloadHash", payloadHash},
      {"installedAt", "2026-11-09T00:00:00.0000000Z"},
  };
}

nlohmann::json MakePending(const fs::path& asarPath,
                           const std::string& previousHash,
                           const std::string& nextHash,
                           const fs::path& rollbackPath,
                           const nlohmann::json& previousRecord,
                           const nlohmann::json& newRecord) {
  return nlohmann::json{
      {"asarPath", Utf8(asarPath)},
      {"previousHash", previousHash},
      {"nextHash", nextHash},
      {"rollbackPath", Utf8(rollbackPath)},
      {"previousRecord", previousRecord},
      {"newRecord", newRecord},
  };
}

std::string IsoNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc = {};
  gmtime_s(&utc, &time);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

void WriteUtf8(HANDLE handle, std::string_view text) {
  if (text.empty()) {
    return;
  }
  const int wideSize =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (wideSize <= 0) {
    return;
  }
  std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), wide.data(), wideSize);

  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) != 0) {
    std::size_t offset = 0;
    while (offset < wide.size()) {
      DWORD written = 0;
      const DWORD request = static_cast<DWORD>(
          std::min<std::size_t>(wide.size() - offset, 8192));
      if (!WriteConsoleW(handle, wide.data() + offset, request, &written,
                         nullptr) ||
          written == 0) {
        break;
      }
      offset += written;
    }
    return;
  }

  const char* bytes = text.data();
  std::size_t offset = 0;
  while (offset < text.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(
        std::min<std::size_t>(text.size() - offset, 1u << 20));
    if (!::WriteFile(handle, bytes + offset, request, &written, nullptr) ||
        written == 0) {
      break;
    }
    offset += written;
  }
}

void Print(std::string_view text) {
  WriteUtf8(GetStdHandle(STD_OUTPUT_HANDLE), text);
}

void PrintError(std::string_view text) {
  WriteUtf8(GetStdHandle(STD_ERROR_HANDLE), text);
}

struct SkipCase {
  std::string reason;
};

struct Assertion {
  std::string name;
  bool passed = false;
  std::string detail;
};

struct CaseResult {
  std::string name;
  bool passed = true;
  bool skipped = false;
  bool passive = true;
  std::string skipReason;
  std::string passivityDetail;
  int snapshotEntries = 0;
  std::vector<Assertion> assertions;

  void Check(bool condition, std::string assertionName,
             std::string detail = {}) {
    if (!condition) {
      passed = false;
    }
    assertions.push_back(
        Assertion{std::move(assertionName), condition, std::move(detail)});
  }

  void Equal(const std::string& actual, const std::string& expected,
             std::string assertionName) {
    Check(actual == expected, std::move(assertionName),
          "expected '" + expected + "', got '" + actual + "'");
  }

  void Contains(const std::string& text, const std::string& needle,
                std::string assertionName) {
    Check(text.find(needle) != std::string::npos, std::move(assertionName),
          "expected to contain '" + needle + "', got '" + text + "'");
  }
};

struct SnapshotEntry {
  std::string path;
  std::string type;
  unsigned long long size = 0;
  long long modified = 0;
  std::string hash;

  bool operator==(const SnapshotEntry&) const = default;
};

bool IsReparse(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

SnapshotEntry MakeSnapshotEntry(const fs::path& root, const fs::path& path,
                                const fs::file_status& status) {
  SnapshotEntry entry;
  const fs::path relative = path.lexically_relative(root);
  entry.path = relative.empty() ? "." : Utf8(relative);

  if (IsReparse(path)) {
    entry.type = "reparse";
  } else if (fs::is_directory(status)) {
    entry.type = "directory";
  } else if (fs::is_regular_file(status)) {
    entry.type = "file";
  } else {
    entry.type = "other";
  }

  std::error_code error;
  if (entry.type == "file") {
    entry.size = fs::file_size(path, error);
    if (error) {
      entry.size = 0;
      error.clear();
    }
    std::string hash;
    std::string hashError;
    if (HashFile(path, hash, hashError) == FileHashState::Hashed) {
      entry.hash = std::move(hash);
    }
  }

  const fs::file_time_type modified = fs::last_write_time(path, error);
  if (!error) {
    entry.modified =
        static_cast<long long>(modified.time_since_epoch().count());
  }
  return entry;
}

std::vector<SnapshotEntry> SnapshotTree(const fs::path& root) {
  std::vector<SnapshotEntry> entries;
  std::error_code error;
  const fs::file_status rootStatus = fs::symlink_status(root, error);
  if (error || !fs::exists(rootStatus)) {
    return entries;
  }
  entries.push_back(MakeSnapshotEntry(root, root, rootStatus));

  fs::recursive_directory_iterator iterator(
      root, fs::directory_options::skip_permission_denied, error);
  if (error) {
    throw std::runtime_error("Could not snapshot " + Utf8(root) + ": " +
                             error.message());
  }
  const fs::recursive_directory_iterator end;
  while (iterator != end) {
    const fs::directory_entry& entry = *iterator;
    std::error_code statusError;
    const fs::file_status status = entry.symlink_status(statusError);
    if (!statusError) {
      entries.push_back(MakeSnapshotEntry(root, entry.path(), status));
      if (IsReparse(entry.path()) && fs::is_directory(status)) {
        iterator.disable_recursion_pending();
      }
    }
    iterator.increment(error);
    if (error) {
      throw std::runtime_error("Could not snapshot " + Utf8(root) + ": " +
                               error.message());
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const SnapshotEntry& left, const SnapshotEntry& right) {
              return left.path < right.path;
            });
  return entries;
}

std::string DescribeEntry(const SnapshotEntry& entry) {
  std::ostringstream output;
  output << entry.path << " (" << entry.type << ", " << entry.size << " bytes";
  if (!entry.hash.empty()) {
    output << ", sha256 " << entry.hash;
  }
  output << ")";
  return output.str();
}

std::string DescribeSnapshotDiff(const std::vector<SnapshotEntry>& before,
                                 const std::vector<SnapshotEntry>& after) {
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < before.size() && right < after.size() &&
         before[left] == after[right]) {
    ++left;
    ++right;
  }
  if (left == before.size() && right == after.size()) {
    return {};
  }

  std::ostringstream output;
  output << "fixture tree changed";
  if (left < before.size()) {
    output << "; before[" << left << "]=" << DescribeEntry(before[left]);
  }
  if (right < after.size()) {
    output << "; after[" << right << "]=" << DescribeEntry(after[right]);
  }
  return output.str();
}

void RemoveTree(const fs::path& root) {
  std::error_code error;
  if (!fs::exists(root, error)) {
    return;
  }

  // Remove junctions and symlinks before remove_all so cleanup can never
  // traverse into a linked fixture directory.
  std::vector<fs::path> reparseDirectories;
  fs::recursive_directory_iterator iterator(
      root, fs::directory_options::skip_permission_denied, error);
  if (!error) {
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
      const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          reparseDirectories.push_back(iterator->path());
        }
        iterator.disable_recursion_pending();
      }
      iterator.increment(error);
      if (error) {
        error.clear();
        break;
      }
    }
  }
  for (const fs::path& directory : reparseDirectories) {
    RemoveDirectoryW(directory.c_str());
  }
  fs::remove_all(root, error);
}

bool CreateJunction(const fs::path& link, const fs::path& target,
                    std::string& error) {
  if (CreateSymbolicLinkW(
          link.c_str(), target.c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
    return true;
  }

  const std::wstring command = L"cmd.exe /d /c mklink /J \"" +
                               link.wstring() + L"\" \"" + target.wstring() +
                               L"\"";
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    error = "Could not start mklink /J.";
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != 0) {
    error = "mklink /J failed with exit code " + std::to_string(exitCode) + ".";
    return false;
  }
  return true;
}

struct Fixture {
  fs::path root;
  fs::path notionDir;
  fs::path notionExe;
  fs::path asar;
  fs::path dataDir;
  fs::path recordRoot;
  RecordPaths paths;

  void Initialize() {
    notionDir = root / "Notion";
    notionExe = notionDir / "Notion.exe";
    asar = notionDir / "resources" / "app.asar";
    dataDir = root / "Data";
    recordRoot = dataDir / "Notion KEX";
    std::error_code error;
    fs::create_directories(asar.parent_path(), error);
    if (error) {
      throw std::runtime_error("Could not create fixture: " + error.message());
    }
    fs::create_directories(dataDir, error);
    if (error) {
      throw std::runtime_error("Could not create fixture: " + error.message());
    }
    WriteFixtureFile(notionExe, "");
    paths = MakeRecordPathsFromAsar(asar, dataDir);
  }

  fs::path BackupPath(
      std::string_view token = "0123456789abcdef0123456789abcdef") const {
    return recordRoot / ("app-asar-before-native-" + std::string(token) +
                         ".bak");
  }

  fs::path RollbackPath(
      std::string_view token = "fedcba9876543210fedcba9876543210") const {
    return recordRoot / ("native-rollback-" + std::string(token) + ".bak");
  }

  fs::path RecordPath() const { return recordRoot / "native-zoom.json"; }
  fs::path PendingPath() const {
    return recordRoot / "native-zoom-pending.json";
  }
};

void ExpectStatus(CaseResult& result, const StatusResult& status,
                  StatusCode expected, std::string_view label) {
  result.Check(status.code == expected, std::string(label) + " status",
               "expected " + std::string(StatusCodeName(expected)) + ", got " +
                   StatusCodeName(status.code) +
                   (status.detail.empty() ? "" : " (" + status.detail + ")"));
}

std::string OriginalBytes() { return "original app.asar payload\n"; }
std::string PatchedBytes() {
  return "patched app.asar payload with native pinch\n";
}
std::string OtherBytes() { return "unexpected replacement payload\n"; }

struct InstalledData {
  std::string original;
  std::string patched;
  std::string originalHash;
  std::string patchedHash;
  fs::path backup;
};

InstalledData PrepareInstalled(Fixture& fixture, bool uppercaseHashes = false) {
  InstalledData data;
  data.original = OriginalBytes();
  data.patched = PatchedBytes();
  data.originalHash = Hash(data.original);
  data.patchedHash = Hash(data.patched);
  data.backup = fixture.BackupPath();

  WriteFixtureFile(fixture.asar, data.patched);
  WriteFixtureFile(data.backup, data.original);
  nlohmann::json record =
      MakeRecord(fixture.asar, data.backup, data.originalHash, data.patchedHash);
  if (uppercaseHashes) {
    record["originalHash"] = Upper(data.originalHash);
    record["patchedHash"] = Upper(data.patchedHash);
    record["payloadHash"] = Upper(std::string(kPayloadHash));
  }
  WriteJson(fixture.RecordPath(), record);
  return data;
}

struct PendingData {
  std::string original;
  std::string patched;
  std::string originalHash;
  std::string patchedHash;
  fs::path backup;
  fs::path rollback;
  nlohmann::json newRecord;
};

PendingData PreparePending(Fixture& fixture, bool archiveIsPatched,
                           std::optional<std::string> archiveBytes =
                               std::nullopt,
                           bool includeCommittedRecord = false) {
  PendingData data;
  data.original = OriginalBytes();
  data.patched = PatchedBytes();
  data.originalHash = Hash(data.original);
  data.patchedHash = Hash(data.patched);
  data.backup = fixture.BackupPath("11111111111111111111111111111111");
  data.rollback = fixture.RollbackPath("22222222222222222222222222222222");

  WriteFixtureFile(fixture.asar,
            archiveBytes.has_value()
                ? *archiveBytes
                : (archiveIsPatched ? data.patched : data.original));
  WriteFixtureFile(data.backup, data.original);
  WriteFixtureFile(data.rollback, data.original);

  data.newRecord =
      MakeRecord(fixture.asar, data.backup, data.originalHash, data.patchedHash);
  WriteJson(fixture.PendingPath(),
            MakePending(fixture.asar, data.originalHash, data.patchedHash,
                        data.rollback, nlohmann::json(nullptr),
                        data.newRecord));
  if (includeCommittedRecord) {
    WriteJson(fixture.RecordPath(), data.newRecord);
  }
  return data;
}

struct TestCase {
  std::string name;
  std::function<std::function<void(CaseResult&)>(Fixture&)> setup;
};

std::vector<TestCase> BuildCases() {
  std::vector<TestCase> cases;

  cases.push_back({"installed", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     const RecordPaths derived =
                         MakeRecordPaths(fixture.notionExe, fixture.dataDir);
                     return [&fixture, data, derived](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::Installed,
                                    "installed");
                       result.Check(status.installed, "installed flag");
                       result.Equal(status.currentHash, data.patchedHash,
                                    "current hash");
                       result.Equal(status.expectedHash, data.patchedHash,
                                    "expected hash");
                       result.Equal(status.originalHash, data.originalHash,
                                    "original hash");
                       result.Equal(status.backupPath, Utf8(data.backup),
                                    "backup path");
                       result.Equal(Utf8(status.paths.recordPath()),
                                    Utf8(fixture.RecordPath()),
                                    "record path");
                       result.Equal(Utf8(status.paths.pendingPath()),
                                    Utf8(fixture.PendingPath()),
                                    "pending path");
                       result.Equal(Utf8(derived.asarPath),
                                    Utf8(fixture.paths.asarPath),
                                    "derived asar path");
                       result.Equal(Utf8(derived.recordRoot),
                                    Utf8(fixture.paths.recordRoot),
                                    "derived record root");
                     };
                   }});

  cases.push_back({"not-installed", [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "record root absent before");
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::NotInstalled,
                                    "not installed");
                       result.Check(!status.installed, "installed flag false");
                       result.Check(status.currentHash.empty(),
                                    "no current hash");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "record root still absent");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "record still absent");
                     };
                   }});

  cases.push_back({"archive-changed", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     const std::string replacement = OtherBytes();
                     const std::string replacementHash = Hash(replacement);
                     WriteFixtureFile(fixture.asar, replacement);
                     return [&fixture, data, replacement,
                             replacementHash](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::ArchiveChanged,
                                    "archive changed");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Equal(status.currentHash, replacementHash,
                                    "replacement hash");
                       result.Equal(status.expectedHash, data.patchedHash,
                                    "expected hash");
                       result.Equal(ReadFile(fixture.asar), replacement,
                                    "archive bytes preserved");
                     };
                   }});

  cases.push_back({"archive-missing", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     fs::remove(fixture.asar);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::ArchiveChanged,
                                    "archive missing");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Check(status.currentHash.empty(),
                                    "no current hash");
                       result.Equal(status.expectedHash, data.patchedHash,
                                    "expected hash");
                       result.Check(!fs::exists(fixture.asar),
                                    "archive remains missing");
                     };
                   }});

  cases.push_back({"record-malformed", [](Fixture& fixture) {
                     WriteFixtureFile(fixture.asar, "archive");
                     WriteFixtureFile(fixture.RecordPath(), "{ \"asarPath\": ");
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "malformed record");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Contains(status.detail, "not valid JSON",
                                       "malformed detail");
                     };
                   }});

  cases.push_back({"record-oversized", [](Fixture& fixture) {
                     WriteFixtureFile(fixture.asar, "archive");
                     WriteFixtureFile(fixture.RecordPath(),
                               std::string(1024 * 1024 + 1, ' '));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "oversized record");
                       result.Contains(status.detail,
                                       "exceeds the supported size",
                                       "oversized detail");
                     };
                   }});

  cases.push_back({"record-invalid-hash-short", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     WriteJson(fixture.RecordPath(),
                               MakeRecord(fixture.asar, data.backup,
                                          std::string(63, 'a'),
                                          data.patchedHash));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "short hash");
                       result.Contains(status.detail,
                                       "Invalid native zoom record hash.",
                                       "short hash detail");
                     };
                   }});

  cases.push_back({"record-invalid-hash-nonhex", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     WriteJson(fixture.RecordPath(),
                               MakeRecord(fixture.asar, data.backup,
                                          std::string(64, 'g'),
                                          data.patchedHash));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "non-hex hash");
                       result.Contains(status.detail,
                                       "Invalid native zoom record hash.",
                                       "non-hex hash detail");
                     };
                   }});

  cases.push_back({"record-wrong-asar", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     const fs::path otherAsar =
                         fixture.root / "Other" / "resources" / "app.asar";
                     WriteJson(fixture.RecordPath(),
                               MakeRecord(otherAsar, data.backup,
                                          data.originalHash,
                                          data.patchedHash));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "wrong asar");
                       result.Contains(status.detail,
                                       "different Notion installation",
                                       "wrong asar detail");
                     };
                   }});

  cases.push_back({"record-backup-outside-root", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     const fs::path outside =
                         fixture.root / "outside" /
                         ("app-asar-before-native-" + std::string(32, 'a') +
                          ".bak");
                     WriteFixtureFile(outside, data.original);
                     WriteJson(fixture.RecordPath(),
                               MakeRecord(fixture.asar, outside,
                                          data.originalHash,
                                          data.patchedHash));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "outside backup");
                       result.Contains(status.detail,
                                       "Unexpected native zoom backup path.",
                                       "outside backup detail");
                     };
                   }});

  cases.push_back({"record-backup-bad-leaf", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     const fs::path badBackup =
                         fixture.recordRoot / "backup.bak";
                     WriteFixtureFile(badBackup, data.original);
                     WriteJson(fixture.RecordPath(),
                               MakeRecord(fixture.asar, badBackup,
                                          data.originalHash,
                                          data.patchedHash));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "bad backup leaf");
                       result.Contains(status.detail,
                                       "Unexpected native zoom backup path.",
                                       "bad backup leaf detail");
                     };
                   }});

  cases.push_back({"record-backup-missing", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     fs::remove(data.backup);
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "missing backup");
                       result.Contains(
                           status.detail,
                           "The original native zoom backup is missing or "
                           "changed.",
                           "missing backup detail");
                     };
                   }});

  cases.push_back({"record-backup-changed", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     WriteFixtureFile(data.backup, OtherBytes());
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "changed backup");
                       result.Contains(
                           status.detail,
                           "The original native zoom backup is missing or "
                           "changed.",
                           "changed backup detail");
                     };
                   }});

  cases.push_back({"record-path-directory", [](Fixture& fixture) {
                     WriteFixtureFile(fixture.asar, "archive");
                     fs::create_directories(fixture.RecordPath());
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "record directory");
                       result.Contains(status.detail, "directory",
                                       "record directory detail");
                     };
                   }});

  cases.push_back({"record-uppercase-hashes", [](Fixture& fixture) {
                     const InstalledData data =
                         PrepareInstalled(fixture, true);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::Installed,
                                    "uppercase hashes");
                       result.Check(status.installed, "installed flag");
                       result.Equal(status.currentHash, data.patchedHash,
                                    "current hash normalized");
                       result.Equal(status.expectedHash, data.patchedHash,
                                    "expected hash normalized");
                       result.Equal(status.originalHash, data.originalHash,
                                    "original hash normalized");
                     };
                   }});

  cases.push_back({"record-extra-fields-bom", [](Fixture& fixture) {
                     const InstalledData data = PrepareInstalled(fixture);
                     nlohmann::json record =
                         MakeRecord(fixture.asar, data.backup,
                                    data.originalHash, data.patchedHash);
                     record["futureField"] = {
                         {"nested", true}, {"values", {1, 2, 3}}};
                     WriteFixtureFile(fixture.RecordPath(),
                               std::string("\xEF\xBB\xBF") + record.dump(2) +
                                   "\n");
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::Installed,
                                    "extra fields and BOM");
                       result.Check(status.installed, "installed flag");
                     };
                   }});

  cases.push_back({"unicode-spaces", [](Fixture& fixture) {
                     const fs::path unicodeRoot =
                         fixture.root / L"N\u00f6tion \u5e94\u7528 with spaces";
                     const fs::path notion = unicodeRoot / "Notion";
                     const fs::path asar =
                         notion / "resources" / "app.asar";
                     const fs::path data =
                         fixture.root / L"N\u00f6tion \u6570\u636e with spaces";
                     const fs::path recordRoot = data / "Notion KEX";

                     const std::string original = "unicode original";
                     const std::string patched = "unicode patched";
                     const std::string originalHash = Hash(original);
                     const std::string patchedHash = Hash(patched);
                     const fs::path backup =
                         recordRoot /
                         ("app-asar-before-native-" + std::string(32, 'c') +
                          ".bak");

                     WriteFixtureFile(notion / "Notion.exe", "");
                     WriteFixtureFile(asar, patched);
                     WriteFixtureFile(backup, original);
                     const RecordPaths paths =
                         MakeRecordPathsFromAsar(asar, data);
                     WriteJson(paths.recordPath(),
                               MakeRecord(asar, backup, originalHash,
                                          patchedHash));

                     return [paths, asar, backup, originalHash,
                             patchedHash](CaseResult& result) {
                       const StatusResult status = Inspect(paths);
                       ExpectStatus(result, status, StatusCode::Installed,
                                    "unicode paths");
                       result.Check(status.installed, "installed flag");
                       result.Equal(status.currentHash, patchedHash,
                                    "unicode current hash");
                       result.Equal(status.originalHash, originalHash,
                                    "unicode original hash");
                       result.Equal(status.backupPath, Utf8(backup),
                                    "unicode backup path");
                       result.Equal(Utf8(paths.asarPath), Utf8(asar),
                                    "unicode asar path");
                       result.Equal(Utf8(paths.recordPath()),
                                    Utf8(backup.parent_path() /
                                         "native-zoom.json"),
                                    "unicode record path");
                     };
                   }});

  cases.push_back({"reparse-record-root", [](Fixture& fixture) {
                     const fs::path realRoot =
                         fixture.root / "real-record-root";
                     std::error_code error;
                     fs::create_directories(realRoot, error);
                     if (error) {
                       throw std::runtime_error(
                           "Could not create reparse target: " +
                           error.message());
                     }
                     std::string junctionError;
                     if (!CreateJunction(fixture.recordRoot, realRoot,
                                         junctionError)) {
                       throw SkipCase{junctionError};
                     }

                     const std::string original = "original";
                     const std::string patched = "archive";
                     const fs::path backup =
                         realRoot /
                         ("app-asar-before-native-" + std::string(32, 'd') +
                          ".bak");
                     WriteFixtureFile(fixture.asar, patched);
                     WriteFixtureFile(backup, original);
                     WriteJson(realRoot / "native-zoom.json",
                               MakeRecord(fixture.asar, backup, Hash(original),
                                          Hash(patched)));

                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "reparse record root");
                       result.Contains(
                           status.detail,
                           "symbolic links or junctions",
                           "reparse record root detail");
                     };
                   }});

  cases.push_back({"reparse-asar-path", [](Fixture& fixture) {
                     const fs::path realNotion = fixture.root / "real-notion";
                     const fs::path linkedNotion =
                         fixture.root / "Linked Notion";
                     std::error_code error;
                     fs::create_directories(realNotion / "resources", error);
                     if (error) {
                       throw std::runtime_error(
                           "Could not create reparse target: " +
                           error.message());
                     }
                     std::string junctionError;
                     if (!CreateJunction(linkedNotion, realNotion,
                                         junctionError)) {
                       throw SkipCase{junctionError};
                     }

                     const fs::path asar =
                         linkedNotion / "resources" / "app.asar";
                     const std::string original = "original";
                     const std::string patched = "patched";
                     const fs::path backup =
                         fixture.recordRoot /
                         ("app-asar-before-native-" + std::string(32, 'e') +
                          ".bak");
                     WriteFixtureFile(asar, patched);
                     WriteFixtureFile(backup, original);
                     const RecordPaths paths =
                         MakeRecordPathsFromAsar(asar, fixture.dataDir);
                     WriteJson(paths.recordPath(),
                               MakeRecord(asar, backup, Hash(original),
                                          Hash(patched)));

                     return [paths](CaseResult& result) {
                       const StatusResult status = Inspect(paths);
                       ExpectStatus(result, status, StatusCode::InvalidRecord,
                                    "reparse asar path");
                       result.Contains(
                           status.detail,
                           "symbolic links or junctions",
                           "reparse asar path detail");
                     };
                   }});

  cases.push_back({"pending-before-swap", [](Fixture& fixture) {
                     const PendingData data =
                         PreparePending(fixture, false);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "pending before swap");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Equal(status.recoveryState,
                                    "archive-matches-previous",
                                    "recovery state");
                       result.Equal(status.currentHash, data.originalHash,
                                    "current hash");
                       result.Equal(status.expectedHash, data.patchedHash,
                                    "expected hash");
                       result.Equal(status.originalHash, data.originalHash,
                                    "original hash");
                       result.Equal(status.backupPath, Utf8(data.backup),
                                    "backup path");
                       result.Contains(status.message, "requires recovery",
                                       "recovery message");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                       result.Check(fs::exists(data.rollback),
                                    "rollback retained");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "no record created");
                     };
                   }});

  cases.push_back({"pending-after-swap", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, true);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "pending after swap");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Equal(status.recoveryState,
                                    "archive-matches-next",
                                    "recovery state");
                       result.Equal(status.currentHash, data.patchedHash,
                                    "current hash");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                       result.Check(fs::exists(data.rollback),
                                    "rollback retained");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "no record created");
                     };
                   }});

  cases.push_back({"pending-conflict", [](Fixture& fixture) {
                     const std::string replacement = OtherBytes();
                     const std::string replacementHash = Hash(replacement);
                     const PendingData data =
                         PreparePending(fixture, false, replacement);
                     return [&fixture, data,
                             replacementHash](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "pending conflict");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Equal(status.recoveryState, "archive-conflict",
                                    "recovery state");
                       result.Equal(status.currentHash, replacementHash,
                                    "current hash");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                       result.Check(fs::exists(data.rollback),
                                    "rollback retained");
                     };
                   }});

  cases.push_back({"pending-archive-missing", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, true);
                     fs::remove(fixture.asar);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "pending archive missing");
                       result.Check(!status.installed,
                                    "installed flag false");
                       result.Equal(status.recoveryState, "archive-missing",
                                    "recovery state");
                       result.Check(status.currentHash.empty(),
                                    "no current hash");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                       result.Check(fs::exists(data.rollback),
                                    "rollback retained");
                     };
                   }});

  cases.push_back({"pending-precedence", [](Fixture& fixture) {
                     const PendingData data =
                         PreparePending(fixture, true, std::nullopt, true);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "pending precedence");
                       result.Equal(status.recoveryState,
                                    "archive-matches-next",
                                    "recovery state");
                       result.Check(fs::exists(fixture.RecordPath()),
                                    "committed record retained");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                     };
                   }});

  cases.push_back({"pending-malformed", [](Fixture& fixture) {
                     WriteFixtureFile(fixture.asar, "archive");
                     WriteFixtureFile(fixture.PendingPath(), "{ not json");
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "malformed pending");
                       result.Contains(status.detail, "not valid JSON",
                                       "malformed pending detail");
                     };
                   }});

  cases.push_back({"pending-invalid-previous-record", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     WriteJson(fixture.PendingPath(),
                               MakePending(
                                   fixture.asar, data.originalHash,
                                   data.patchedHash, data.rollback,
                                   nlohmann::json{{"asarPath", 123}},
                                   data.newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "invalid previous record");
                       result.Contains(status.detail,
                                       "missing string field 'asarPath'",
                                       "invalid previous record detail");
                     };
                   }});

  cases.push_back({"pending-previous-backup-missing",
                   [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     const fs::path missingBackup =
                         fixture.recordRoot /
                         ("app-asar-before-native-" + std::string(32, 'f') +
                          ".bak");
                     const nlohmann::json previous =
                         MakeRecord(fixture.asar, missingBackup,
                                    Hash("previous"), Hash("previous patched"));
                     WriteJson(fixture.PendingPath(),
                               MakePending(fixture.asar, data.originalHash,
                                           data.patchedHash, data.rollback,
                                           previous, data.newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "previous backup missing");
                       result.Contains(
                           status.detail,
                           "The original native zoom backup is missing or "
                           "changed.",
                           "previous backup missing detail");
                     };
                   }});

  cases.push_back({"pending-missing-previous-record",
                   [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     nlohmann::json pending = MakePending(
                         fixture.asar, data.originalHash, data.patchedHash,
                         data.rollback, nlohmann::json(nullptr),
                         data.newRecord);
                     pending.erase("previousRecord");
                     WriteJson(fixture.PendingPath(), pending);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::RecoveryRequired,
                                    "missing previous record");
                       result.Equal(status.recoveryState,
                                    "archive-matches-previous",
                                    "recovery state");
                       result.Equal(status.currentHash, data.originalHash,
                                    "current hash");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                     };
                   }});

  cases.push_back({"pending-new-backup-missing", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     fs::remove(data.backup);
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "new backup missing");
                       result.Contains(
                           status.detail,
                           "The original native zoom backup is missing or "
                           "changed.",
                           "new backup missing detail");
                     };
                   }});

  cases.push_back({"pending-rollback-bad-leaf", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     const fs::path badRollback =
                         fixture.recordRoot / "rollback.bak";
                     WriteFixtureFile(badRollback, data.original);
                     WriteJson(fixture.PendingPath(),
                               MakePending(fixture.asar, data.originalHash,
                                           data.patchedHash, badRollback,
                                           nlohmann::json(nullptr),
                                           data.newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "rollback bad leaf");
                       result.Contains(status.detail,
                                       "Unexpected native rollback path.",
                                       "rollback bad leaf detail");
                     };
                   }});

  cases.push_back({"pending-new-record-wrong-asar",
                   [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     nlohmann::json newRecord = data.newRecord;
                     newRecord["asarPath"] =
                         Utf8(fixture.root / "Other" / "resources" /
                              "app.asar");
                     WriteJson(fixture.PendingPath(),
                               MakePending(fixture.asar, data.originalHash,
                                           data.patchedHash, data.rollback,
                                           nlohmann::json(nullptr),
                                           newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "new record wrong asar");
                       result.Contains(status.detail,
                                       "different Notion installation",
                                       "new record wrong asar detail");
                     };
                   }});

  cases.push_back({"pending-rollback-missing", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     fs::remove(data.rollback);
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "rollback missing");
                       result.Contains(
                           status.detail,
                           "The native rollback backup is missing or changed.",
                           "rollback missing detail");
                     };
                   }});

  cases.push_back({"pending-rollback-changed", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     WriteFixtureFile(data.rollback, OtherBytes());
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "rollback changed");
                       result.Contains(
                           status.detail,
                           "The native rollback backup is missing or changed.",
                           "rollback changed detail");
                     };
                   }});

  cases.push_back({"pending-rollback-outside-root",
                   [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     const fs::path outside =
                         fixture.root / "outside" /
                         ("native-rollback-" + std::string(32, '1') + ".bak");
                     WriteFixtureFile(outside, data.original);
                     WriteJson(fixture.PendingPath(),
                               MakePending(fixture.asar, data.originalHash,
                                           data.patchedHash, outside,
                                           nlohmann::json(nullptr),
                                           data.newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "rollback outside root");
                       result.Contains(status.detail,
                                       "Unexpected native rollback path.",
                                       "rollback outside root detail");
                     };
                   }});

  cases.push_back({"pending-wrong-asar", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     const fs::path otherAsar =
                         fixture.root / "Other" / "resources" / "app.asar";
                     WriteJson(fixture.PendingPath(),
                               MakePending(otherAsar, data.originalHash,
                                           data.patchedHash, data.rollback,
                                           nlohmann::json(nullptr),
                                           data.newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "pending wrong asar");
                       result.Contains(status.detail,
                                       "different Notion installation",
                                       "pending wrong asar detail");
                     };
                   }});

  cases.push_back({"pending-next-hash-mismatch", [](Fixture& fixture) {
                     const PendingData data = PreparePending(fixture, false);
                     nlohmann::json newRecord = data.newRecord;
                     newRecord["patchedHash"] = Hash(OtherBytes());
                     WriteJson(fixture.PendingPath(),
                               MakePending(fixture.asar, data.originalHash,
                                           data.patchedHash, data.rollback,
                                           nlohmann::json(nullptr),
                                           newRecord));
                     return [&fixture](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       ExpectStatus(result, status,
                                    StatusCode::InvalidRecovery,
                                    "pending next hash mismatch");
                       result.Contains(
                           status.detail,
                           "Invalid pending native zoom transaction.",
                           "pending next hash mismatch detail");
                     };
                   }});

  cases.push_back({"hash-and-record-primitives", [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       std::string hex;
                       std::string error;
                       result.Check(HashBytes("", 0, hex, error),
                                    "hash empty succeeds", error);
                       result.Equal(
                           hex,
                           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934"
                           "ca495991b7852b855",
                           "empty sha256");
                       result.Check(HashBytes("abc", 3, hex, error),
                                    "hash abc succeeds", error);
                       result.Equal(
                           hex,
                           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
                           "b410ff61f20015ad",
                           "abc sha256");
                       result.Check(IsValidSha256(std::string(64, 'a')),
                                    "valid lowercase hash");
                       result.Check(IsValidSha256(std::string(64, 'A')),
                                    "valid uppercase hash");
                       result.Check(!IsValidSha256(std::string(63, 'a')),
                                    "short hash rejected");
                       result.Check(!IsValidSha256(std::string(64, 'g')),
                                    "non-hex hash rejected");
                       result.Equal(NormalizeSha256(std::string(64, 'A')),
                                    std::string(64, 'a'),
                                    "normalize uppercase");
                       std::string missingHash;
                       std::string missingError;
                       result.Check(
                           HashFile(fixture.root / "missing.bin", missingHash,
                                    missingError) == FileHashState::Missing,
                           "missing file state", missingError);
                     };
                   }});

  return cases;
}

struct TestOptions {
  bool keep = false;
  bool help = false;
  std::optional<fs::path> evidence;
  std::optional<std::string> filter;
};

bool ParseArguments(int argc, wchar_t** argv, TestOptions& options,
                    std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--help" || argument == L"-h") {
      options.help = true;
    } else if (argument == L"--keep") {
      options.keep = true;
    } else if (argument == L"--evidence") {
      if (options.evidence.has_value()) {
        error = "--evidence was supplied more than once.";
        return false;
      }
      if (index + 1 >= argc) {
        error = "--evidence requires a path.";
        return false;
      }
      options.evidence = fs::path(argv[++index]);
    } else if (argument == L"--filter") {
      if (options.filter.has_value()) {
        error = "--filter was supplied more than once.";
        return false;
      }
      if (index + 1 >= argc) {
        error = "--filter requires text.";
        return false;
      }
      const std::wstring wide = argv[++index];
      const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        error = "--filter is not valid UTF-16.";
        return false;
      }
      std::string text(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                          static_cast<int>(wide.size()), text.data(), size,
                          nullptr, nullptr);
      options.filter = std::move(text);
    } else {
      error = "Unknown option. Run kex-status-tests --help for usage.";
      return false;
    }
  }
  return true;
}

void PrintUsage() {
  Print(
      "Usage: kex-status-tests [options]\n"
      "\n"
      "Runs generated-fixture tests for the read-only kex-status component.\n"
      "Every case snapshots its fixture tree before and after inspection.\n"
      "\n"
      "  --evidence <path>  Write machine-readable JSON evidence.\n"
      "  --filter <text>    Run only cases whose names contain text.\n"
      "  --keep             Keep generated fixture directories.\n"
      "  --help, -h         Show this help.\n");
}

std::string SanitizeName(const std::string& name) {
  std::string result = name;
  for (char& character : result) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') ||
                         character == '-';
    if (!allowed) {
      character = '-';
    }
  }
  return result;
}

CaseResult RunOne(const TestCase& testCase, const fs::path& caseRoot,
                  bool keep) {
  CaseResult result;
  result.name = testCase.name;
  Fixture fixture;
  fixture.root = caseRoot;

  try {
    fixture.Initialize();
    const std::function<void(CaseResult&)> run = testCase.setup(fixture);
    const std::vector<SnapshotEntry> before = SnapshotTree(caseRoot);
    result.snapshotEntries = static_cast<int>(before.size());

    try {
      run(result);
    } catch (const std::exception& exception) {
      result.passed = false;
      result.assertions.push_back(
          Assertion{"run threw", false, exception.what()});
    }

    const std::vector<SnapshotEntry> after = SnapshotTree(caseRoot);
    result.passive = (before == after);
    if (!result.passive) {
      result.passed = false;
      result.passivityDetail = DescribeSnapshotDiff(before, after);
    }
  } catch (const SkipCase& skip) {
    result.skipped = true;
    result.skipReason = skip.reason;
  } catch (const std::exception& exception) {
    result.passed = false;
    result.assertions.push_back(
        Assertion{"unexpected exception", false, exception.what()});
  }

  if (!keep) {
    RemoveTree(caseRoot);
  }
  return result;
}

nlohmann::json CaseJson(const CaseResult& result) {
  nlohmann::json assertions = nlohmann::json::array();
  for (const Assertion& assertion : result.assertions) {
    assertions.push_back(nlohmann::json{
        {"name", assertion.name},
        {"passed", assertion.passed},
        {"detail", assertion.detail},
    });
  }
  return nlohmann::json{
      {"name", result.name},
      {"passed", result.passed},
      {"skipped", result.skipped},
      {"skipReason", result.skipReason},
      {"passive", result.passive},
      {"passivityDetail", result.passivityDetail},
      {"snapshotEntries", result.snapshotEntries},
      {"assertions", std::move(assertions)},
  };
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  TestOptions options;
  std::string error;
  if (!ParseArguments(argc, argv, options, error)) {
    PrintError("kex-status-tests: " + error + "\n\n");
    PrintUsage();
    return 2;
  }
  if (options.help) {
    PrintUsage();
    return 0;
  }

  std::vector<TestCase> cases = BuildCases();
  if (options.filter.has_value()) {
    const std::string& filter = *options.filter;
    cases.erase(std::remove_if(cases.begin(), cases.end(),
                               [&filter](const TestCase& testCase) {
                                 return testCase.name.find(filter) ==
                                        std::string::npos;
                               }),
                cases.end());
    if (cases.empty()) {
      PrintError("kex-status-tests: no cases matched --filter.\n");
      return 2;
    }
  }

  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const fs::path base =
      fs::temp_directory_path() /
      ("notion-kex-native-status-tests-" +
       std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
  std::error_code directoryError;
  fs::create_directories(base, directoryError);
  if (directoryError) {
    PrintError("kex-status-tests: could not create fixture root: " +
               directoryError.message() + "\n");
    return 2;
  }

  std::vector<CaseResult> results;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  bool allPassive = true;

  Print("kex-status fixture tests\n");
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const fs::path caseRoot =
        base / (std::to_string(index + 1) + "-" + SanitizeName(cases[index].name));
    CaseResult result = RunOne(cases[index], caseRoot, options.keep);
    if (result.skipped) {
      ++skipped;
      Print("[SKIP] " + result.name + ": " + result.skipReason + "\n");
    } else if (result.passed) {
      ++passed;
      Print("[PASS] " + result.name + " (" +
            std::to_string(result.assertions.size()) + " assertions, " +
            std::to_string(result.snapshotEntries) + " snapshot entries, " +
            (result.passive ? "passive" : "CHANGED") + ")\n");
    } else {
      ++failed;
      Print("[FAIL] " + result.name + "\n");
      for (const Assertion& assertion : result.assertions) {
        if (!assertion.passed) {
          Print("       " + assertion.name + ": " + assertion.detail + "\n");
        }
      }
      if (!result.passive) {
        Print("       passivity: " + result.passivityDetail + "\n");
      }
    }
    allPassive = allPassive && result.passive;
    results.push_back(std::move(result));
  }

  Print("\nsummary: " + std::to_string(passed) + " passed, " +
        std::to_string(failed) + " failed, " + std::to_string(skipped) +
        " skipped; " + (allPassive ? "all fixtures unchanged" : "FIXTURES CHANGED") +
        "\n");
  if (options.keep) {
    Print("fixtures kept at: " + Utf8(base) + "\n");
  }

  if (options.evidence.has_value()) {
    fs::path evidencePath = *options.evidence;
    if (evidencePath.is_relative()) {
      evidencePath = fs::absolute(evidencePath);
    }
    std::error_code errorCode;
    fs::create_directories(evidencePath.parent_path(), errorCode);
    if (errorCode) {
      PrintError("kex-status-tests: could not create evidence directory: " +
                 errorCode.message() + "\n");
      return 2;
    }

    nlohmann::json evidence;
    evidence["component"] = "kex-status";
    evidence["version"] = KEX_STATUS_VERSION;
    evidence["generatedAt"] = IsoNow();
    evidence["build"] = {
        {"compilerId", KEX_COMPILER_ID},
        {"compilerVersion", KEX_COMPILER_VERSION},
        {"msvcVersion", _MSC_VER},
        {"cplusplus", __cplusplus},
        {"configuration", KEX_BUILD_TYPE},
        {"pointerBits", sizeof(void*) * 8},
    };
    evidence["capabilities"] = {
        {"readOnly", true},
        {"writesFiles", false},
        {"launchesProcesses", false},
        {"network", false},
        {"watcher", false},
        {"autostart", false},
        {"recoversPendingTransactions", false},
    };
    evidence["cases"] = nlohmann::json::array();
    for (const CaseResult& result : results) {
      evidence["cases"].push_back(CaseJson(result));
    }
    evidence["summary"] = {
        {"total", static_cast<int>(results.size())},
        {"passed", passed},
        {"failed", failed},
        {"skipped", skipped},
        {"allPassed", failed == 0},
        {"allPassive", allPassive},
    };
    evidence["limitations"] = nlohmann::json::array(
        {"Fixture tests prove passive status semantics only; they do not prove "
         "repair, launch, updater integration, or live Notion compatibility.",
         "A pending journal is reported but never recovered, committed, or "
         "cleaned up.",
         "The original safety-stop reason remains unknown; this component does "
         "not claim to resolve it."});

    std::ofstream stream(evidencePath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      PrintError("kex-status-tests: could not write evidence file.\n");
      return 2;
    }
    const std::string text = evidence.dump(2) + "\n";
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
      PrintError("kex-status-tests: could not finish evidence file.\n");
      return 2;
    }
    Print("evidence: " + Utf8(evidencePath) + "\n");
  }

  if (!options.keep) {
    std::error_code cleanupError;
    fs::remove(base, cleanupError);
  }
  return failed == 0 ? 0 : 1;
}
