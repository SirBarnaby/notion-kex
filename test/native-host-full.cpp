// Full fixture suite for the native Notion KEX host.
//
// Every case runs inside a generated temporary tree. No case reads or writes
// the live Notion installation, the real desktop, the real Start menu, the
// real taskbar or the real startup folder. Shortcut tests pass explicit
// ShortcutLocations and CLI tests pass --no-shortcuts --launcher-root.
//
// The suite exercises the same transaction engine that tools/NativeZoom.ps1
// implements: bounded ASAR parsing, integrity verification, backup creation,
// pending-journal recovery, concurrent-change refusal, extension lifecycle,
// shortcuts, launcher fast path, watcher, sign-in check and the CLI.

#include "notion_kex/asar.hpp"
#include "notion_kex/embedded.hpp"
#include "notion_kex/hash.hpp"
#include "notion_kex/launcher.hpp"
#include "notion_kex/native_zoom.hpp"
#include "notion_kex/record.hpp"
#include "notion_kex/setup.hpp"
#include "notion_kex/shortcuts.hpp"
#include "notion_kex/status.hpp"

#include "file_util.hpp"
#include "path_util.hpp"
#include "process.hpp"

#include <windows.h>
#include <objbase.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef KEX_VERSION
#define KEX_VERSION "0.0.0"
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
#ifndef KEX_CLI_PATH
#define KEX_CLI_PATH "Notion KEX.exe"
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using notion_kex::AsarArchive;
using notion_kex::FileHashState;
using notion_kex::HashBytes;
using notion_kex::HashFile;
using notion_kex::Inspect;
using notion_kex::InspectExtension;
using notion_kex::InstallExtension;
using notion_kex::InstallNativeZoom;
using notion_kex::InstallShortcuts;
using notion_kex::LauncherOptions;
using notion_kex::LauncherResult;
using notion_kex::MakeRecordPathsFromAsar;
using notion_kex::NativeRecord;
using notion_kex::NativeZoomOutcome;
using notion_kex::QuickInspect;
using notion_kex::QuickStatus;
using notion_kex::RecordPaths;
using notion_kex::RecoverNativeZoom;
using notion_kex::RepairNativeZoom;
using notion_kex::RunLauncher;
using notion_kex::RunSignInCheck;
using notion_kex::RunWatcher;
using notion_kex::SetupOutcome;
using notion_kex::ShortcutLocations;
using notion_kex::ShortcutOutcome;
using notion_kex::StatusCode;
using notion_kex::StatusCodeName;
using notion_kex::StatusResult;
using notion_kex::UninstallExtension;
using notion_kex::UninstallNativeZoom;
using notion_kex::UninstallShortcuts;
using notion_kex::ValidateNativeZoom;

constexpr std::string_view kPreloadPath =
    ".webpack/renderer/tab_browser_view/preload.js";
constexpr std::string_view kMainPath = ".webpack/main/index.js";
constexpr std::string_view kPreloadMarker =
    "/* Notion KEX native pinch v1 */";
constexpr std::string_view kMainMarker =
    "/* Notion KEX native pinch reset v1 */";
constexpr std::string_view kFuseSentinel =
    "dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX";
constexpr std::string_view kClosedMessage =
    "Quit Notion completely from its tray menu before changing its native "
    "pinch support.";
constexpr std::string_view kSetupClosedMessage =
    "Quit Notion completely from its tray menu, then run this command again. "
    "Running settings will not be overwritten.";
constexpr std::string_view kBusyMessage =
    "Another native zoom installation is running.";

constexpr std::array<const char*, 7> kExtensionFiles = {
    "extension/kex.css",        "extension/kex.js",
    "extension/manifest.json",  "extension/policy.js",
    "extension/settings.css",   "extension/settings.html",
    "extension/settings.js",
};

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::wstring WidenUtf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string NarrowUtf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      narrow.data(), size, nullptr, nullptr);
  return narrow;
}

std::string Hash(std::string_view bytes) {
  std::string hex;
  std::string error;
  if (!HashBytes(bytes.data(), bytes.size(), hex, error)) {
    throw std::runtime_error("HashBytes failed: " + error);
  }
  return hex;
}

std::string HashVector(const std::vector<std::uint8_t>& bytes) {
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

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

void WriteFixtureBytes(const fs::path& path,
                       const std::vector<std::uint8_t>& bytes) {
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
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  if (!stream) {
    throw std::runtime_error("Could not finish writing " + Utf8(path));
  }
}

void WriteFixtureFile(const fs::path& path, std::string_view bytes) {
  WriteFixtureBytes(path, Bytes(bytes));
}

std::vector<std::uint8_t> ReadFixtureBytes(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Could not read " + Utf8(path));
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

std::string ReadFixtureFile(const fs::path& path) {
  const std::vector<std::uint8_t> bytes = ReadFixtureBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

void WriteJsonFile(const fs::path& path,
                   const nlohmann::ordered_json& value) {
  WriteFixtureFile(path, value.dump(2) + "\n");
}

nlohmann::ordered_json ReadJsonFile(const fs::path& path) {
  return nlohmann::ordered_json::parse(ReadFixtureFile(path));
}

std::string Trim(std::string value) {
  const auto notSpace = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
              value.end());
  return value;
}

// ---------------------------------------------------------------------------
// Assertion harness
// ---------------------------------------------------------------------------

struct Assertion {
  std::string name;
  bool passed = false;
  std::string detail;
};

struct SkipCase {
  std::string reason;
};

struct CaseResult {
  std::string name;
  bool passed = true;
  bool skipped = false;
  std::string skipReason;
  bool passiveChecked = false;
  bool passive = true;
  std::string passivityDetail;
  int snapshotEntries = 0;
  std::vector<Assertion> assertions;

  void Check(bool condition, std::string name, std::string detail = {}) {
    assertions.push_back(
        Assertion{std::move(name), condition, std::move(detail)});
    if (!condition) {
      passed = false;
    }
  }

  void Equal(std::string actual, std::string expected, std::string name,
             std::string detail = {}) {
    const bool passed = actual == expected;
    if (detail.empty() && !passed) {
      detail = "expected '" + expected + "', got '" + actual + "'";
    }
    Check(passed, std::move(name), std::move(detail));
  }

  void EqualU64(std::uint64_t actual, std::uint64_t expected,
                std::string name) {
    const bool passed = actual == expected;
    Check(passed, std::move(name),
          passed ? std::string()
                 : "expected " + std::to_string(expected) + ", got " +
                       std::to_string(actual));
  }

  void Contains(std::string_view text, std::string_view needle,
                std::string name, std::string detail = {}) {
    const bool passed = text.find(needle) != std::string_view::npos;
    if (detail.empty() && !passed) {
      detail = "'" + std::string(text) + "' does not contain '" +
               std::string(needle) + "'";
    }
    Check(passed, std::move(name), std::move(detail));
  }

  void NotContains(std::string_view text, std::string_view needle,
                   std::string name, std::string detail = {}) {
    const bool passed = text.find(needle) == std::string_view::npos;
    if (detail.empty() && !passed) {
      detail = "'" + std::string(text) + "' unexpectedly contains '" +
               std::string(needle) + "'";
    }
    Check(passed, std::move(name), std::move(detail));
  }

  void NotEqual(std::string actual, std::string unexpected, std::string name,
                std::string detail = {}) {
    const bool passed = actual != unexpected;
    if (detail.empty() && !passed) {
      detail = "did not expect '" + unexpected + "'";
    }
    Check(passed, std::move(name), std::move(detail));
  }

  template <typename Callable>
  void Throws(Callable&& callable, std::string_view needle,
              std::string name) {
    try {
      callable();
      Check(false, std::move(name), "no exception was thrown");
    } catch (const std::exception& exception) {
      const std::string message = exception.what();
      Check(message.find(needle) != std::string::npos, std::move(name),
            "message: " + message);
    }
  }
};

// ---------------------------------------------------------------------------
// Snapshot helpers for passive checks
// ---------------------------------------------------------------------------

struct SnapshotEntry {
  std::string path;
  std::uint64_t size = 0;
  std::uint64_t writeTime = 0;
  std::uint32_t attributes = 0;

  bool operator==(const SnapshotEntry& other) const {
    return path == other.path && size == other.size &&
           writeTime == other.writeTime && attributes == other.attributes;
  }
};

std::optional<SnapshotEntry> SnapshotOne(const fs::path& root,
                                         const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return std::nullopt;
  }
  SnapshotEntry entry;
  std::error_code error;
  const fs::path relative = fs::relative(path, root, error);
  entry.path = error ? Utf8(path) : Utf8(relative);
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    ULARGE_INTEGER size = {};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    entry.size = size.QuadPart;
  }
  ULARGE_INTEGER writeTime = {};
  writeTime.LowPart = data.ftLastWriteTime.dwLowDateTime;
  writeTime.HighPart = data.ftLastWriteTime.dwHighDateTime;
  entry.writeTime = writeTime.QuadPart;
  entry.attributes = data.dwFileAttributes;
  return entry;
}

std::vector<SnapshotEntry> SnapshotTree(const fs::path& root) {
  std::vector<SnapshotEntry> entries;
  std::error_code error;
  if (!fs::exists(root, error)) {
    return entries;
  }
  fs::recursive_directory_iterator iterator(
      root, fs::directory_options::skip_permission_denied, error);
  if (error) {
    throw std::runtime_error("Could not snapshot " + Utf8(root) + ": " +
                             error.message());
  }
  const fs::recursive_directory_iterator end;
  while (iterator != end) {
    const fs::directory_entry& entry = *iterator;
    const std::optional<SnapshotEntry> snapshot =
        SnapshotOne(root, entry.path());
    if (snapshot.has_value()) {
      entries.push_back(*snapshot);
      if ((snapshot->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
          (snapshot->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
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

std::string DescribeSnapshotDiff(const std::vector<SnapshotEntry>& before,
                                 const std::vector<SnapshotEntry>& after) {
  std::map<std::string, SnapshotEntry> first;
  std::map<std::string, SnapshotEntry> second;
  for (const SnapshotEntry& entry : before) {
    first[entry.path] = entry;
  }
  for (const SnapshotEntry& entry : after) {
    second[entry.path] = entry;
  }
  std::ostringstream output;
  int changes = 0;
  for (const auto& [path, entry] : first) {
    const auto found = second.find(path);
    if (found == second.end()) {
      if (changes++ < 6) {
        output << "removed " << path << "; ";
      }
    } else if (!(entry == found->second)) {
      if (changes++ < 6) {
        output << "changed " << path << "; ";
      }
    }
  }
  for (const auto& [path, entry] : second) {
    if (first.find(path) == first.end()) {
      if (changes++ < 6) {
        output << "added " << path << "; ";
      }
    }
  }
  if (changes == 0) {
    return {};
  }
  return output.str();
}

void RemoveTree(const fs::path& root) {
  if (root.empty()) {
    return;
  }
  std::error_code error;
  fs::remove_all(root, error);
  if (!error) {
    return;
  }
  // A read-only or briefly locked fixture should not fail the whole run.
  for (int attempt = 0; attempt < 5; ++attempt) {
    Sleep(50);
    error.clear();
    fs::remove_all(root, error);
    if (!error) {
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// ASAR fixture writer
// ---------------------------------------------------------------------------

struct AsarMemberSpec {
  std::string path;
  std::string bytes;
  std::uint64_t blockSize = 4194304;
};

std::vector<std::string> SplitPath(std::string_view path) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
    parts.emplace_back(path.substr(start, end - start));
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return parts;
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void WriteAsarFile(const fs::path& path,
                   const std::vector<AsarMemberSpec>& members) {
  nlohmann::ordered_json header;
  header["files"] = nlohmann::ordered_json::object();
  std::string body;
  std::uint64_t offset = 0;

  for (const AsarMemberSpec& member : members) {
    if (member.blockSize == 0) {
      throw std::runtime_error("ASAR fixture block size must be positive.");
    }
    const std::vector<std::string> parts = SplitPath(member.path);
    if (parts.empty() || parts.front().empty()) {
      throw std::runtime_error("ASAR fixture member path is empty.");
    }

    nlohmann::ordered_json* directory = &header;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
      if (!(*directory).contains("files") ||
          !(*directory)["files"].is_object()) {
        (*directory)["files"] = nlohmann::ordered_json::object();
      }
      nlohmann::ordered_json& files = (*directory)["files"];
      if (!files.contains(parts[index]) || !files[parts[index]].is_object()) {
        files[parts[index]] = nlohmann::ordered_json::object();
      }
      directory = &files[parts[index]];
    }
    if (!(*directory).contains("files") ||
        !(*directory)["files"].is_object()) {
      (*directory)["files"] = nlohmann::ordered_json::object();
    }
    nlohmann::ordered_json& files = (*directory)["files"];

    nlohmann::ordered_json integrity;
    integrity["algorithm"] = "SHA256";
    integrity["hash"] = Hash(member.bytes);
    integrity["blockSize"] = member.blockSize;
    nlohmann::ordered_json blocks = nlohmann::ordered_json::array();
    for (std::size_t start = 0; start < member.bytes.size();
         start += static_cast<std::size_t>(member.blockSize)) {
      const std::size_t length = std::min<std::size_t>(
          static_cast<std::size_t>(member.blockSize),
          member.bytes.size() - start);
      blocks.push_back(Hash(std::string_view(member.bytes).substr(start, length)));
    }
    integrity["blocks"] = std::move(blocks);

    nlohmann::ordered_json entry;
    entry["size"] = member.bytes.size();
    entry["offset"] = std::to_string(offset);
    entry["integrity"] = std::move(integrity);
    files[parts.back()] = std::move(entry);

    body += member.bytes;
    offset += member.bytes.size();
  }

  const std::string json = header.dump();
  const std::uint64_t padded =
      ((static_cast<std::uint64_t>(json.size()) + 3) / 4) * 4;
  std::vector<std::uint8_t> output;
  AppendU32(output, 4);
  AppendU32(output, static_cast<std::uint32_t>(8 + padded));
  AppendU32(output, static_cast<std::uint32_t>(4 + padded));
  AppendU32(output, static_cast<std::uint32_t>(json.size()));
  output.insert(output.end(), json.begin(), json.end());
  output.resize(16 + static_cast<std::size_t>(padded), 0);
  output.insert(output.end(), body.begin(), body.end());
  WriteFixtureBytes(path, output);
}

// ---------------------------------------------------------------------------
// Fuse, state and shortcut fixtures
// ---------------------------------------------------------------------------

void WriteFuseExe(const fs::path& path, char integrity = '0',
                  bool sentinel = true, int version = 9,
                  int firstByte = 1) {
  std::string bytes = "Fixture only; never executed. ";
  if (sentinel) {
    bytes += kFuseSentinel;
  }
  bytes.push_back(static_cast<char>(firstByte));
  bytes.push_back(static_cast<char>(version));
  bytes += "0100";
  bytes.push_back(integrity);
  bytes += "1011";
  WriteFixtureFile(path, bytes);
}

std::string PreloadSource(std::string_view tail = "fixture preload tail") {
  return "const { contextBridge, ipcRenderer } = require('electron');\n"
         "window.__electronApi = { platform: 'win32' };\n"
         "window.__isElectron = true;\n"
         "/* " +
         std::string(tail) + " */\n";
}

std::string MainSource(std::string_view tail = "fixture main tail") {
  return "const { app, BrowserWindow } = require(\"electron\");\n"
         "function setupExtensions(){}\n"
         "function isChromeExtensionsEnabled(){ return true; }\n"
         "function isChromeExtensionsAllowed(){ return true; }\n"
         "function loadExtension(){}\n"
         "/* " +
         std::string(tail) + " */\n";
}

std::string BlockedBytes() {
  return "0123456789abcdefghijklmnopqrstuvwxyzABCD";
}

std::vector<AsarMemberSpec> StandardMembers() {
  return {
      {std::string(kPreloadPath), PreloadSource()},
      {std::string(kMainPath), MainSource()},
      {"node_modules/left-pad/index.js",
       "module.exports = (value, length) => value.padStart(length);\n"},
      {"package.json", "{\"name\":\"notion-fixture\",\"version\":\"1.0.0\"}\n"},
      {"assets/blocked.bin", BlockedBytes(), 16},
  };
}

nlohmann::ordered_json MakeState(bool chrome = false, bool debug = false,
                                 bool includePreferences = true) {
  nlohmann::ordered_json state;
  state["appState"] = nlohmann::ordered_json::object();
  state["appState"]["other"] = "kept";
  if (includePreferences) {
    nlohmann::ordered_json preferences;
    preferences["isChromeExtensionsEnabled"] = chrome;
    preferences["isDebugMenuEnabled"] = debug;
    preferences["disabledExtensions"] =
        nlohmann::ordered_json::array({"Notion KEX", "Other Extension"});
    preferences["theme"] = "dark";
    state["appState"]["preferences"] = std::move(preferences);
  }
  return state;
}

void WriteStateFile(const fs::path& path, const nlohmann::ordered_json& state) {
  WriteJsonFile(path, state);
}

std::wstring QuoteArgument(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

std::wstring QuotePath(const fs::path& path) {
  return QuoteArgument(path.wstring());
}

fs::path ComSpec() {
  const wchar_t* value = _wgetenv(L"ComSpec");
  if (value == nullptr || *value == L'\0') {
    return fs::path(L"C:\\Windows\\System32\\cmd.exe");
  }
  return fs::path(value);
}

struct ShortcutInfo {
  std::wstring target;
  std::wstring arguments;
  std::wstring workingDirectory;
  std::wstring icon;
  std::wstring description;
  std::wstring appId;
};

ShortcutInfo ReadTestShortcut(const fs::path& path) {
  ShortcutInfo info;
  ComPtr<IShellLinkW> link;
  const HRESULT created =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(link.GetAddressOf()));
  if (FAILED(created)) {
    throw std::runtime_error("Could not create a shell link reader.");
  }
  ComPtr<IPersistFile> persist;
  if (FAILED(link.As(&persist))) {
    throw std::runtime_error("Could not open the shell link interface.");
  }
  if (FAILED(persist->Load(path.c_str(), STGM_READ))) {
    throw std::runtime_error("Could not read shortcut " + Utf8(path));
  }

  std::vector<wchar_t> buffer(32768);
  WIN32_FIND_DATAW data = {};
  if (SUCCEEDED(link->GetPath(buffer.data(), static_cast<int>(buffer.size()),
                              &data, SLGP_RAWPATH))) {
    info.target = buffer.data();
  }
  buffer.assign(buffer.size(), L'\0');
  if (SUCCEEDED(link->GetArguments(buffer.data(),
                                   static_cast<int>(buffer.size())))) {
    info.arguments = buffer.data();
  }
  buffer.assign(buffer.size(), L'\0');
  if (SUCCEEDED(link->GetWorkingDirectory(
          buffer.data(), static_cast<int>(buffer.size())))) {
    info.workingDirectory = buffer.data();
  }
  buffer.assign(buffer.size(), L'\0');
  int iconIndex = 0;
  if (SUCCEEDED(link->GetIconLocation(buffer.data(),
                                      static_cast<int>(buffer.size()),
                                      &iconIndex))) {
    info.icon = buffer.data();
  }
  buffer.assign(buffer.size(), L'\0');
  if (SUCCEEDED(link->GetDescription(buffer.data(),
                                     static_cast<int>(buffer.size())))) {
    info.description = buffer.data();
  }

  ComPtr<IPropertyStore> store;
  if (SUCCEEDED(link.As(&store))) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value))) {
      if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        info.appId = value.pwszVal;
      }
    }
    PropVariantClear(&value);
  }
  return info;
}

void CreateTestShortcut(const fs::path& path, const fs::path& target,
                        const std::wstring& arguments,
                        const fs::path& workingDirectory,
                        const fs::path& icon) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create shortcut folder: " +
                             error.message());
  }
  ComPtr<IShellLinkW> link;
  const HRESULT created =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(link.GetAddressOf()));
  if (FAILED(created)) {
    throw std::runtime_error("Could not create a shell link.");
  }
  link->SetPath(target.c_str());
  link->SetArguments(arguments.c_str());
  link->SetWorkingDirectory(workingDirectory.c_str());
  link->SetIconLocation(icon.c_str(), 0);
  link->SetDescription(L"Notion");

  ComPtr<IPersistFile> persist;
  if (FAILED(link.As(&persist))) {
    throw std::runtime_error("Could not open the shell link interface.");
  }
  if (FAILED(persist->Save(path.c_str(), TRUE))) {
    throw std::runtime_error("Could not save shortcut " + Utf8(path));
  }
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

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct Fixture {
  fs::path root;
  fs::path notionDir;
  fs::path notionExe;
  fs::path asar;
  fs::path dataDir;
  fs::path recordRoot;
  fs::path launcherRoot;
  fs::path shortcutRoot;
  RecordPaths paths;

  void Initialize() {
    notionDir = root / "Notion";
    notionExe = notionDir / "Notion.exe";
    asar = notionDir / "resources" / "app.asar";
    dataDir = root / "Data";
    recordRoot = dataDir / "Notion KEX";
    launcherRoot = root / "Launcher";
    shortcutRoot = root / "Shortcuts";

    std::error_code error;
    for (const fs::path& directory :
         {notionDir, asar.parent_path(), dataDir, shortcutRoot}) {
      fs::create_directories(directory, error);
      if (error) {
        throw std::runtime_error("Could not create fixture: " +
                                 error.message());
      }
    }
    WriteFuseExe(notionExe);
    WriteAsarFile(asar, StandardMembers());
    WriteStateFile(StatePath(), MakeState());
    paths = MakeRecordPathsFromAsar(asar, dataDir);
  }

  fs::path StatePath() const { return dataDir / "state.json"; }
  fs::path InstallRoot() const {
    return dataDir / "extensions" / "Notion KEX";
  }
  fs::path InstallRecord() const {
    return dataDir / "Notion KEX" / "install.json";
  }
  fs::path RecordPath() const { return paths.recordPath(); }
  fs::path PendingPath() const { return paths.pendingPath(); }

  fs::path BackupPath(
      std::string_view token = "0123456789abcdef0123456789abcdef") const {
    return recordRoot / ("app-asar-before-native-" + std::string(token) +
                         ".bak");
  }
  fs::path RollbackPath(
      std::string_view token = "fedcba9876543210fedcba9876543210") const {
    return recordRoot / ("native-rollback-" + std::string(token) + ".bak");
  }

  ShortcutLocations Locations() const {
    ShortcutLocations locations;
    locations.desktop = shortcutRoot / "Desktop";
    locations.startMenu = shortcutRoot / "StartMenu";
    locations.taskbar = shortcutRoot / "Taskbar";
    locations.startup = shortcutRoot / "Startup";
    locations.launcherRoot = launcherRoot;
    locations.notionExe = notionExe;
    locations.launcherExe = launcherRoot / "Notion KEX.exe";
    return locations;
  }

  LauncherOptions Launcher(bool noLaunch = true) const {
    LauncherOptions options;
    options.notionExe = notionExe;
    options.notionData = dataDir;
    options.launcherRoot = launcherRoot;
    options.noLaunch = noLaunch;
    return options;
  }

  void WriteInstallRecord() const {
    WriteJsonFile(InstallRecord(), nlohmann::ordered_json::object());
  }
};

// ---------------------------------------------------------------------------
// Native record and pending-journal helpers
// ---------------------------------------------------------------------------

nlohmann::ordered_json RecordJson(const fs::path& asarPath,
                                  const fs::path& backupPath,
                                  const std::string& originalHash,
                                  const std::string& patchedHash,
                                  const std::string& payloadHash =
                                      std::string(64, 'a')) {
  return nlohmann::ordered_json{
      {"asarPath", Utf8(asarPath)},
      {"backupPath", Utf8(backupPath)},
      {"originalHash", originalHash},
      {"patchedHash", patchedHash},
      {"payloadHash", payloadHash},
      {"installedAt", "2026-11-09T00:00:00.0000000Z"},
  };
}

void WriteNativeRecord(const fs::path& path, const fs::path& asarPath,
                       const fs::path& backupPath,
                       const std::string& originalHash,
                       const std::string& patchedHash,
                       const std::string& payloadHash = std::string(64, 'a')) {
  WriteJsonFile(path,
                RecordJson(asarPath, backupPath, originalHash, patchedHash,
                           payloadHash));
}

struct PendingData {
  std::string previousBytes;
  std::string nextBytes;
  std::string previousHash;
  std::string nextHash;
  fs::path backup;
  fs::path rollback;
  nlohmann::ordered_json previousRecord;
  nlohmann::ordered_json newRecord;
};

PendingData PreparePendingData(Fixture& fixture, std::string previousBytes,
                               std::string nextBytes,
                               bool includePreviousRecord) {
  PendingData data;
  data.previousBytes = std::move(previousBytes);
  data.nextBytes = std::move(nextBytes);
  data.previousHash = Hash(data.previousBytes);
  data.nextHash = Hash(data.nextBytes);
  data.backup = fixture.BackupPath("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  data.rollback = fixture.RollbackPath("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

  WriteFixtureFile(data.backup, data.previousBytes);
  WriteFixtureFile(data.rollback, data.previousBytes);
  data.newRecord = RecordJson(fixture.asar, data.backup, data.previousHash,
                              data.nextHash);
  if (includePreviousRecord) {
    data.previousRecord = RecordJson(fixture.asar, data.backup,
                                     data.previousHash, data.previousHash);
  } else {
    data.previousRecord = nlohmann::ordered_json(nullptr);
  }
  return data;
}

void WritePendingFile(const Fixture& fixture, const PendingData& data) {
  nlohmann::ordered_json pending;
  pending["asarPath"] = Utf8(fixture.asar);
  pending["previousHash"] = data.previousHash;
  pending["nextHash"] = data.nextHash;
  pending["rollbackPath"] = Utf8(data.rollback);
  pending["previousRecord"] = data.previousRecord;
  pending["newRecord"] = data.newRecord;
  WriteJsonFile(fixture.PendingPath(), pending);
}

PendingData SetupPending(Fixture& fixture, bool archiveIsNext,
                         bool includePreviousRecord,
                         std::string previousBytes = {},
                         std::string nextBytes = {}) {
  if (previousBytes.empty()) {
    previousBytes = "previous app archive bytes\n";
  }
  if (nextBytes.empty()) {
    nextBytes = "next app archive bytes\n";
  }
  PendingData data = PreparePendingData(fixture, previousBytes, nextBytes,
                                        includePreviousRecord);
  WriteFixtureFile(fixture.asar,
                   archiveIsNext ? data.nextBytes : data.previousBytes);
  WritePendingFile(fixture, data);
  return data;
}

std::string UpdatedStandardAsar(Fixture& fixture,
                                std::string_view tail = "updated v2") {
  std::vector<AsarMemberSpec> members = StandardMembers();
  members[0].bytes = PreloadSource(tail);
  members[1].bytes = MainSource(tail);
  members.push_back({"assets/updated.txt", "updated asset\n"});
  const fs::path temp = fixture.root / "updated.asar";
  WriteAsarFile(temp, members);
  return ReadFixtureFile(temp);
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

struct ChildProcess {
  PROCESS_INFORMATION info = {};

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : info(other.info) {
    other.info = {};
  }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      Stop();
      info = other.info;
      other.info = {};
    }
    return *this;
  }
  ~ChildProcess() { Stop(); }

  void Stop() {
    if (info.hProcess != nullptr) {
      TerminateProcess(info.hProcess, 1);
      WaitForSingleObject(info.hProcess, 5000);
      CloseHandle(info.hThread);
      CloseHandle(info.hProcess);
      info = {};
    }
  }
};

ChildProcess SpawnSleeper(const fs::path& exe) {
  ChildProcess child;
  std::wstring command =
      QuoteArgument(exe.wstring()) + L" /d /c ping -n 120 127.0.0.1 >nul";
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(),
                      &startup, &child.info)) {
    throw std::runtime_error("Could not start fixture process: " +
                             notion_kex::detail::Win32Error(GetLastError()));
  }
  return child;
}

bool WaitForNotionPath(const fs::path& exe, bool expected, int timeoutMs) {
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
  while (GetTickCount64() < deadline) {
    if (notion_kex::detail::IsNotionRunning(exe) == expected) {
      return true;
    }
    Sleep(50);
  }
  return notion_kex::detail::IsNotionRunning(exe) == expected;
}

struct ProcessOutput {
  int exitCode = -1;
  std::string text;
};

ProcessOutput RunProcess(const fs::path& exe, const std::wstring& arguments) {
  ProcessOutput output;
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
    throw std::runtime_error("Could not create a process pipe.");
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = writePipe;
  startup.hStdError = writePipe;

  std::wstring command = QuoteArgument(exe.wstring());
  if (!arguments.empty()) {
    command += L" " + arguments;
  }
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');

  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                      exe.parent_path().c_str(), &startup, &process)) {
    CloseHandle(readPipe);
    CloseHandle(writePipe);
    throw std::runtime_error("Could not start " + Utf8(exe) + ": " +
                             notion_kex::detail::Win32Error(GetLastError()));
  }
  CloseHandle(writePipe);

  char chunk[4096];
  DWORD read = 0;
  while (ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) &&
         read > 0) {
    output.text.append(chunk, read);
  }
  CloseHandle(readPipe);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  output.exitCode = static_cast<int>(exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return output;
}

fs::path CliPath() {
  return fs::path(WidenUtf8(KEX_CLI_PATH));
}

std::wstring CliArguments(const Fixture& fixture,
                          const std::wstring& modeAndOptions) {
  return modeAndOptions + L" --notion-exe " + QuotePath(fixture.notionExe) +
         L" --notion-data " + QuotePath(fixture.dataDir) +
         L" --launcher-root " + QuotePath(fixture.launcherRoot);
}

ProcessOutput RunCli(const Fixture& fixture, const std::wstring& modeAndOptions) {
  if (!notion_kex::detail::FileExists(CliPath())) {
    throw SkipCase{"The Notion KEX CLI binary is unavailable."};
  }
  return RunProcess(CliPath(), CliArguments(fixture, modeAndOptions));
}

nlohmann::ordered_json ParseJsonOutput(const ProcessOutput& output) {
  const std::string text = Trim(output.text);
  try {
    return nlohmann::ordered_json::parse(text);
  } catch (const std::exception& exception) {
    throw std::runtime_error("CLI output is not JSON: " + text +
                             " (" + exception.what() + ")");
  }
}

// ---------------------------------------------------------------------------
// Native zoom and recovery cases
// ---------------------------------------------------------------------------

struct TestCase {
  std::string name;
  bool passive = false;
  std::function<std::function<void(CaseResult&)>(Fixture&)> setup;
};

std::vector<std::string> ListFiles(const fs::path& root,
                                   std::string_view prefix) {
  std::vector<std::string> names;
  std::error_code error;
  if (!fs::exists(root, error)) {
    return names;
  }
  for (const fs::directory_entry& entry :
       fs::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    const std::string name = Utf8(entry.path().filename());
    if (name.rfind(prefix, 0) == 0) {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::uint8_t> Concat(std::string_view prefix,
                                 const notion_kex::embedded::File& payload) {
  std::vector<std::uint8_t> result(prefix.begin(), prefix.end());
  result.push_back('\n');
  result.insert(result.end(), payload.data, payload.data + payload.size);
  return result;
}

void AddNativeCases(std::vector<TestCase>& cases) {
  cases.push_back({"native-install", false, [](Fixture& fixture) {
                     const std::string original = ReadFixtureFile(fixture.asar);
                     const NativeZoomOutcome outcome =
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, original,
                             outcome](CaseResult& result) {
                       result.Check(outcome.installed, "installed outcome");
                       result.Check(outcome.changed, "changed outcome");
                       result.Contains(outcome.message,
                                       "installed with a verified",
                                       "install message");

                       NativeRecord record;
                       const notion_kex::RecordResult parsed =
                           notion_kex::ReadNativeRecord(fixture.RecordPath(),
                                                        record);
                       result.Check(parsed.ok, "record parses", parsed.error);
                       result.Equal(record.asarPath, Utf8(fixture.asar),
                                    "record asar path");
                       result.Equal(record.originalHash, Hash(original),
                                    "record original hash");
                       const std::string patched = ReadFixtureFile(fixture.asar);
                       result.Equal(record.patchedHash, Hash(patched),
                                    "record patched hash");
                       result.Equal(record.payloadHash,
                                    notion_kex::embedded::PayloadHash(),
                                    "record payload hash");
                       result.Check(!record.installedAt.empty() &&
                                        record.installedAt.find('T') !=
                                            std::string::npos &&
                                        record.installedAt.back() == 'Z',
                                    "record installedAt is UTC");
                       result.Check(record.patchedSize.has_value(),
                                    "record patchedSize present");
                       result.Check(record.patchedWriteTimeUtc.has_value(),
                                    "record patchedWriteTimeUtc present");
                       if (record.patchedSize.has_value() &&
                           record.patchedWriteTimeUtc.has_value()) {
                         const notion_kex::detail::FileInfo info =
                             notion_kex::detail::GetFileInfo(fixture.asar);
                         result.EqualU64(*record.patchedSize, info.size,
                                         "record patched size");
                         result.EqualU64(*record.patchedWriteTimeUtc,
                                         info.writeTime,
                                         "record patched write time");
                       }

                       const fs::path backup =
                           notion_kex::detail::FromUtf8(record.backupPath);
                       result.Check(notion_kex::detail::IsBackupLeafName(backup),
                                    "backup leaf name");
                       result.Equal(ReadFixtureFile(backup), original,
                                    "backup is original bytes");
                       result.Check(notion_kex::detail::IsUnderDirectory(
                                        backup, fixture.recordRoot),
                                    "backup under record root");

                       const AsarArchive archive =
                           notion_kex::ParseAsarFile(fixture.asar);
                       const auto* pinch =
                           notion_kex::embedded::Find("native/pinch.js");
                       const auto* pinchMain =
                           notion_kex::embedded::Find("native/pinch-main.js");
                       result.Check(pinch != nullptr, "preload payload embedded");
                       result.Check(pinchMain != nullptr,
                                    "main payload embedded");
                       if (pinch != nullptr && pinchMain != nullptr) {
                         const std::vector<std::uint8_t> preload =
                             notion_kex::GetAsarMember(
                                 archive, std::string(kPreloadPath));
                         const std::vector<std::uint8_t> main =
                             notion_kex::GetAsarMember(
                                 archive, std::string(kMainPath));
                         result.Check(
                             preload == Concat(PreloadSource(), *pinch),
                             "preload replacement bytes");
                         result.Check(
                             main == Concat(MainSource(), *pinchMain),
                             "main replacement bytes");
                         result.Contains(
                             std::string(preload.begin(), preload.end()),
                             kPreloadMarker, "preload marker");
                         result.Contains(std::string(main.begin(), main.end()),
                                         kMainMarker, "main marker");
                       }

                       result.Check(
                           notion_kex::GetAsarMember(
                               archive, "node_modules/left-pad/index.js") ==
                               Bytes("module.exports = (value, length) => "
                                     "value.padStart(length);\n"),
                           "unrelated member preserved");
                       result.Check(
                           notion_kex::GetAsarMember(archive,
                                                     "package.json") ==
                               Bytes("{\"name\":\"notion-fixture\","
                                     "\"version\":\"1.0.0\"}\n"),
                           "package.json preserved");
                       result.Check(
                           notion_kex::GetAsarMember(archive,
                                                     "assets/blocked.bin") ==
                               Bytes(BlockedBytes()),
                           "blocked member preserved");
                       const auto& blocks =
                           archive.header["files"]["assets"]["files"]
                                         ["blocked.bin"]["integrity"]["blocks"];
                       result.EqualU64(blocks.size(), 3,
                                       "blocked integrity block count");

                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "no pending journal");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot, "native-rollback-")
                               .size(),
                           0, "no rollback files");

                       const StatusResult status = Inspect(fixture.paths);
                       result.Check(status.code == StatusCode::Installed,
                                    "inspect installed",
                                    StatusCodeName(status.code));
                       result.Check(status.installed, "inspect flag");

                       const QuickStatus quick = QuickInspect(fixture.paths);
                       result.Check(quick.recordExists, "quick record exists");
                       result.Check(quick.installed, "quick installed");
                       result.Check(quick.fastPath, "quick fast path");

                       const NativeZoomOutcome validated =
                           ValidateNativeZoom(fixture.paths, fixture.notionExe);
                       result.Check(validated.installed, "validate installed");
                       result.Check(!validated.changed, "validate unchanged");
                     };
                   }});

  cases.push_back({"native-reinstall-current", false, [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const std::string recordBefore =
                         ReadFixtureFile(fixture.RecordPath());
                     const std::string asarBefore =
                         ReadFixtureFile(fixture.asar);
                     const std::vector<std::string> backupsBefore =
                         ListFiles(fixture.recordRoot,
                                   "app-asar-before-native-");
                     const NativeZoomOutcome outcome =
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, recordBefore, asarBefore, backupsBefore,
                             outcome](CaseResult& result) {
                       result.Check(outcome.installed, "installed outcome");
                       result.Check(!outcome.changed, "unchanged outcome");
                       result.Contains(outcome.message, "already matches",
                                       "already current message");
                       result.Equal(ReadFixtureFile(fixture.RecordPath()),
                                    recordBefore, "record unchanged");
                       result.Equal(ReadFixtureFile(fixture.asar), asarBefore,
                                    "archive unchanged");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           backupsBefore.size(), "no new backup");
                     };
                   }});

  cases.push_back({"native-reinstall-refreshes-fastpath", false,
                   [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     const std::string installedAt =
                         record["installedAt"].get<std::string>();
                     record.erase("patchedSize");
                     record.erase("patchedWriteTimeUtc");
                     WriteJsonFile(fixture.RecordPath(), record);
                     const std::vector<std::string> backupsBefore =
                         ListFiles(fixture.recordRoot,
                                   "app-asar-before-native-");
                     const NativeZoomOutcome outcome =
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, installedAt, backupsBefore,
                             outcome](CaseResult& result) {
                       result.Check(outcome.installed, "installed outcome");
                       result.Check(!outcome.changed, "unchanged outcome");
                       const nlohmann::ordered_json updated =
                           ReadJsonFile(fixture.RecordPath());
                       result.Check(updated.contains("patchedSize"),
                                    "patchedSize restored");
                       result.Check(updated.contains("patchedWriteTimeUtc"),
                                    "patchedWriteTimeUtc restored");
                       result.Equal(updated["installedAt"].get<std::string>(),
                                    installedAt, "installedAt preserved");
                       const notion_kex::detail::FileInfo info =
                           notion_kex::detail::GetFileInfo(fixture.asar);
                       result.EqualU64(
                           updated["patchedSize"].get<std::uint64_t>(),
                           info.size, "patchedSize value");
                       result.EqualU64(
                           updated["patchedWriteTimeUtc"]
                               .get<std::uint64_t>(),
                           info.writeTime, "patchedWriteTimeUtc value");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           backupsBefore.size(), "no new backup");
                     };
                   }});

  cases.push_back({"native-reinstall-repairs-fastpath", false,
                   [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     fixture.WriteInstallRecord();
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     record["patchedSize"] = 1;
                     WriteJsonFile(fixture.RecordPath(), record);
                     const LauncherResult outcome =
                         RunLauncher(fixture.Launcher(true));
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "launcher exit code");
                       result.Equal(outcome.message, "Native zoom repaired.",
                                    "launcher message");
                       const nlohmann::ordered_json updated =
                           ReadJsonFile(fixture.RecordPath());
                       const notion_kex::detail::FileInfo info =
                           notion_kex::detail::GetFileInfo(fixture.asar);
                       result.EqualU64(
                           updated["patchedSize"].get<std::uint64_t>(),
                           info.size, "fast path repaired");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed after repair");
                     };
                   }});

  cases.push_back({"native-reinstall-new-payload", false, [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     const std::string backupPath =
                         record["backupPath"].get<std::string>();
                     record["payloadHash"] = std::string(64, 'c');
                     WriteJsonFile(fixture.RecordPath(), record);
                     const NativeZoomOutcome outcome =
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, backupPath,
                             outcome](CaseResult& result) {
                       result.Check(outcome.installed, "installed outcome");
                       result.Check(outcome.changed, "repatch changed");
                       const nlohmann::ordered_json updated =
                           ReadJsonFile(fixture.RecordPath());
                       result.Equal(
                           updated["payloadHash"].get<std::string>(),
                           notion_kex::embedded::PayloadHash(),
                           "payload hash updated");
                       result.Equal(
                           updated["backupPath"].get<std::string>(),
                           backupPath, "backup path reused");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           1, "single backup");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed after repatch");
                     };
                   }});

  cases.push_back({"native-update-repair", false, [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const std::string oldBackup =
                         ReadJsonFile(fixture.RecordPath())["backupPath"]
                             .get<std::string>();
                     const std::string updated = UpdatedStandardAsar(fixture);
                     WriteFixtureFile(fixture.asar, updated);
                     const NativeZoomOutcome outcome =
                         RepairNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, oldBackup, updated,
                             outcome](CaseResult& result) {
                       result.Check(outcome.installed, "repaired outcome");
                       result.Check(outcome.changed, "repair changed");
                       const nlohmann::ordered_json record =
                           ReadJsonFile(fixture.RecordPath());
                       const std::string newBackup =
                           record["backupPath"].get<std::string>();
                       result.Check(newBackup != oldBackup,
                                    "new backup path");
                       result.Check(notion_kex::detail::FileExists(
                                        notion_kex::detail::FromUtf8(
                                            oldBackup)),
                                    "old backup retained");
                       result.Equal(
                           record["originalHash"].get<std::string>(),
                           Hash(updated), "new original hash");
                       result.Equal(
                           record["patchedHash"].get<std::string>(),
                           Hash(ReadFixtureFile(fixture.asar)),
                           "new patched hash");
                       result.Equal(ReadFixtureFile(
                                        notion_kex::detail::FromUtf8(
                                            newBackup)),
                                    updated, "new backup bytes");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           2, "two backups retained");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed after update");
                       result.Check(QuickInspect(fixture.paths).installed,
                                    "quick installed after update");
                     };
                   }});

  cases.push_back({"native-validate-supported", true, [](Fixture& fixture) {
                     const std::string original = ReadFixtureFile(fixture.asar);
                     return [&fixture, original](CaseResult& result) {
                       const NativeZoomOutcome outcome =
                           ValidateNativeZoom(fixture.paths, fixture.notionExe);
                       result.Check(!outcome.installed,
                                    "validate not installed");
                       result.Check(!outcome.changed, "validate unchanged");
                       result.Contains(outcome.message, "supported",
                                       "validate supported message");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "validate archive unchanged");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "validate creates no record root");
                     };
                   }});

  cases.push_back({"native-uninstall-restores", false, [](Fixture& fixture) {
                     const std::string original = ReadFixtureFile(fixture.asar);
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const std::string backup =
                         ReadJsonFile(fixture.RecordPath())["backupPath"]
                             .get<std::string>();
                     const NativeZoomOutcome outcome =
                         UninstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, original, backup,
                             outcome](CaseResult& result) {
                       result.Contains(outcome.message,
                                       "original app archive is restored",
                                       "uninstall message");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive restored exactly");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "record removed");
                       result.Check(notion_kex::detail::FileExists(
                                        notion_kex::detail::FromUtf8(backup)),
                                    "backup retained");
                       result.Check(
                           Inspect(fixture.paths).code ==
                               StatusCode::NotInstalled,
                           "inspect not installed after removal");
                     };
                   }});

  cases.push_back({"native-uninstall-missing-record", false,
                   [](Fixture& fixture) {
                     const NativeZoomOutcome outcome =
                         UninstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, outcome](CaseResult& result) {
                       result.Contains(outcome.message, "no installation record",
                                       "missing record message");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "no record created");
                     };
                   }});

  cases.push_back({"native-uninstall-replaced-archive", false,
                   [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const std::string replacement =
                         UpdatedStandardAsar(fixture, "replacement v3");
                     WriteFixtureFile(fixture.asar, replacement);
                     const NativeZoomOutcome outcome =
                         UninstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, replacement,
                             outcome](CaseResult& result) {
                       result.Contains(outcome.message,
                                       "newer or replaced app archive",
                                       "replaced archive message");
                       result.Equal(ReadFixtureFile(fixture.asar), replacement,
                                    "replacement archive retained");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "record removed");
                     };
                   }});

  cases.push_back({"native-uninstall-edited-patched", false,
                   [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const AsarArchive archive =
                         notion_kex::ParseAsarFile(fixture.asar);
                     const std::vector<std::uint8_t> edited =
                         notion_kex::SetAsarMember(
                             archive, "node_modules/left-pad/index.js",
                             Bytes("module.exports = () => 'edited';\n"));
                     WriteFixtureBytes(fixture.asar, edited);
                     std::string message;
                     try {
                       UninstallNativeZoom(fixture.paths, fixture.notionExe);
                     } catch (const std::exception& exception) {
                       message = exception.what();
                     }
                     return [&fixture, message](CaseResult& result) {
                       result.Contains(message, "edited after native zoom",
                                       "edited patched archive rejected");
                       result.Check(fs::exists(fixture.RecordPath()),
                                    "record retained");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           1, "backup retained");
                     };
                   }});

  cases.push_back({"native-uninstall-missing-archive", false,
                   [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     fs::remove(fixture.asar);
                     const NativeZoomOutcome outcome =
                         UninstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, outcome](CaseResult& result) {
                       result.Contains(outcome.message,
                                       "newer or replaced app archive",
                                       "missing archive message");
                       result.Check(!fs::exists(fixture.asar),
                                    "archive remains missing");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "record removed");
                     };
                   }});

  cases.push_back({"native-fuse-integrity-rejected", true, [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '1');
                     const std::string original = ReadFixtureFile(fixture.asar);
                     return [&fixture, original](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "embedded ASAR integrity",
                                       "fuse integrity message");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive unchanged");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-fuse-missing-rejected", true, [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '0', false);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "could not be verified",
                                       "missing fuse message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-fuse-old-version-rejected", true,
                   [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '0', true, 5);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "Unsupported Electron fuse",
                                       "old fuse version rejected");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-fuse-sentinel-rejected", true,
                   [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '0', true, 9, 2);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "Unsupported Electron fuse",
                                       "wrong fuse sentinel byte rejected");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-marker-rejected", true, [](Fixture& fixture) {
                     std::vector<AsarMemberSpec> members = StandardMembers();
                     members[0].bytes = PreloadSource();
                     members[0].bytes += std::string(kPreloadMarker) + "\n";
                     WriteAsarFile(fixture.asar, members);
                     const std::string original = ReadFixtureFile(fixture.asar);
                     return [&fixture, original](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "unrecorded native zoom patch",
                                       "marker rejection message");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive unchanged");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-contract-rejected", true, [](Fixture& fixture) {
                     std::vector<AsarMemberSpec> members = StandardMembers();
                     members[0].bytes = PreloadSource();
                     members[0].bytes.erase(
                         members[0].bytes.find("__isElectron"), 13);
                     WriteAsarFile(fixture.asar, members);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "native entry changed",
                                       "contract rejection message");
                       result.Contains(message, "__isElectron",
                                       "contract name in message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-invalid-utf8-rejected", true, [](Fixture& fixture) {
                     std::vector<AsarMemberSpec> members = StandardMembers();
                     std::string invalid = PreloadSource();
                     invalid.push_back(static_cast<char>(0xFF));
                     members[0].bytes = invalid;
                     WriteAsarFile(fixture.asar, members);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "native entry changed",
                                       "invalid utf8 message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-missing-member-rejected", true,
                   [](Fixture& fixture) {
                     std::vector<AsarMemberSpec> members = StandardMembers();
                     members.erase(members.begin());
                     WriteAsarFile(fixture.asar, members);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "Required packed ASAR member",
                                       "missing member message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-missing-archive-rejected", true,
                   [](Fixture& fixture) {
                     fs::remove(fixture.asar);
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "app archive is missing",
                                       "missing archive message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"native-invalid-record-rejected", true,
                   [](Fixture& fixture) {
                     WriteFixtureFile(fixture.RecordPath(),
                                      "{ \"asarPath\": ");
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "not valid JSON",
                                       "invalid record message");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "no pending journal");
                     };
                   }});

  cases.push_back({"native-wrong-record-rejected", false, [](Fixture& fixture) {
                     const fs::path backup = fixture.BackupPath();
                     WriteFixtureFile(backup, "backup bytes");
                     WriteNativeRecord(fixture.RecordPath(),
                                       fixture.root / "Other" / "app.asar",
                                       backup, Hash("backup bytes"),
                                       Hash("patched"));
                     std::string wrongPathMessage;
                     try {
                       InstallNativeZoom(fixture.paths, fixture.notionExe);
                     } catch (const std::exception& exception) {
                       wrongPathMessage = exception.what();
                     }
                     WriteNativeRecord(fixture.RecordPath(), fixture.asar,
                                       backup, Hash("backup bytes"),
                                       Hash("patched"));
                     std::string missingBackupMessage;
                     fs::remove(backup);
                     try {
                       InstallNativeZoom(fixture.paths, fixture.notionExe);
                     } catch (const std::exception& exception) {
                       missingBackupMessage = exception.what();
                     }
                     return [&fixture, wrongPathMessage,
                             missingBackupMessage](CaseResult& result) {
                       result.Contains(wrongPathMessage,
                                       "different Notion installation",
                                       "wrong record asar");
                       result.Contains(missingBackupMessage,
                                       "backup is missing or changed",
                                       "missing record backup");
                     };
                   }});

  cases.push_back({"native-process-closed", false, [](Fixture& fixture) {
                     const fs::path cmd = ComSpec();
                     if (!notion_kex::detail::FileExists(cmd)) {
                       throw SkipCase{"cmd.exe is unavailable."};
                     }
                     notion_kex::detail::CopyFileTo(cmd, fixture.notionExe);
                     auto child = std::make_shared<ChildProcess>(
                         SpawnSleeper(fixture.notionExe));
                     if (!WaitForNotionPath(fixture.notionExe, true, 8000)) {
                       throw SkipCase{"The fixture Notion process did not start."};
                     }
                     std::string closedMessage;
                     try {
                       notion_kex::detail::AssertNotionClosed(
                           fixture.notionExe, kClosedMessage);
                     } catch (const std::exception& exception) {
                       closedMessage = exception.what();
                     }
                     std::string installMessage;
                     try {
                       InstallNativeZoom(fixture.paths, fixture.notionExe);
                     } catch (const std::exception& exception) {
                       installMessage = exception.what();
                     }
                     return [&fixture, child, closedMessage,
                             installMessage](CaseResult& result) {
                       result.Equal(closedMessage, std::string(kClosedMessage),
                                    "closed assertion message");
                       result.Equal(installMessage, std::string(kClosedMessage),
                                    "install refuses while running");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root while running");
                       child->Stop();
                       result.Check(!notion_kex::detail::IsNotionRunning(
                                        fixture.notionExe),
                                    "fixture process stopped");
                     };
                   }});

  cases.push_back({"native-process-path-filter", false, [](Fixture& fixture) {
                     const fs::path cmd = ComSpec();
                     if (!notion_kex::detail::FileExists(cmd)) {
                       throw SkipCase{"cmd.exe is unavailable."};
                     }
                     const fs::path otherDir = fixture.root / "Other";
                     std::error_code error;
                     fs::create_directories(otherDir, error);
                     if (error) {
                       throw std::runtime_error("Could not create other dir.");
                     }
                     const fs::path otherExe = otherDir / "Notion.exe";
                     notion_kex::detail::CopyFileTo(cmd, otherExe);
                     auto child = std::make_shared<ChildProcess>(
                         SpawnSleeper(otherExe));
                     if (!WaitForNotionPath(otherExe, true, 8000)) {
                       throw SkipCase{"The other fixture process did not start."};
                     }
                     return [&fixture, child,
                             otherExe](CaseResult& result) {
                       result.Check(
                           notion_kex::detail::IsNotionRunning(otherExe),
                           "other path is running");
                       result.Check(
                           !notion_kex::detail::IsNotionRunning(fixture.notionExe),
                           "fixture path is not running");
                       result.Check(notion_kex::detail::IsAnyNotionRunning(),
                                    "any notion running");
                       child->Stop();
                     };
                   }});

  cases.push_back({"native-mutex-busy", false, [](Fixture& fixture) {
                     const std::string name = notion_kex::detail::MakeMutexName(
                         "Local\\NotionKexNativeZoom", fixture.paths.asarPath);
                     notion_kex::detail::MutexGuard guard(name,
                                                          std::string(kBusyMessage));
                     std::string message;
                     std::thread worker([&]() {
                       try {
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                     });
                     worker.join();
                     return [&fixture, message](CaseResult& result) {
                       result.Equal(message, std::string(kBusyMessage),
                                    "busy message");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root while busy");
                     };
                   }});
}

void AddRecoveryCases(std::vector<TestCase>& cases) {
  cases.push_back({"recovery-commits-next", false, [](Fixture& fixture) {
                     const PendingData data =
                         SetupPending(fixture, true, false);
                     RecoverNativeZoom(fixture.paths);
                     return [&fixture, data](CaseResult& result) {
                       result.Check(fs::exists(fixture.RecordPath()),
                                    "record committed");
                       const nlohmann::ordered_json record =
                           ReadJsonFile(fixture.RecordPath());
                       result.Equal(
                           record["patchedHash"].get<std::string>(),
                           data.nextHash, "committed next hash");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "pending removed");
                       result.Check(!fs::exists(data.rollback),
                                    "rollback removed");
                       result.Check(fs::exists(data.backup),
                                    "backup retained");
                     };
                   }});

  cases.push_back({"recovery-restores-previous-record", false,
                   [](Fixture& fixture) {
                     const PendingData data =
                         SetupPending(fixture, false, true);
                     RecoverNativeZoom(fixture.paths);
                     return [&fixture, data](CaseResult& result) {
                       result.Check(fs::exists(fixture.RecordPath()),
                                    "previous record restored");
                       const nlohmann::ordered_json record =
                           ReadJsonFile(fixture.RecordPath());
                       result.Equal(
                           record["patchedHash"].get<std::string>(),
                           data.previousHash, "previous patched hash");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "pending removed");
                       result.Check(!fs::exists(data.rollback),
                                    "rollback removed");
                       result.Equal(ReadFixtureFile(fixture.asar),
                                    data.previousBytes,
                                    "previous archive preserved");
                     };
                   }});

  cases.push_back({"recovery-restores-missing-record", false,
                   [](Fixture& fixture) {
                     const PendingData data =
                         SetupPending(fixture, false, false);
                     RecoverNativeZoom(fixture.paths);
                     return [&fixture, data](CaseResult& result) {
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "no record after rollback");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "pending removed");
                       result.Check(!fs::exists(data.rollback),
                                    "rollback removed");
                     };
                   }});

  cases.push_back({"recovery-conflict-retains", false, [](Fixture& fixture) {
                     PendingData data =
                         PreparePendingData(fixture, "previous bytes\n",
                                            "next bytes\n", false);
                     WriteFixtureFile(fixture.asar, "conflicting bytes\n");
                     WritePendingFile(fixture, data);
                     std::string message;
                     try {
                       RecoverNativeZoom(fixture.paths);
                     } catch (const std::exception& exception) {
                       message = exception.what();
                     }
                     return [&fixture, data, message](CaseResult& result) {
                       result.Contains(message,
                                       "changed during an interrupted",
                                       "conflict message");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                       result.Check(fs::exists(data.rollback),
                                    "rollback retained");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "no guessed record");
                     };
                   }});

  cases.push_back({"recovery-malformed-retains", false, [](Fixture& fixture) {
                     WriteFixtureFile(fixture.PendingPath(), "{ not json");
                     std::string message;
                     try {
                       RecoverNativeZoom(fixture.paths);
                     } catch (const std::exception& exception) {
                       message = exception.what();
                     }
                     return [&fixture, message](CaseResult& result) {
                       result.Contains(message, "not valid JSON",
                                       "malformed pending message");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                     };
                   }});

  cases.push_back({"recovery-missing-rollback-retains", false,
                   [](Fixture& fixture) {
                     const PendingData data =
                         SetupPending(fixture, true, false);
                     fs::remove(data.rollback);
                     std::string message;
                     try {
                       RecoverNativeZoom(fixture.paths);
                     } catch (const std::exception& exception) {
                       message = exception.what();
                     }
                     return [&fixture, message](CaseResult& result) {
                       result.Contains(message,
                                       "rollback backup is missing or changed",
                                       "missing rollback message");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained");
                     };
                   }});

  cases.push_back({"recovery-install-recovers-then-installs", false,
                   [](Fixture& fixture) {
                     const std::string previous =
                         ReadFixtureFile(fixture.asar);
                     const std::string next =
                         UpdatedStandardAsar(fixture, "pending next");
                     const PendingData data = PreparePendingData(
                         fixture, previous, next, false);
                     WritePendingFile(fixture, data);
                     const NativeZoomOutcome outcome =
                         InstallNativeZoom(fixture.paths, fixture.notionExe);
                     return [&fixture, outcome](CaseResult& result) {
                       result.Check(outcome.installed, "install outcome");
                       result.Check(outcome.changed, "install changed");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "pending recovered");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed");
                     };
                   }});

  cases.push_back({"recovery-status-is-passive", true, [](Fixture& fixture) {
                     const std::string previous =
                         ReadFixtureFile(fixture.asar);
                     const std::string next =
                         UpdatedStandardAsar(fixture, "status pending");
                     const PendingData data = PreparePendingData(
                         fixture, previous, next, false);
                     WriteFixtureFile(fixture.asar, next);
                     WritePendingFile(fixture, data);
                     return [&fixture, data](CaseResult& result) {
                       const StatusResult status = Inspect(fixture.paths);
                       result.Check(status.code == StatusCode::RecoveryRequired,
                                    "status recovery required",
                                    StatusCodeName(status.code));
                       result.Check(!status.installed, "status not installed");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained by status");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "record not committed by status");
                     };
                   }});

  cases.push_back({"recovery-quick-is-passive", true, [](Fixture& fixture) {
                     const std::string previous =
                         ReadFixtureFile(fixture.asar);
                     const std::string next =
                         UpdatedStandardAsar(fixture, "quick pending");
                     const PendingData data = PreparePendingData(
                         fixture, previous, next, false);
                     WriteFixtureFile(fixture.asar, next);
                     WritePendingFile(fixture, data);
                     return [&fixture, data](CaseResult& result) {
                       const QuickStatus quick = QuickInspect(fixture.paths);
                       result.Check(quick.recordExists,
                                    "quick sees pending");
                       result.Check(!quick.installed, "quick not installed");
                       result.Contains(quick.message, "requires recovery",
                                       "quick recovery message");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained by quick");
                     };
                   }});

  cases.push_back({"recovery-validate-is-passive", true, [](Fixture& fixture) {
                     const std::string previous =
                         ReadFixtureFile(fixture.asar);
                     const std::string next =
                         UpdatedStandardAsar(fixture, "validate pending");
                     const PendingData data = PreparePendingData(
                         fixture, previous, next, false);
                     WriteFixtureFile(fixture.asar, next);
                     WritePendingFile(fixture, data);
                     return [&fixture, data](CaseResult& result) {
                       const NativeZoomOutcome outcome =
                           ValidateNativeZoom(fixture.paths, fixture.notionExe);
                       result.Check(!outcome.changed, "validate unchanged");
                       result.Contains(outcome.message, "supported",
                                       "validate supported");
                       result.Check(fs::exists(fixture.PendingPath()),
                                    "pending retained by validate");
                     };
                   }});
}

// ---------------------------------------------------------------------------
// Extension, shortcut, launcher, CLI, embedded and record cases
// ---------------------------------------------------------------------------

fs::path ModulePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD size =
      GetModuleFileNameW(nullptr, buffer.data(),
                         static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    throw std::runtime_error("The test executable path could not be read.");
  }
  return fs::path(std::wstring(buffer.data(), size));
}

void InstallFixtureAndRecord(Fixture& fixture) {
  InstallNativeZoom(fixture.paths, fixture.notionExe);
  fixture.WriteInstallRecord();
}

void AddExtensionCases(std::vector<TestCase>& cases) {
  cases.push_back({"extension-install", false, [](Fixture& fixture) {
                     const std::string stateBefore =
                         ReadFixtureFile(fixture.StatePath());
                     const std::string asarBefore =
                         ReadFixtureFile(fixture.asar);
                     const SetupOutcome outcome =
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture, stateBefore, asarBefore,
                             outcome](CaseResult& result) {
                       result.Check(outcome.changed, "install changed");
                       result.Contains(outcome.message, "1.1.2 installed in",
                                       "install message");
                       result.Contains(outcome.message,
                                       "installed with a verified",
                                       "native install message");
                       result.Check(!fs::exists(fixture.PendingPath()),
                                    "no pending journal");

                       for (const char* path : kExtensionFiles) {
                         const notion_kex::embedded::File* file =
                             notion_kex::embedded::Find(path);
                         result.Check(file != nullptr,
                                      std::string("embedded ") + path);
                         if (file == nullptr) {
                           continue;
                         }
                         const fs::path leaf =
                             fs::path(path).filename();
                         const std::vector<std::uint8_t> installed =
                             ReadFixtureBytes(fixture.InstallRoot() / leaf);
                         result.Check(
                             installed ==
                                 std::vector<std::uint8_t>(
                                     file->data, file->data + file->size),
                             std::string("installed bytes ") + path);
                       }
                       result.Equal(
                           ReadFixtureFile(fixture.InstallRoot() /
                                           ".notion-kex-owned"),
                           "Notion KEX installer owns this folder.",
                           "ownership marker");

                       const nlohmann::ordered_json record =
                           ReadJsonFile(fixture.InstallRecord());
                       result.Equal(record["version"].get<std::string>(),
                                    "1.1.2", "record version");
                       result.Check(record.contains("installedAt"),
                                    "record installedAt");
                       result.Check(record.contains("stateBackup"),
                                    "record state backup");
                       const fs::path stateBackup =
                           notion_kex::detail::FromUtf8(
                               record["stateBackup"].get<std::string>());
                       result.Equal(ReadFixtureFile(stateBackup), stateBefore,
                                    "state backup bytes");
                       result.EqualU64(record["files"].size(), 7,
                                       "record file count");
                       for (const char* path : kExtensionFiles) {
                         const fs::path leaf = fs::path(path).filename();
                         const std::string expected =
                             HashVector(ReadFixtureBytes(
                                 fixture.InstallRoot() / leaf));
                         result.Equal(
                             record["files"][Utf8(leaf)].get<std::string>(),
                             expected,
                             std::string("record hash ") + Utf8(leaf));
                       }
                       const auto& before =
                           record["preferencesBefore"];
                       result.Check(
                           before["isChromeExtensionsEnabled"]["existed"]
                               .get<bool>(),
                           "chrome preference existed");
                       result.Check(
                           !before["isChromeExtensionsEnabled"]["value"]
                                .get<bool>(),
                           "chrome preference value");
                       result.Check(
                           before["isDebugMenuEnabled"]["existed"]
                               .get<bool>(),
                           "debug preference existed");
                       result.Check(
                           !before["isDebugMenuEnabled"]["value"].get<bool>(),
                           "debug preference value");

                       const nlohmann::ordered_json state =
                           ReadJsonFile(fixture.StatePath());
                       const auto& preferences =
                           state["appState"]["preferences"];
                       result.Check(
                           preferences["isChromeExtensionsEnabled"]
                               .get<bool>(),
                           "chrome enabled");
                       result.Check(
                           preferences["isDebugMenuEnabled"].get<bool>(),
                           "debug enabled");
                       result.Equal(
                           preferences["disabledExtensions"].dump(),
                           nlohmann::ordered_json::array(
                               {"Other Extension"}).dump(),
                           "only KEX re-enabled");
                       result.Equal(preferences["theme"].get<std::string>(),
                                    "dark", "theme preserved");
                       result.Equal(state["appState"]["other"]
                                        .get<std::string>(),
                                    "kept", "other state preserved");
                       result.NotEqual(ReadFixtureFile(fixture.asar), asarBefore,
                                       "archive patched");

                       const notion_kex::ExtensionStatus status =
                           InspectExtension(fixture.notionExe,
                                            fixture.dataDir);
                       result.Check(status.installed, "status installed");
                       result.Check(status.healthy, "status healthy",
                                    status.problems.empty()
                                        ? ""
                                        : status.problems.front());
                       result.Equal(status.version, "1.1.2",
                                    "status version");
                       result.Check(status.nativeInstalled,
                                    "status native installed");
                       result.EqualU64(status.files.size(), 7,
                                       "status file count");
                       for (const auto& file : status.files) {
                         result.Check(file.present && file.hashMatches,
                                      "status file " + file.name);
                       }
                       result.Check(Inspect(fixture.paths).installed,
                                    "native inspect installed");
                     };
                   }});

  cases.push_back({"extension-status-healthy", true, [](Fixture& fixture) {
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture](CaseResult& result) {
                       const notion_kex::ExtensionStatus status =
                           InspectExtension(fixture.notionExe,
                                            fixture.dataDir);
                       result.Check(status.installed, "installed");
                       result.Check(status.healthy, "healthy");
                       result.Check(status.nativeInstalled, "native installed");
                       result.Check(status.problems.empty(), "no problems",
                                    status.problems.empty()
                                        ? ""
                                        : status.problems.front());
                     };
                   }});

  cases.push_back({"extension-status-detects-changes", true,
                   [](Fixture& fixture) {
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     WriteFixtureFile(fixture.InstallRoot() / "kex.js",
                                      "// changed after install\n");
                     return [&fixture](CaseResult& result) {
                       const notion_kex::ExtensionStatus status =
                           InspectExtension(fixture.notionExe,
                                            fixture.dataDir);
                       result.Check(status.installed, "installed");
                       result.Check(!status.healthy, "unhealthy");
                       bool found = false;
                       for (const std::string& problem : status.problems) {
                         if (problem.find("kex.js") != std::string::npos) {
                           found = true;
                         }
                       }
                       result.Check(found, "changed file reported");
                     };
                   }});

  cases.push_back({"extension-uninstall-restores-preferences", false,
                   [](Fixture& fixture) {
                     WriteStateFile(fixture.StatePath(), MakeState(false, false));
                     const std::string original =
                         ReadFixtureFile(fixture.asar);
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     const std::string backup =
                         ReadJsonFile(fixture.RecordPath())["backupPath"]
                             .get<std::string>();
                     const SetupOutcome outcome =
                         UninstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture, original, backup,
                             outcome](CaseResult& result) {
                       result.Check(outcome.changed, "uninstall changed");
                       result.Contains(outcome.message, "removed",
                                       "uninstall message");
                       const nlohmann::ordered_json state =
                           ReadJsonFile(fixture.StatePath());
                       const auto& preferences =
                           state["appState"]["preferences"];
                       result.Check(
                           !preferences["isChromeExtensionsEnabled"]
                                .get<bool>(),
                           "chrome restored");
                       result.Check(
                           !preferences["isDebugMenuEnabled"].get<bool>(),
                           "debug restored");
                       result.Equal(
                           preferences["disabledExtensions"].dump(),
                           nlohmann::ordered_json::array(
                               {"Other Extension"}).dump(),
                           "disabled extensions retained");
                       result.Equal(preferences["theme"].get<std::string>(),
                                    "dark", "theme retained");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "extension folder removed");
                       result.Check(!fs::exists(fixture.InstallRecord()),
                                    "install record removed");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "native record removed");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive restored");
                       result.Check(notion_kex::detail::FileExists(
                                        notion_kex::detail::FromUtf8(backup)),
                                    "native backup retained");
                     };
                   }});

  cases.push_back({"extension-uninstall-keeps-extensions-enabled", false,
                   [](Fixture& fixture) {
                     WriteStateFile(fixture.StatePath(), MakeState(false, false));
                     WriteFixtureFile(
                         fixture.dataDir / "extensions" / "Other" /
                             "manifest.json",
                         "{\"name\":\"Other\"}\n");
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     UninstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture](CaseResult& result) {
                       const nlohmann::ordered_json state =
                           ReadJsonFile(fixture.StatePath());
                       const auto& preferences =
                           state["appState"]["preferences"];
                       result.Check(
                           preferences["isChromeExtensionsEnabled"]
                               .get<bool>(),
                           "chrome stays enabled for other extensions");
                       result.Check(
                           !preferences["isDebugMenuEnabled"].get<bool>(),
                           "debug restored");
                       result.Check(
                           fs::exists(fixture.dataDir / "extensions" / "Other"),
                           "other extension retained");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "KEX extension removed");
                     };
                   }});

  cases.push_back({"extension-uninstall-absent-preferences", false,
                   [](Fixture& fixture) {
                     nlohmann::ordered_json state = MakeState(false, false);
                     state["appState"]["preferences"].erase(
                         "isChromeExtensionsEnabled");
                     state["appState"]["preferences"].erase(
                         "isDebugMenuEnabled");
                     WriteStateFile(fixture.StatePath(), state);
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     UninstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture](CaseResult& result) {
                       const nlohmann::ordered_json stateAfter =
                           ReadJsonFile(fixture.StatePath());
                       const auto& preferences =
                           stateAfter["appState"]["preferences"];
                       result.Check(
                           !preferences.contains("isChromeExtensionsEnabled"),
                           "absent chrome preference removed");
                       result.Check(
                           !preferences.contains("isDebugMenuEnabled"),
                           "absent debug preference removed");
                       result.Equal(
                           preferences["theme"].get<std::string>(), "dark",
                           "theme retained");
                     };
                   }});

  cases.push_back({"extension-install-missing-state", true,
                   [](Fixture& fixture) {
                     fs::remove(fixture.StatePath());
                     const std::string original =
                         ReadFixtureFile(fixture.asar);
                     return [&fixture, original](CaseResult& result) {
                       std::string message;
                       try {
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "Start Notion once",
                                       "missing state message");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive unchanged");
                     };
                   }});

  cases.push_back({"extension-install-unsupported-state", true,
                   [](Fixture& fixture) {
                     WriteStateFile(fixture.StatePath(),
                                    nlohmann::ordered_json{{"appState", {}}});
                     const std::string stateBefore =
                         ReadFixtureFile(fixture.StatePath());
                     return [&fixture, stateBefore](CaseResult& result) {
                       std::string message;
                       try {
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "unsupported",
                                       "unsupported state message");
                       result.Equal(ReadFixtureFile(fixture.StatePath()),
                                    stateBefore, "state unchanged");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                     };
                   }});

  cases.push_back({"extension-install-unowned-directory", true,
                   [](Fixture& fixture) {
                     WriteFixtureFile(fixture.InstallRoot() / "foreign.txt",
                                      "foreign\n");
                     const std::string stateBefore =
                         ReadFixtureFile(fixture.StatePath());
                     return [&fixture, stateBefore](CaseResult& result) {
                       std::string message;
                       try {
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "unrecognized extension",
                                       "unowned directory message");
                       result.Check(
                           fs::exists(fixture.InstallRoot() / "foreign.txt"),
                           "foreign file retained");
                       result.Equal(ReadFixtureFile(fixture.StatePath()),
                                    stateBefore, "state unchanged");
                     };
                   }});

  cases.push_back({"extension-install-fuse-rejected", true,
                   [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '1');
                     const std::string stateBefore =
                         ReadFixtureFile(fixture.StatePath());
                     const std::string asarBefore =
                         ReadFixtureFile(fixture.asar);
                     return [&fixture, stateBefore,
                             asarBefore](CaseResult& result) {
                       std::string message;
                       try {
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "embedded ASAR integrity",
                                       "fuse message");
                       result.Equal(ReadFixtureFile(fixture.StatePath()),
                                    stateBefore, "state unchanged");
                       result.Equal(ReadFixtureFile(fixture.asar), asarBefore,
                                    "archive unchanged");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "no record root");
                     };
                   }});

  cases.push_back({"extension-install-loader-changed", true,
                   [](Fixture& fixture) {
                     std::vector<AsarMemberSpec> members = StandardMembers();
                     members[1].bytes = MainSource();
                     members[1].bytes.erase(
                         members[1].bytes.find("loadExtension"), 13);
                     WriteAsarFile(fixture.asar, members);
                     const std::string stateBefore =
                         ReadFixtureFile(fixture.StatePath());
                     return [&fixture, stateBefore](CaseResult& result) {
                       std::string message;
                       try {
                         InstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message, "loader changed",
                                       "loader contract message");
                       result.Equal(ReadFixtureFile(fixture.StatePath()),
                                    stateBefore, "state unchanged");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                     };
                   }});

  cases.push_back({"extension-uninstall-missing-record", true,
                   [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       std::string message;
                       try {
                         UninstallExtension(fixture.notionExe, fixture.dataDir);
                       } catch (const std::exception& exception) {
                         message = exception.what();
                       }
                       result.Contains(message,
                                       "No Notion KEX installation record",
                                       "missing install record message");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                     };
                   }});

  cases.push_back({"extension-reinstall-preserves-record", false,
                   [](Fixture& fixture) {
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     const nlohmann::ordered_json before =
                         ReadJsonFile(fixture.InstallRecord());
                     const std::string installedAt =
                         before["installedAt"].get<std::string>();
                     const std::string stateBackup =
                         before["stateBackup"].get<std::string>();
                     nlohmann::ordered_json state =
                         ReadJsonFile(fixture.StatePath());
                     state["appState"]["preferences"]["theme"] = "light";
                     WriteJsonFile(fixture.StatePath(), state);
                     InstallExtension(fixture.notionExe, fixture.dataDir);
                     return [&fixture, installedAt,
                             stateBackup](CaseResult& result) {
                       const nlohmann::ordered_json after =
                           ReadJsonFile(fixture.InstallRecord());
                       result.Equal(after["installedAt"].get<std::string>(),
                                    installedAt, "installedAt preserved");
                       result.Equal(after["stateBackup"].get<std::string>(),
                                    stateBackup, "state backup preserved");
                       result.Equal(
                           ReadJsonFile(fixture.StatePath())["appState"]
                                       ["preferences"]["theme"]
                                           .get<std::string>(),
                           "light", "theme preserved");
                       result.EqualU64(
                           ListFiles(fixture.recordRoot,
                                     "app-asar-before-native-")
                               .size(),
                           1, "single native backup");
                     };
                   }});
}

void AddShortcutCases(std::vector<TestCase>& cases) {
  cases.push_back({"shortcuts-install-repoints", false, [](Fixture& fixture) {
                     const ShortcutLocations locations = fixture.Locations();
                     const fs::path powershell =
                         fs::path(L"C:\\Windows\\System32\\WindowsPowerShell\\"
                                  L"v1.0\\powershell.exe");
                     const fs::path powershellTarget =
                         notion_kex::detail::FileExists(powershell)
                             ? powershell
                             : ComSpec();
                     const fs::path desktopNotion =
                         locations.desktop / "Notion.lnk";
                     const fs::path startMenuOld =
                         locations.startMenu / "Old Notion.lnk";
                     const fs::path taskbarNotion =
                         locations.taskbar / "Notion Pinned.lnk";
                     const fs::path customNotion =
                         locations.desktop / "Notion Custom.lnk";
                     const fs::path other =
                         locations.desktop / "Other.lnk";
                     CreateTestShortcut(desktopNotion, fixture.notionExe, L"",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     CreateTestShortcut(
                         startMenuOld, powershellTarget,
                         L"-NoProfile -File \"C:\\Users\\x\\AppData\\Local\\"
                         L"NotionKEX\\tools\\Launch.ps1\"",
                         powershellTarget.parent_path(), powershellTarget);
                     CreateTestShortcut(taskbarNotion, fixture.notionExe, L"",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     CreateTestShortcut(customNotion, fixture.notionExe,
                                        L"--profile work",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     CreateTestShortcut(other, ComSpec(), L"",
                                        ComSpec().parent_path(), ComSpec());

                     std::map<std::string, std::string> originals;
                     for (const fs::path& path :
                          {desktopNotion, startMenuOld, taskbarNotion,
                           customNotion, other}) {
                       originals[Utf8(path)] = ReadFixtureFile(path);
                     }

                     notion_kex::InstallLauncherExecutable(
                         fixture.launcherRoot);
                     const ShortcutOutcome outcome =
                         InstallShortcuts(locations);
                     return [&fixture, locations, outcome, originals,
                             powershellTarget](CaseResult& result) {
                       result.EqualU64(outcome.updated, 3,
                                       "updated shortcut count");
                       result.EqualU64(outcome.created, 2,
                                       "created shortcut count");
                       result.Contains(outcome.message, "Updated 3",
                                       "shortcut message");

                       const ShortcutInfo desktop = ReadTestShortcut(
                           locations.desktop / "Notion.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(desktop.target),
                               locations.launcherExe),
                           "desktop repointed");
                       result.Check(desktop.arguments.empty(),
                                    "desktop arguments cleared");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(desktop.workingDirectory),
                               locations.launcherRoot),
                           "desktop working directory");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(desktop.icon), fixture.notionExe),
                           "desktop icon");
                       result.Equal(NarrowUtf8(desktop.appId),
                                    "com.electron.notion", "desktop AUMID");

                       const ShortcutInfo old = ReadTestShortcut(
                           locations.startMenu / "Old Notion.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(old.target), locations.launcherExe),
                           "old wrapper repointed");

                       const ShortcutInfo pinned = ReadTestShortcut(
                           locations.taskbar / "Notion Pinned.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(pinned.target),
                               locations.launcherExe),
                           "taskbar shortcut repointed");

                       const ShortcutInfo custom = ReadTestShortcut(
                           locations.desktop / "Notion Custom.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(custom.target), fixture.notionExe),
                           "custom shortcut untouched");
                       result.Equal(NarrowUtf8(custom.arguments),
                                    "--profile work",
                                    "custom arguments untouched");

                       const ShortcutInfo unrelated = ReadTestShortcut(
                           locations.desktop / "Other.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(unrelated.target), ComSpec()),
                           "unrelated shortcut untouched");

                       const fs::path desktopLauncher =
                           locations.desktop / "Notion KEX.lnk";
                       const fs::path startupLauncher =
                           locations.startup / "Notion KEX.lnk";
                       result.Check(fs::exists(desktopLauncher),
                                    "desktop launcher shortcut created");
                       result.Check(fs::exists(startupLauncher),
                                    "startup launcher shortcut created");
                       const ShortcutInfo desktopLink =
                           ReadTestShortcut(desktopLauncher);
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(desktopLink.target),
                               locations.launcherExe),
                           "desktop launcher target");
                       result.Equal(NarrowUtf8(desktopLink.appId),
                                    "com.electron.notion",
                                    "desktop launcher AUMID");
                       const ShortcutInfo startupLink =
                           ReadTestShortcut(startupLauncher);
                       result.Equal(NarrowUtf8(startupLink.arguments),
                                    "--signin-check",
                                    "startup sign-in arguments");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(startupLink.target),
                               locations.launcherExe),
                           "startup launcher target");

                       const nlohmann::ordered_json manifest = ReadJsonFile(
                           fixture.launcherRoot / "shortcuts.json");
                       result.EqualU64(manifest["shortcuts"].size(), 3,
                                       "manifest shortcut count");
                       result.EqualU64(manifest["created"].size(), 2,
                                       "manifest created count");
                       for (const auto& entry : manifest["shortcuts"]) {
                         const std::string path =
                             entry["path"].get<std::string>();
                         const std::string backup =
                             entry["backup"].get<std::string>();
                         result.Check(
                             notion_kex::detail::FileExists(
                                 notion_kex::detail::FromUtf8(backup)),
                             "backup exists for " + path);
                         result.Equal(
                             ReadFixtureFile(
                                 notion_kex::detail::FromUtf8(backup)),
                             originals.at(path), "backup bytes for " + path);
                       }
                     };
                   }});

  cases.push_back({"shortcuts-reinstall-idempotent", false,
                   [](Fixture& fixture) {
                     const ShortcutLocations locations = fixture.Locations();
                     CreateTestShortcut(locations.desktop / "Notion.lnk",
                                        fixture.notionExe, L"",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     notion_kex::InstallLauncherExecutable(
                         fixture.launcherRoot);
                     InstallShortcuts(locations);
                     const std::string manifestBefore = ReadFixtureFile(
                         fixture.launcherRoot / "shortcuts.json");
                     const std::size_t backupsBefore =
                         ListFiles(fixture.launcherRoot, "shortcut-").size();
                     const ShortcutOutcome outcome =
                         InstallShortcuts(locations);
                     return [&fixture, manifestBefore, backupsBefore,
                             outcome](CaseResult& result) {
                       result.EqualU64(outcome.updated, 0,
                                       "no updates on reinstall");
                       result.EqualU64(outcome.created, 0,
                                       "no creations on reinstall");
                       result.Equal(
                           ReadFixtureFile(fixture.launcherRoot /
                                           "shortcuts.json"),
                           manifestBefore, "manifest unchanged");
                       result.EqualU64(
                           ListFiles(fixture.launcherRoot, "shortcut-").size(),
                           backupsBefore, "no duplicate backups");
                     };
                   }});

  cases.push_back({"shortcuts-uninstall-restores", false, [](Fixture& fixture) {
                     const ShortcutLocations locations = fixture.Locations();
                     const fs::path powershell =
                         fs::path(L"C:\\Windows\\System32\\WindowsPowerShell\\"
                                  L"v1.0\\powershell.exe");
                     const fs::path powershellTarget =
                         notion_kex::detail::FileExists(powershell)
                             ? powershell
                             : ComSpec();
                     const fs::path desktopNotion =
                         locations.desktop / "Notion.lnk";
                     const fs::path startMenuOld =
                         locations.startMenu / "Old Notion.lnk";
                     const fs::path taskbarNotion =
                         locations.taskbar / "Notion Pinned.lnk";
                     CreateTestShortcut(desktopNotion, fixture.notionExe, L"",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     CreateTestShortcut(
                         startMenuOld, powershellTarget,
                         L"-NoProfile -File \"C:\\Users\\x\\AppData\\Local\\"
                         L"NotionKEX\\tools\\Launch.ps1\"",
                         powershellTarget.parent_path(), powershellTarget);
                     CreateTestShortcut(taskbarNotion, fixture.notionExe, L"",
                                        fixture.notionExe.parent_path(),
                                        fixture.notionExe);
                     notion_kex::InstallLauncherExecutable(
                         fixture.launcherRoot);
                     InstallShortcuts(locations);
                     const ShortcutOutcome outcome =
                         UninstallShortcuts(locations);
                     return [&fixture, locations,
                             outcome](CaseResult& result) {
                       result.EqualU64(outcome.restored, 3,
                                       "restored shortcut count");
                       const ShortcutInfo desktop = ReadTestShortcut(
                           locations.desktop / "Notion.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(desktop.target), fixture.notionExe),
                           "desktop restored");
                       const ShortcutInfo old = ReadTestShortcut(
                           locations.startMenu / "Old Notion.lnk");
                       // The PowerShell wrapper's script is removed with the
                       // launcher folder, so its shortcut opens Notion itself.
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(old.target), fixture.notionExe),
                           "old wrapper restored to Notion");
                       result.Check(old.arguments.empty(),
                                    "old wrapper arguments cleared");
                       const ShortcutInfo pinned = ReadTestShortcut(
                           locations.taskbar / "Notion Pinned.lnk");
                       result.Check(
                           notion_kex::detail::EqualsPath(
                               fs::path(pinned.target), fixture.notionExe),
                           "taskbar restored");
                       result.Check(
                           !fs::exists(locations.desktop /
                                       "Notion KEX.lnk"),
                           "desktop launcher removed");
                       result.Check(
                           !fs::exists(locations.startup /
                                       "Notion KEX.lnk"),
                           "startup launcher removed");
                       result.Check(
                           !fs::exists(fixture.launcherRoot /
                                       "shortcuts.json"),
                           "manifest removed");
                       result.EqualU64(
                           ListFiles(fixture.launcherRoot, "shortcut-").size(),
                           0, "backup files removed");
                     };
                   }});

  cases.push_back({"shortcuts-uninstall-without-manifest", false,
                   [](Fixture& fixture) {
                     const ShortcutLocations locations = fixture.Locations();
                     notion_kex::InstallLauncherExecutable(
                         fixture.launcherRoot);
                     CreateTestShortcut(locations.desktop / "Notion KEX.lnk",
                                        locations.launcherExe, L"",
                                        fixture.launcherRoot,
                                        fixture.notionExe);
                     CreateTestShortcut(locations.startup / "Notion KEX.lnk",
                                        locations.launcherExe,
                                        L"--signin-check",
                                        fixture.launcherRoot,
                                        fixture.notionExe);
                     const ShortcutOutcome outcome =
                         UninstallShortcuts(locations);
                     return [&fixture, locations,
                             outcome](CaseResult& result) {
                       result.Contains(outcome.message, "Restored 0",
                                       "no manifest message");
                       result.Check(
                           !fs::exists(locations.desktop /
                                       "Notion KEX.lnk"),
                           "desktop launcher removed");
                       result.Check(
                           !fs::exists(locations.startup /
                                       "Notion KEX.lnk"),
                           "startup launcher removed");
                     };
                   }});

  cases.push_back({"shortcuts-launcher-copy", false, [](Fixture& fixture) {
                     std::error_code error;
                     fs::create_directories(fixture.launcherRoot, error);
                     if (error) {
                       throw std::runtime_error("Could not create launcher.");
                     }
                     WriteFixtureFile(
                         fixture.launcherRoot /
                             "Notion KEX.exe.old-deadbeef",
                         "stale launcher");
                     const fs::path first =
                         notion_kex::InstallLauncherExecutable(
                             fixture.launcherRoot);
                     const fs::path second =
                         notion_kex::InstallLauncherExecutable(
                             fixture.launcherRoot);
                     return [&fixture, first,
                             second](CaseResult& result) {
                       result.Check(
                           notion_kex::detail::EqualsPath(first, second),
                           "stable launcher path");
                       result.Equal(
                           ReadFixtureFile(second), ReadFixtureFile(ModulePath()),
                           "launcher bytes match the running executable");
                       result.Equal(Hash(ReadFixtureFile(second)),
                                    Hash(ReadFixtureFile(ModulePath())),
                                    "launcher hash matches");
                       result.EqualU64(
                           ListFiles(fixture.launcherRoot,
                                     "Notion KEX.exe.old-")
                               .size(),
                           0, "stale launcher files removed");
                     };
                   }});

  cases.push_back({"shortcuts-remove-launcher-folder", false,
                   [](Fixture& fixture) {
                     std::error_code error;
                     fs::create_directories(fixture.launcherRoot, error);
                     WriteFixtureFile(fixture.launcherRoot / "stale.txt",
                                      "stale\n");
                     notion_kex::RemoveLauncherFolder(fixture.launcherRoot);
                     return [&fixture](CaseResult& result) {
                       result.Check(!fs::exists(fixture.launcherRoot),
                                    "launcher folder removed");
                     };
                   }});

  cases.push_back({"shortcuts-reject-junction", false, [](Fixture& fixture) {
                     const fs::path target = fixture.root / "JunctionTarget";
                     std::error_code error;
                     fs::create_directories(target, error);
                     std::string junctionError;
                     if (!CreateJunction(fixture.launcherRoot, target,
                                         junctionError)) {
                       throw SkipCase{"Could not create a junction: " +
                                      junctionError};
                     }
                     std::string launcherMessage;
                     try {
                       notion_kex::InstallLauncherExecutable(
                           fixture.launcherRoot);
                     } catch (const std::exception& exception) {
                       launcherMessage = exception.what();
                     }
                     std::string shortcutMessage;
                     try {
                       InstallShortcuts(fixture.Locations());
                     } catch (const std::exception& exception) {
                       shortcutMessage = exception.what();
                     }
                     RemoveDirectoryW(fixture.launcherRoot.c_str());
                     return [&fixture, target, launcherMessage,
                             shortcutMessage](CaseResult& result) {
                       result.Contains(launcherMessage,
                                       "must not be a symbolic link",
                                       "launcher junction rejected");
                       result.Contains(shortcutMessage,
                                       "must not be a symbolic link",
                                       "shortcut junction rejected");
                       result.Check(fs::exists(target),
                                    "junction target retained");
                     };
                   }});
}

void AddLauncherCases(std::vector<TestCase>& cases) {
  cases.push_back({"launcher-not-installed", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunLauncher(fixture.Launcher(true));
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Notion KEX is not installed.",
                                    "not installed message");
                     };
                   }});

  cases.push_back({"launcher-current", true, [](Fixture& fixture) {
                     InstallFixtureAndRecord(fixture);
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunLauncher(fixture.Launcher(true));
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Native zoom is current.",
                                    "current message");
                     };
                   }});

  cases.push_back({"launcher-repairs-stale", false, [](Fixture& fixture) {
                     InstallFixtureAndRecord(fixture);
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     record["patchedSize"] = 1;
                     WriteJsonFile(fixture.RecordPath(), record);
                     const LauncherResult outcome =
                         RunLauncher(fixture.Launcher(true));
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message, "Native zoom repaired.",
                                    "repair message");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed after repair");
                       const notion_kex::detail::FileInfo info =
                           notion_kex::detail::GetFileInfo(fixture.asar);
                       result.EqualU64(
                           ReadJsonFile(fixture.RecordPath())
                               ["patchedSize"].get<std::uint64_t>(),
                           info.size, "fast path refreshed");
                     };
                   }});

  cases.push_back({"launcher-failure-logs", false, [](Fixture& fixture) {
                     InstallFixtureAndRecord(fixture);
                     WriteFixtureFile(fixture.asar, "broken");
                     const LauncherResult outcome =
                         RunLauncher(fixture.Launcher(true));
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "1",
                                    "failure exit code");
                       result.Contains(outcome.message, "Unsupported ASAR",
                                       "failure message");
                       const fs::path log =
                           fixture.launcherRoot / "launcher.log";
                       result.Check(fs::exists(log), "launcher log written");
                       if (fs::exists(log)) {
                         result.Contains(ReadFixtureFile(log),
                                         "Unsupported ASAR",
                                         "launcher log content");
                       }
                       result.Equal(ReadFixtureFile(fixture.asar), "broken",
                                    "archive retained");
                     };
                   }});

  cases.push_back({"launcher-notion-running", false, [](Fixture& fixture) {
                     const fs::path cmd = ComSpec();
                     if (!notion_kex::detail::FileExists(cmd)) {
                       throw SkipCase{"cmd.exe is unavailable."};
                     }
                     notion_kex::detail::CopyFileTo(cmd, fixture.notionExe);
                     auto child = std::make_shared<ChildProcess>(
                         SpawnSleeper(fixture.notionExe));
                     if (!WaitForNotionPath(fixture.notionExe, true, 8000)) {
                       throw SkipCase{"The fixture Notion process did not start."};
                     }
                     fixture.WriteInstallRecord();
                     const std::string asarBefore =
                         ReadFixtureFile(fixture.asar);
                     const LauncherResult outcome =
                         RunLauncher(fixture.Launcher(true));
                     return [&fixture, child, asarBefore,
                             outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "1",
                                    "exit code");
                       result.Contains(outcome.message,
                                       "Quit Notion from its tray menu",
                                       "running message");
                       result.Equal(ReadFixtureFile(fixture.asar), asarBefore,
                                    "archive unchanged while running");
                       child->Stop();
                     };
                   }});

  cases.push_back({"launcher-missing-notion", false, [](Fixture& fixture) {
                     fs::remove(fixture.notionExe);
                     const LauncherResult outcome =
                         RunLauncher(fixture.Launcher(false));
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "1",
                                    "exit code");
                       result.Contains(outcome.message, "Notion was not found",
                                       "missing Notion message");
                       result.Check(
                           fs::exists(fixture.launcherRoot / "launcher.log"),
                           "failure log written");
                     };
                   }});

  cases.push_back({"signin-not-installed", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunSignInCheck(fixture.Launcher());
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Notion KEX is not installed.",
                                    "not installed message");
                     };
                   }});

  cases.push_back({"signin-current", true, [](Fixture& fixture) {
                     if (notion_kex::detail::IsAnyNotionRunning() ||
                         notion_kex::detail::IsUpdaterRunning()) {
                       throw SkipCase{
                           "Notion or its updater is running; the sign-in "
                           "check would defer."};
                     }
                     InstallFixtureAndRecord(fixture);
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunSignInCheck(fixture.Launcher());
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Native zoom is current.",
                                    "current message");
                     };
                   }});

  cases.push_back({"signin-repairs", false, [](Fixture& fixture) {
                     if (notion_kex::detail::IsAnyNotionRunning() ||
                         notion_kex::detail::IsUpdaterRunning()) {
                       throw SkipCase{
                           "Notion or its updater is running; the sign-in "
                           "check would defer."};
                     }
                     InstallFixtureAndRecord(fixture);
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     record["patchedSize"] = 1;
                     WriteJsonFile(fixture.RecordPath(), record);
                     const LauncherResult outcome =
                         RunSignInCheck(fixture.Launcher());
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Contains(outcome.message,
                                       "repaired during the sign-in check",
                                       "repair message");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed");
                     };
                   }});

  cases.push_back({"signin-defers-running", false, [](Fixture& fixture) {
                     const fs::path cmd = ComSpec();
                     if (!notion_kex::detail::FileExists(cmd)) {
                       throw SkipCase{"cmd.exe is unavailable."};
                     }
                     notion_kex::detail::CopyFileTo(cmd, fixture.notionExe);
                     auto child = std::make_shared<ChildProcess>(
                         SpawnSleeper(fixture.notionExe));
                     if (!WaitForNotionPath(fixture.notionExe, true, 8000)) {
                       throw SkipCase{"The fixture Notion process did not start."};
                     }
                     fixture.WriteInstallRecord();
                     const LauncherResult outcome =
                         RunSignInCheck(fixture.Launcher());
                     return [child, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Contains(outcome.message, "deferred",
                                       "deferred message");
                       child->Stop();
                     };
                   }});

  cases.push_back({"watcher-not-installed", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunWatcher(fixture.Launcher());
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Notion KEX is not installed.",
                                    "not installed message");
                     };
                   }});

  cases.push_back({"watcher-current", true, [](Fixture& fixture) {
                     if (notion_kex::detail::IsAnyNotionRunning() ||
                         notion_kex::detail::IsUpdaterRunning()) {
                       throw SkipCase{
                           "Notion or its updater is running; the watcher "
                           "would wait."};
                     }
                     InstallFixtureAndRecord(fixture);
                     return [&fixture](CaseResult& result) {
                       const LauncherResult outcome =
                           RunWatcher(fixture.Launcher());
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Equal(outcome.message,
                                    "Native zoom is current.",
                                    "current message");
                     };
                   }});

  cases.push_back({"watcher-repairs", false, [](Fixture& fixture) {
                     if (notion_kex::detail::IsAnyNotionRunning() ||
                         notion_kex::detail::IsUpdaterRunning()) {
                       throw SkipCase{
                           "Notion or its updater is running; the watcher "
                           "would wait."};
                     }
                     InstallFixtureAndRecord(fixture);
                     nlohmann::ordered_json record =
                         ReadJsonFile(fixture.RecordPath());
                     record["patchedSize"] = 1;
                     WriteJsonFile(fixture.RecordPath(), record);
                     const LauncherResult outcome =
                         RunWatcher(fixture.Launcher());
                     return [&fixture, outcome](CaseResult& result) {
                       result.Equal(std::to_string(outcome.exitCode), "0",
                                    "exit code");
                       result.Contains(outcome.message,
                                       "repaired after Notion closed",
                                       "repair message");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed");
                       result.Contains(
                           ReadFixtureFile(fixture.launcherRoot /
                                           "launcher.log"),
                           "repaired after Notion closed",
                           "watcher log");
                     };
                   }});

  cases.push_back({"watcher-busy", false, [](Fixture& fixture) {
                     fixture.WriteInstallRecord();
                     const std::string name = notion_kex::detail::MakeMutexName(
                         "Local\\NotionKexWatch", fixture.paths.asarPath);
                     notion_kex::detail::MutexGuard guard(
                         name, "Another Notion KEX watcher is already running.");
                     std::string message;
                     std::thread worker([&]() {
                       message = RunWatcher(fixture.Launcher()).message;
                     });
                     worker.join();
                     return [message](CaseResult& result) {
                       result.Equal(message,
                                    "Another watcher is already running.",
                                    "watcher busy message");
                     };
                   }});
}

void AddCliCases(std::vector<TestCase>& cases) {
  cases.push_back({"cli-help", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const ProcessOutput output =
                           RunCli(fixture, L"--help");
                       result.Equal(std::to_string(output.exitCode), "0",
                                    "help exit code");
                       result.Contains(output.text, "Usage: Notion KEX.exe",
                                       "help usage");
                       result.Contains(output.text, "--no-shortcuts",
                                       "help no-shortcuts");
                     };
                   }});

  cases.push_back({"cli-unknown-mode", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const ProcessOutput output =
                           RunCli(fixture, L"bogus");
                       result.Equal(std::to_string(output.exitCode), "2",
                                    "unknown mode exit code");
                       result.Contains(output.text, "Unknown mode: bogus",
                                       "unknown mode message");
                     };
                   }});

  // The startup shortcut runs --signin-check and the asar payload runs
  // --watch; a parse failure there would show an error box at every sign-in.
  cases.push_back({"cli-hidden-mode-flags", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       for (const wchar_t* flag :
                            {L"--signin-check", L"--watch"}) {
                         const ProcessOutput output = RunCli(fixture, flag);
                         const std::string name =
                             notion_kex::detail::ToUtf8(std::wstring(flag));
                         result.Equal(std::to_string(output.exitCode), "0",
                                      name + " exit code");
                         result.Contains(output.text,
                                         "Notion KEX is not installed",
                                         name + " message");
                       }
                     };
                   }});

  cases.push_back({"cli-status-not-installed", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const ProcessOutput output =
                           RunCli(fixture, L"status --json");
                       const nlohmann::ordered_json json =
                           ParseJsonOutput(output);
                       result.Equal(std::to_string(output.exitCode), "1",
                                    "status exit code");
                       result.Check(!json["extension"]["installed"].get<bool>(),
                                    "extension not installed");
                       result.Equal(
                           json["native"]["code"].get<std::string>(),
                           "not-installed", "native status code");
                       result.Check(!json["launcher"]["installed"].get<bool>(),
                                    "launcher not installed");
                     };
                   }});

  cases.push_back({"cli-install-status-uninstall", false,
                   [](Fixture& fixture) {
                     const std::string original =
                         ReadFixtureFile(fixture.asar);
                     const ProcessOutput install = RunCli(
                         fixture, L"install --no-launch --no-shortcuts");
                     // Capture post-install state before uninstall removes it.
                     const bool launcherCopied =
                         fs::exists(fixture.launcherRoot / "Notion KEX.exe");
                     const bool shortcutsSkipped =
                         !fs::exists(fixture.launcherRoot / "shortcuts.json");
                     const ProcessOutput status =
                         RunCli(fixture, L"status --json");
                     const nlohmann::ordered_json statusJson =
                         ParseJsonOutput(status);
                     const ProcessOutput uninstall =
                         RunCli(fixture, L"uninstall --no-shortcuts");
                     const ProcessOutput after =
                         RunCli(fixture, L"status --json");
                     return [&fixture, original, install, launcherCopied,
                             shortcutsSkipped, status, statusJson, uninstall,
                             after](CaseResult& result) {
                       result.Equal(std::to_string(install.exitCode), "0",
                                    "install exit code");
                       result.Contains(install.text, "1.1.2 installed in",
                                       "install output");
                       result.Check(launcherCopied, "launcher copied");
                       result.Check(shortcutsSkipped, "shortcuts skipped");
                       result.Equal(std::to_string(status.exitCode), "0",
                                    "status exit code");
                       result.Check(
                           statusJson["extension"]["healthy"].get<bool>(),
                           "extension healthy");
                       result.Equal(
                           statusJson["native"]["code"].get<std::string>(),
                           "installed", "native installed");
                       result.Check(
                           statusJson["launcher"]["installed"].get<bool>(),
                           "launcher installed");
                       result.Equal(std::to_string(uninstall.exitCode), "0",
                                    "uninstall exit code");
                       result.Contains(uninstall.text, "removed",
                                       "uninstall output");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "extension removed");
                       result.Check(!fs::exists(fixture.launcherRoot),
                                    "launcher removed");
                       result.Check(!fs::exists(fixture.RecordPath()),
                                    "native record removed");
                       result.Equal(ReadFixtureFile(fixture.asar), original,
                                    "archive restored");
                       result.Equal(std::to_string(after.exitCode), "1",
                                    "status after uninstall");
                     };
                   }});

  cases.push_back({"cli-install-quiet", false, [](Fixture& fixture) {
                     const ProcessOutput output = RunCli(
                         fixture,
                         L"install --no-launch --no-shortcuts --quiet");
                     return [&fixture, output](CaseResult& result) {
                       result.Equal(std::to_string(output.exitCode), "0",
                                    "quiet exit code");
                       result.Equal(Trim(output.text), "",
                                    "quiet output");
                       result.Check(fs::exists(fixture.InstallRoot()),
                                    "quiet install created extension");
                     };
                   }});

  cases.push_back({"cli-validate", true, [](Fixture& fixture) {
                     return [&fixture](CaseResult& result) {
                       const ProcessOutput output =
                           RunCli(fixture, L"validate");
                       result.Equal(std::to_string(output.exitCode), "0",
                                    "validate exit code");
                       result.Contains(output.text, "supported",
                                       "validate output");
                       result.Check(!fs::exists(fixture.recordRoot),
                                    "validate creates no record root");
                     };
                   }});

  cases.push_back({"cli-repair", false, [](Fixture& fixture) {
                     InstallNativeZoom(fixture.paths, fixture.notionExe);
                     const ProcessOutput output =
                         RunCli(fixture, L"repair");
                     return [&fixture, output](CaseResult& result) {
                       result.Equal(std::to_string(output.exitCode), "0",
                                    "repair exit code");
                       result.Contains(output.text, "already matches",
                                       "repair output");
                       result.Check(Inspect(fixture.paths).installed,
                                    "inspect installed");
                     };
                   }});

  cases.push_back({"cli-fuse-rejected", false, [](Fixture& fixture) {
                     WriteFuseExe(fixture.notionExe, '1');
                     const ProcessOutput output = RunCli(
                         fixture, L"install --no-launch --no-shortcuts");
                     return [&fixture, output](CaseResult& result) {
                       result.Equal(std::to_string(output.exitCode), "1",
                                    "fuse exit code");
                       result.Contains(output.text, "embedded ASAR integrity",
                                       "fuse output");
                       result.Check(!fs::exists(fixture.InstallRoot()),
                                    "no extension folder");
                     };
                   }});

  cases.push_back({"cli-status-malformed-record", true, [](Fixture& fixture) {
                     WriteFixtureFile(fixture.RecordPath(),
                                      "{ \"asarPath\": ");
                     return [&fixture](CaseResult& result) {
                       const ProcessOutput output =
                           RunCli(fixture, L"status --json");
                       const nlohmann::ordered_json json =
                           ParseJsonOutput(output);
                       result.Equal(std::to_string(output.exitCode), "1",
                                    "status exit code");
                       result.Equal(
                           json["native"]["code"].get<std::string>(),
                           "invalid-record", "invalid record status");
                       result.Check(
                           !json["native"]["installed"].get<bool>(),
                           "native not installed");
                       result.Contains(
                           json["native"]["detail"].get<std::string>(),
                           "not valid JSON", "invalid record detail");
                     };
                   }});
}

void AddEmbeddedCases(std::vector<TestCase>& cases) {
  cases.push_back({"embedded-inventory", true, [](Fixture&) {
                     return [](CaseResult& result) {
                       const auto files = notion_kex::embedded::Files();
                       result.EqualU64(files.size(), 9,
                                       "embedded file count");
                       const notion_kex::embedded::File* preload =
                           notion_kex::embedded::Find("native/pinch.js");
                       const notion_kex::embedded::File* main =
                           notion_kex::embedded::Find("native/pinch-main.js");
                       const notion_kex::embedded::File* manifest =
                           notion_kex::embedded::Find(
                               "extension/manifest.json");
                       result.Check(preload != nullptr, "preload embedded");
                       result.Check(main != nullptr, "main embedded");
                       result.Check(manifest != nullptr, "manifest embedded");
                       if (preload == nullptr || main == nullptr ||
                           manifest == nullptr) {
                         return;
                       }
                       const std::string preloadText(
                           reinterpret_cast<const char*>(preload->data),
                           preload->size);
                       const std::string mainText(
                           reinterpret_cast<const char*>(main->data),
                           main->size);
                       result.Contains(preloadText,
                                       "notion-kex:configure-pinch",
                                       "preload IPC contract");
                       result.Contains(preloadText,
                                       "notion-kex:pinch-state",
                                       "preload state report");
                       result.Contains(mainText, "before-quit-for-update",
                                       "watcher update hook");
                       result.Contains(mainText, "before-quit",
                                       "watcher quit hook");
                       result.Contains(mainText, "--watch",
                                       "watcher argument");
                       const std::string preloadHash = Hash(preloadText);
                       const std::string mainHash = Hash(mainText);
                       result.Equal(
                           notion_kex::embedded::PayloadHash(),
                           Hash(preloadHash + "\n" + mainHash),
                           "payload hash");
                       const nlohmann::ordered_json parsed =
                           nlohmann::ordered_json::parse(std::string(
                               reinterpret_cast<const char*>(manifest->data),
                               manifest->size));
                       result.Equal(parsed["name"].get<std::string>(),
                                    "Notion KEX", "manifest name");
                       result.Check(!parsed["version"].get<std::string>().empty(),
                                    "manifest version");
                     };
                   }});
}

void AddRecordCases(std::vector<TestCase>& cases) {
  cases.push_back({"record-optional-metadata", true, [](Fixture& fixture) {
                     const fs::path backup = fixture.BackupPath();
                     WriteFixtureFile(backup, "backup bytes");
                     const notion_kex::detail::FileInfo info =
                         notion_kex::detail::GetFileInfo(fixture.asar);
                     nlohmann::ordered_json record = RecordJson(
                         fixture.asar, backup, Upper(Hash("backup bytes")),
                         Upper(Hash(ReadFixtureFile(fixture.asar))),
                         Upper(notion_kex::embedded::PayloadHash()));
                     // The fast path accepts string and numeric metadata.
                     record["patchedSize"] = std::to_string(info.size);
                     record["patchedWriteTimeUtc"] = info.writeTime;
                     WriteJsonFile(fixture.RecordPath(), record);
                     return [&fixture, info](CaseResult& result) {
                       NativeRecord parsed;
                       const notion_kex::RecordResult read =
                           notion_kex::ReadNativeRecord(fixture.RecordPath(),
                                                        parsed);
                       result.Check(read.ok, "record parses", read.error);
                       result.Equal(parsed.originalHash, Hash("backup bytes"),
                                    "hash normalized");
                       result.Check(parsed.patchedSize.has_value(),
                                    "string patchedSize accepted");
                       if (parsed.patchedSize.has_value()) {
                         result.EqualU64(*parsed.patchedSize, info.size,
                                         "patchedSize value");
                       }
                       result.Check(parsed.patchedWriteTimeUtc.has_value(),
                                    "numeric write time accepted");
                       if (parsed.patchedWriteTimeUtc.has_value()) {
                         result.EqualU64(*parsed.patchedWriteTimeUtc,
                                         info.writeTime,
                                         "write time value");
                       }
                       const notion_kex::RecordResult valid =
                           notion_kex::ValidateNativeRecord(parsed,
                                                            fixture.paths);
                       result.Check(valid.ok, "record validates", valid.error);
                       const StatusResult status = Inspect(fixture.paths);
                       result.Check(status.installed, "inspect installed");
                       const QuickStatus quick = QuickInspect(fixture.paths);
                       result.Check(quick.fastPath, "quick fast path");
                       result.Check(quick.installed, "quick installed");
                     };
                   }});

  cases.push_back({"record-malformed-optional", true, [](Fixture& fixture) {
                     const fs::path backup = fixture.BackupPath();
                     WriteFixtureFile(backup, "backup bytes");
                     nlohmann::ordered_json record =
                         RecordJson(fixture.asar, backup, Hash("backup bytes"),
                                    Hash(ReadFixtureFile(fixture.asar)));
                     record["patchedSize"] = "not-a-number";
                     record["patchedWriteTimeUtc"] = -1;
                     WriteJsonFile(fixture.RecordPath(), record);
                     return [&fixture](CaseResult& result) {
                       NativeRecord parsed;
                       const notion_kex::RecordResult read =
                           notion_kex::ReadNativeRecord(fixture.RecordPath(),
                                                        parsed);
                       result.Check(read.ok, "record still parses", read.error);
                       result.Check(!parsed.patchedSize.has_value(),
                                    "malformed size ignored");
                       result.Check(!parsed.patchedWriteTimeUtc.has_value(),
                                    "negative write time ignored");
                       const QuickStatus quick = QuickInspect(fixture.paths);
                       result.Check(!quick.fastPath,
                                    "quick falls back to full verification");
                     };
                   }});

  cases.push_back({"record-path-validation", false, [](Fixture& fixture) {
                     const fs::path backup = fixture.BackupPath();
                     WriteFixtureFile(backup, "backup bytes");
                     const std::string originalHash = Hash("backup bytes");
                     const std::string patchedHash =
                         Hash(ReadFixtureFile(fixture.asar));
                     std::string wrongAsar;
                     {
                       NativeRecord record;
                       record.asarPath = Utf8(fixture.root / "Other" /
                                              "app.asar");
                       record.backupPath = Utf8(backup);
                       record.originalHash = originalHash;
                       record.patchedHash = patchedHash;
                       record.payloadHash = std::string(64, 'a');
                       record.installedAt = "2026-11-09T00:00:00Z";
                       wrongAsar =
                           notion_kex::ValidateNativeRecord(record,
                                                            fixture.paths)
                               .error;
                     }
                     std::string badLeaf;
                     {
                       NativeRecord record;
                       record.asarPath = Utf8(fixture.asar);
                       record.backupPath =
                           Utf8(fixture.recordRoot / "evil.bak");
                       record.originalHash = originalHash;
                       record.patchedHash = patchedHash;
                       record.payloadHash = std::string(64, 'a');
                       record.installedAt = "2026-11-09T00:00:00Z";
                       badLeaf =
                           notion_kex::ValidateNativeRecord(record,
                                                            fixture.paths)
                               .error;
                     }
                     std::string outside;
                     {
                       NativeRecord record;
                       record.asarPath = Utf8(fixture.asar);
                       record.backupPath = Utf8(
                           fixture.root / "outside" /
                           "app-asar-before-native-0123456789abcdef"
                           "0123456789abcdef.bak");
                       record.originalHash = originalHash;
                       record.patchedHash = patchedHash;
                       record.payloadHash = std::string(64, 'a');
                       record.installedAt = "2026-11-09T00:00:00Z";
                       outside =
                           notion_kex::ValidateNativeRecord(record,
                                                            fixture.paths)
                               .error;
                     }
                     return [wrongAsar, badLeaf,
                             outside](CaseResult& result) {
                       result.Contains(wrongAsar,
                                       "different Notion installation",
                                       "wrong asar rejected");
                       result.Contains(badLeaf,
                                       "Unexpected native zoom backup path",
                                       "bad leaf rejected");
                       result.Contains(outside,
                                       "Unexpected native zoom backup path",
                                       "outside root rejected");
                     };
                   }});
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

std::vector<TestCase> BuildCases() {
  std::vector<TestCase> cases;
  AddNativeCases(cases);
  AddRecoveryCases(cases);
  AddExtensionCases(cases);
  AddShortcutCases(cases);
  AddLauncherCases(cases);
  AddCliCases(cases);
  AddEmbeddedCases(cases);
  AddRecordCases(cases);
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
      options.filter = NarrowUtf8(argv[++index]);
    } else {
      error = "Unknown option. Run kex-native-tests --help for usage.";
      return false;
    }
  }
  return true;
}

void PrintUsage() {
  std::fputs(
      "Usage: kex-native-tests [options]\n"
      "\n"
      "Runs generated-fixture tests for the full native Notion KEX host.\n"
      "Every case uses its own temporary tree and never touches the live\n"
      "Notion installation or the real shell shortcut folders.\n"
      "\n"
      "  --evidence <path>  Write machine-readable JSON evidence.\n"
      "  --filter <text>    Run only cases whose names contain text.\n"
      "  --keep             Keep generated fixture directories.\n"
      "  --help, -h         Show this help.\n",
      stdout);
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
    std::vector<SnapshotEntry> before;
    if (testCase.passive) {
      before = SnapshotTree(caseRoot);
      result.snapshotEntries = static_cast<int>(before.size());
    }
    result.passiveChecked = testCase.passive;

    try {
      run(result);
    } catch (const std::exception& exception) {
      result.passed = false;
      result.assertions.push_back(
          Assertion{"run threw", false, exception.what()});
    }

    if (testCase.passive) {
      const std::vector<SnapshotEntry> after = SnapshotTree(caseRoot);
      result.passive = (before == after);
      if (!result.passive) {
        result.passed = false;
        result.passivityDetail = DescribeSnapshotDiff(before, after);
      }
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
      {"passiveChecked", result.passiveChecked},
      {"passive", result.passive},
      {"passivityDetail", result.passivityDetail},
      {"snapshotEntries", result.snapshotEntries},
      {"assertions", std::move(assertions)},
  };
}

void Print(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

void PrintError(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stderr);
  std::fflush(stderr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const HRESULT comResult =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool comInitialized =
      comResult == S_OK || comResult == S_FALSE;

  TestOptions options;
  std::string error;
  if (!ParseArguments(argc, argv, options, error)) {
    PrintError("kex-native-tests: " + error + "\n\n");
    PrintUsage();
    if (comInitialized) {
      CoUninitialize();
    }
    return 2;
  }
  if (options.help) {
    PrintUsage();
    if (comInitialized) {
      CoUninitialize();
    }
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
      PrintError("kex-native-tests: no cases matched --filter.\n");
      if (comInitialized) {
        CoUninitialize();
      }
      return 2;
    }
  }

  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const fs::path base =
      fs::temp_directory_path() /
      ("notion-kex-native-tests-" + std::to_string(GetCurrentProcessId()) +
       "-" + std::to_string(stamp));
  std::error_code directoryError;
  fs::create_directories(base, directoryError);
  if (directoryError) {
    PrintError("kex-native-tests: could not create fixture root: " +
               directoryError.message() + "\n");
    if (comInitialized) {
      CoUninitialize();
    }
    return 2;
  }

  std::vector<CaseResult> results;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  int passiveCases = 0;
  bool allPassive = true;

  Print("kex-native fixture tests\n");
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const fs::path caseRoot =
        base / (std::to_string(index + 1) + "-" +
                SanitizeName(cases[index].name));
    CaseResult result = RunOne(cases[index], caseRoot, options.keep);
    if (result.skipped) {
      ++skipped;
      Print("[SKIP] " + result.name + ": " + result.skipReason + "\n");
    } else if (result.passed) {
      ++passed;
      Print("[PASS] " + result.name + " (" +
            std::to_string(result.assertions.size()) + " assertions" +
            (result.passiveChecked
                 ? ", " + std::to_string(result.snapshotEntries) +
                       " snapshot entries, passive"
                 : ", mutating fixture") +
            ")\n");
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
    if (result.passiveChecked && !result.skipped) {
      ++passiveCases;
      allPassive = allPassive && result.passive;
    }
    results.push_back(std::move(result));
  }

  Print("\nsummary: " + std::to_string(passed) + " passed, " +
        std::to_string(failed) + " failed, " + std::to_string(skipped) +
        " skipped; " +
        (allPassive ? "all passive fixtures unchanged"
                    : "PASSIVE FIXTURES CHANGED") +
        "\n");
  if (options.keep) {
    Print("fixtures kept at: " + Utf8(base) + "\n");
  }

  if (options.evidence.has_value()) {
    fs::path evidencePath = *options.evidence;
    if (evidencePath.is_relative()) {
      evidencePath = fs::absolute(evidencePath);
    }
    std::error_code evidenceError;
    fs::create_directories(evidencePath.parent_path(), evidenceError);
    if (evidenceError) {
      PrintError("kex-native-tests: could not create evidence directory: " +
                 evidenceError.message() + "\n");
      if (comInitialized) {
        CoUninitialize();
      }
      return 2;
    }

    nlohmann::json evidence;
    evidence["component"] = "kex-native";
    evidence["version"] = KEX_VERSION;
    evidence["generatedAt"] = notion_kex::detail::UtcNowIso();
    evidence["build"] = {
        {"compilerId", KEX_COMPILER_ID},
        {"compilerVersion", KEX_COMPILER_VERSION},
        {"msvcVersion", _MSC_VER},
        {"cplusplus", __cplusplus},
        {"configuration", KEX_BUILD_TYPE},
        {"pointerBits", sizeof(void*) * 8},
    };
    evidence["capabilities"] = {
        {"mutatesFixtures", true},
        {"liveNotionTouched", false},
        {"network", false},
        {"launchesFixtureProcesses", true},
        {"shortcutsUseFixtureLocations", true},
        {"messageBoxes", false},
        {"recoversPendingTransactions", true},
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
        {"passiveCases", passiveCases},
        {"allPassed", failed == 0},
        {"allPassive", allPassive},
    };
    evidence["limitations"] = nlohmann::json::array(
        {"Fixture tests prove transaction, recovery, extension, shortcut, "
         "launcher and CLI semantics without modifying the live Notion "
         "installation; live end-to-end validation is deferred by design.",
         "Shortcut cases pass explicit fixture folders and never touch the "
         "real desktop, Start menu, taskbar or startup folder.",
         "Watcher cases are skipped while Notion or its updater is running, "
         "because the watcher deliberately waits for both to exit.",
         "The original safety-stop reason remains unknown; this suite does "
         "not claim to resolve it."});

    std::ofstream stream(evidencePath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      PrintError("kex-native-tests: could not write evidence file.\n");
      if (comInitialized) {
        CoUninitialize();
      }
      return 2;
    }
    std::string text;
    try {
      // Assertion details can quote fixture bytes. Replace invalid UTF-8 so a
      // diagnostic string can never turn evidence collection into a crash.
      text = evidence.dump(2, ' ', false,
                           nlohmann::json::error_handler_t::replace) +
             "\n";
    } catch (const std::exception& exception) {
      PrintError(std::string("kex-native-tests: could not serialize evidence: ") +
                 exception.what() + "\n");
      if (comInitialized) {
        CoUninitialize();
      }
      return 2;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
      PrintError("kex-native-tests: could not finish evidence file.\n");
      if (comInitialized) {
        CoUninitialize();
      }
      return 2;
    }
    Print("evidence: " + Utf8(evidencePath) + "\n");
  }

  if (!options.keep) {
    std::error_code cleanupError;
    fs::remove(base, cleanupError);
  }
  if (comInitialized) {
    CoUninitialize();
  }
  return failed == 0 ? 0 : 1;
}
