#include "notion_kex/setup.hpp"

#include "file_util.hpp"
#include "notion_kex/embedded.hpp"
#include "notion_kex/hash.hpp"
#include "notion_kex/native_zoom.hpp"
#include "notion_kex/record.hpp"
#include "notion_kex/status.hpp"
#include "path_util.hpp"
#include "process.hpp"

#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace notion_kex {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kMarkerName = ".notion-kex-owned";
constexpr std::string_view kMarkerText =
    "Notion KEX installer owns this folder.";
constexpr std::string_view kClosedMessage =
    "Quit Notion completely from its tray menu, then run this command again. "
    "Running settings will not be overwritten.";
constexpr std::string_view kStateMissingMessage =
    "Start Notion once, quit it completely, and run the installer again.";
constexpr std::string_view kStateUnsupportedMessage =
    "This Notion settings format is unsupported. No changes made.";

constexpr std::array<const char*, 7> kExtensionFiles = {
    "extension/kex.css",        "extension/kex.js",
    "extension/manifest.json",  "extension/policy.js",
    "extension/settings.css",   "extension/settings.html",
    "extension/settings.js",
};

constexpr std::array<const char*, 4> kLoaderContracts = {
    "setupExtensions", "isChromeExtensionsEnabled",
    "isChromeExtensionsAllowed", "loadExtension",
};

fs::path InstallRoot(const fs::path& notionData) {
  return notionData / "extensions" / "Notion KEX";
}

fs::path RecordRoot(const fs::path& notionData) {
  return notionData / "Notion KEX";
}

fs::path RecordPath(const fs::path& notionData) {
  return RecordRoot(notionData) / "install.json";
}

bool ExistsAny(const fs::path& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsReparsePoint(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool EndsWithIgnoreCase(const std::wstring& value, const std::wstring& suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  return CompareStringOrdinal(
             value.c_str() + (value.size() - suffix.size()),
             static_cast<int>(suffix.size()), suffix.c_str(),
             static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

bool ContainsBytes(const std::vector<std::uint8_t>& bytes,
                   std::string_view needle) {
  if (needle.empty() || bytes.size() < needle.size()) {
    return false;
  }
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) !=
         bytes.end();
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

nlohmann::ordered_json ReadJsonFile(const fs::path& path,
                                    std::uint64_t maxBytes,
                                    std::string_view label) {
  std::string error;
  if (detail::CheckRegularPath(path, error) != detail::PathCheck::Ok) {
    throw std::runtime_error(error);
  }
  const std::string text = detail::ReadFileText(path, maxBytes, label);
  try {
    return nlohmann::ordered_json::parse(text);
  } catch (const std::exception&) {
    throw std::runtime_error("Native zoom metadata is not valid JSON.");
  }
}

nlohmann::ordered_json ReadState(const fs::path& notionData) {
  const fs::path statePath = notionData / "state.json";
  if (!detail::FileExists(statePath)) {
    throw std::runtime_error(std::string(kStateMissingMessage));
  }
  nlohmann::ordered_json state;
  try {
    state = ReadJsonFile(statePath, detail::kMaxStateBytes, "Notion settings");
  } catch (const std::exception&) {
    throw std::runtime_error(std::string(kStateUnsupportedMessage));
  }
  if (!state.is_object()) {
    throw std::runtime_error(std::string(kStateUnsupportedMessage));
  }
  const auto appState = state.find("appState");
  if (appState == state.end() || !appState->is_object()) {
    throw std::runtime_error(std::string(kStateUnsupportedMessage));
  }
  const auto preferences = appState->find("preferences");
  if (preferences == appState->end() || !preferences->is_object()) {
    throw std::runtime_error(std::string(kStateUnsupportedMessage));
  }
  return state;
}

void AssertOwnedDirectory(const fs::path& notionData) {
  const fs::path installRoot = InstallRoot(notionData);
  const fs::path recordRoot = RecordRoot(notionData);

  std::error_code error;
  const fs::path fullData =
      fs::absolute(notionData, error).lexically_normal();
  if (error) {
    throw std::runtime_error("Unexpected installation path.");
  }
  error.clear();
  const fs::path fullInstall =
      fs::absolute(installRoot, error).lexically_normal();
  if (error) {
    throw std::runtime_error("Unexpected installation path.");
  }

  std::wstring dataText = fullData.native();
  while (!dataText.empty() &&
         (dataText.back() == L'\\' || dataText.back() == L'/')) {
    dataText.pop_back();
  }
  dataText.push_back(L'\\');
  const std::wstring installText = fullInstall.native();
  const bool underData =
      installText.size() > dataText.size() &&
      CompareStringOrdinal(installText.c_str(),
                           static_cast<int>(dataText.size()), dataText.c_str(),
                           static_cast<int>(dataText.size()), TRUE) ==
          CSTR_EQUAL;
  if (!underData ||
      !EndsWithIgnoreCase(installText,
                          L"\\extensions\\Notion KEX")) {
    throw std::runtime_error("Unexpected installation path.");
  }

  if (ExistsAny(installRoot) &&
      !detail::FileExists(installRoot / std::string(kMarkerName))) {
    throw std::runtime_error(
        "The target contains an unrecognized extension. It will not be "
        "overwritten.");
  }
  if (ExistsAny(installRoot) && IsReparsePoint(installRoot)) {
    throw std::runtime_error(
        "Extension directory must not be a symbolic link or junction.");
  }
  for (const fs::path& ancestor :
       {notionData, notionData / "extensions", recordRoot}) {
    if (ExistsAny(ancestor) && IsReparsePoint(ancestor)) {
      throw std::runtime_error(
          "Installation ancestors must not be symbolic links or junctions.");
    }
  }
}

std::string ProductVersion(const fs::path& notionExe) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(notionExe.c_str(), &handle);
  if (size == 0) {
    return {};
  }
  std::vector<std::uint8_t> buffer(size);
  if (!GetFileVersionInfoW(notionExe.c_str(), 0, size, buffer.data())) {
    return {};
  }

  struct Translation {
    WORD language;
    WORD codePage;
  };
  Translation* translations = nullptr;
  UINT translationBytes = 0;
  if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                     reinterpret_cast<void**>(&translations),
                     &translationBytes) &&
      translationBytes >= sizeof(Translation)) {
    wchar_t subBlock[64] = {};
    swprintf(subBlock, 64, L"\\StringFileInfo\\%04x%04x\\ProductVersion",
             static_cast<unsigned int>(translations[0].language),
             static_cast<unsigned int>(translations[0].codePage));
    wchar_t* value = nullptr;
    UINT valueBytes = 0;
    if (VerQueryValueW(buffer.data(), subBlock,
                       reinterpret_cast<void**>(&value), &valueBytes) &&
        value != nullptr && valueBytes >= sizeof(wchar_t)) {
      return detail::ToUtf8(std::wstring(value));
    }
  }

  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixedBytes = 0;
  if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&fixed),
                     &fixedBytes) &&
      fixed != nullptr && fixedBytes >= sizeof(VS_FIXEDFILEINFO)) {
    const unsigned int parts[4] = {
        HIWORD(fixed->dwProductVersionMS), LOWORD(fixed->dwProductVersionMS),
        HIWORD(fixed->dwProductVersionLS), LOWORD(fixed->dwProductVersionLS)};
    char text[64] = {};
    snprintf(text, sizeof(text), "%u.%u.%u.%u", parts[0], parts[1], parts[2],
             parts[3]);
    return text;
  }
  return {};
}

void SetPreference(nlohmann::ordered_json& state, const char* name,
                   const nlohmann::ordered_json& value) {
  state["appState"]["preferences"][name] = value;
}

void FilterDisabledExtensions(nlohmann::ordered_json& state) {
  auto& preferences = state["appState"]["preferences"];
  const auto disabled = preferences.find("disabledExtensions");
  if (disabled == preferences.end()) {
    return;
  }
  nlohmann::ordered_json filtered = nlohmann::ordered_json::array();
  if (disabled->is_array()) {
    for (const auto& value : *disabled) {
      if (value.is_string() && value.get<std::string>() == "Notion KEX") {
        continue;
      }
      filtered.push_back(value);
    }
  } else if (disabled->is_string()) {
    if (disabled->get<std::string>() != "Notion KEX") {
      filtered.push_back(*disabled);
    }
  }
  preferences["disabledExtensions"] = std::move(filtered);
}

bool HasOtherExtensions(const fs::path& notionData) {
  const fs::path root = notionData / "extensions";
  std::error_code error;
  fs::directory_iterator iterator(root, error);
  if (error) {
    return false;
  }
  for (const fs::directory_entry& entry : iterator) {
    std::error_code statusError;
    if (!entry.is_directory(statusError) || statusError) {
      continue;
    }
    if (entry.path().filename() != L"Notion KEX") {
      return true;
    }
  }
  return false;
}

nlohmann::ordered_json ExtensionRecord(const fs::path& recordPath) {
  if (!detail::FileExists(recordPath)) {
    return nlohmann::ordered_json::object();
  }
  return ReadJsonFile(recordPath, detail::kMaxMetadataBytes,
                      "Notion KEX installation record");
}

nlohmann::ordered_json PreferencesBefore(
    const nlohmann::ordered_json& state) {
  nlohmann::ordered_json before = nlohmann::ordered_json::object();
  const auto& preferences = state["appState"]["preferences"];
  for (const char* name :
       {"isChromeExtensionsEnabled", "isDebugMenuEnabled"}) {
    const auto property = preferences.find(name);
    nlohmann::ordered_json entry = nlohmann::ordered_json::object();
    entry["existed"] = property != preferences.end();
    if (property != preferences.end()) {
      entry["value"] = *property;
    } else {
      entry["value"] = nullptr;
    }
    before[name] = std::move(entry);
  }
  return before;
}

const embedded::File* FindExtensionFile(std::string_view path) {
  const embedded::File* file = embedded::Find(path);
  if (file == nullptr) {
    throw std::runtime_error("The embedded extension package is incomplete.");
  }
  return file;
}

std::string ManifestVersion() {
  const std::string text = embedded::ExtensionManifestJson();
  nlohmann::ordered_json manifest;
  try {
    manifest = nlohmann::ordered_json::parse(text);
  } catch (const std::exception&) {
    throw std::runtime_error("Unexpected package identity.");
  }
  if (!manifest.is_object() || !manifest.contains("name") ||
      !manifest["name"].is_string() ||
      manifest["name"].get<std::string>() != "Notion KEX" ||
      !manifest.contains("version") || !manifest["version"].is_string() ||
      manifest["version"].get<std::string>().empty()) {
    throw std::runtime_error("Unexpected package identity.");
  }
  return manifest["version"].get<std::string>();
}

}  // namespace

SetupOutcome InstallExtension(const fs::path& notionExe,
                              const fs::path& notionData) {
  detail::AssertNotionClosed(notionExe, kClosedMessage);
  AssertOwnedDirectory(notionData);

  if (!detail::FileExists(notionExe)) {
    throw std::runtime_error(
        "Notion was not found. Install the official Windows desktop app first, "
        "or pass --notion-exe.");
  }
  const fs::path asarPath =
      notionExe.parent_path() / "resources" / "app.asar";
  if (!detail::FileExists(asarPath)) {
    throw std::runtime_error("The installed Notion package is unsupported.");
  }

  const std::vector<std::uint8_t> bundle =
      detail::ReadFileBytes(asarPath, detail::kMaxAsarBytes,
                            "The Notion app archive");
  for (const char* term : kLoaderContracts) {
    if (!ContainsBytes(bundle, term)) {
      throw std::runtime_error(
          "Notion's native extension loader changed (" + std::string(term) +
          "). No changes made; update Notion KEX first.");
    }
  }

  const fs::path statePath = notionData / "state.json";
  nlohmann::ordered_json state = ReadState(notionData);
  const std::string version = ManifestVersion();

  for (const char* path : kExtensionFiles) {
    (void)FindExtensionFile(path);
  }

  const RecordPaths paths = MakeRecordPaths(notionExe, notionData);

  // Validate is mutating only in the sense that a prior interrupted journal is
  // resolved first. Every check still runs before the first profile write.
  RecoverNativeZoom(paths);
  ValidateNativeZoom(paths, notionExe);

  const fs::path recordRoot = RecordRoot(notionData);
  const fs::path recordPath = RecordPath(notionData);
  detail::CreateDirectories(recordRoot);

  nlohmann::ordered_json record = ExtensionRecord(recordPath);
  if (!record.is_object()) {
    throw std::runtime_error(
        "Notion KEX installation record is not a JSON object.");
  }
  if (record.empty()) {
    const fs::path backup =
        recordRoot /
        detail::FromUtf8("state-before-install-" + detail::LocalTimestamp() +
                         ".json");
    detail::CopyFileTo(statePath, backup);
    record["version"] = version;
    record["installedAt"] = detail::UtcNowIso();
    record["preferencesBefore"] = PreferencesBefore(state);
    record["stateBackup"] = detail::ToUtf8(backup);
    record["files"] = nlohmann::ordered_json::object();
  }

  const fs::path installRoot = InstallRoot(notionData);
  detail::CreateDirectories(installRoot);

  nlohmann::ordered_json files = nlohmann::ordered_json::object();
  for (const char* path : kExtensionFiles) {
    const embedded::File* file = FindExtensionFile(path);
    const fs::path leaf = fs::path(file->path).filename();
    const std::vector<std::uint8_t> bytes(file->data, file->data + file->size);
    detail::WriteFileBytes(installRoot / leaf, bytes);
    files[detail::ToUtf8(leaf)] = detail::HashBytesVector(bytes);
  }
  detail::WriteFileText(installRoot / std::string(kMarkerName), kMarkerText);

  record["files"] = std::move(files);
  record["version"] = version;
  detail::WriteJsonAtomic(recordPath, record);

  SetPreference(state, "isChromeExtensionsEnabled", true);
  SetPreference(state, "isDebugMenuEnabled", true);
  FilterDisabledExtensions(state);
  detail::WriteJsonAtomic(statePath, state);

  const NativeZoomOutcome native = InstallNativeZoom(paths, notionExe);

  SetupOutcome outcome;
  outcome.changed = true;
  outcome.message = "Notion KEX " + version + " installed in " +
                    detail::ToUtf8(installRoot) + ". " + native.message;
  return outcome;
}

SetupOutcome UninstallExtension(const fs::path& notionExe,
                                const fs::path& notionData) {
  detail::AssertNotionClosed(notionExe, kClosedMessage);
  AssertOwnedDirectory(notionData);

  const fs::path recordPath = RecordPath(notionData);
  if (!detail::FileExists(recordPath)) {
    throw std::runtime_error(
        "No Notion KEX installation record was found.");
  }

  const fs::path statePath = notionData / "state.json";
  nlohmann::ordered_json state = ReadState(notionData);
  const nlohmann::ordered_json record =
      ReadJsonFile(recordPath, detail::kMaxMetadataBytes,
                   "Notion KEX installation record");
  if (!record.is_object()) {
    throw std::runtime_error(
        "Notion KEX installation record is not a JSON object.");
  }

  const RecordPaths paths = MakeRecordPaths(notionExe, notionData);
  const NativeZoomOutcome native = UninstallNativeZoom(paths, notionExe);

  const auto before = record.find("preferencesBefore");
  if (before != record.end() && before->is_object()) {
    auto& preferences = state["appState"]["preferences"];
    const bool keepExtensionsEnabled =
        HasOtherExtensions(notionData);
    for (auto iterator = before->begin(); iterator != before->end();
         ++iterator) {
      const std::string name = iterator.key();
      if (name == "isChromeExtensionsEnabled" && keepExtensionsEnabled) {
        continue;
      }
      const auto current = preferences.find(name);
      if (current == preferences.end() || !current->is_boolean() ||
          !current->get<bool>()) {
        continue;
      }
      const auto existed = iterator.value().find("existed");
      if (existed != iterator.value().end() && existed->is_boolean() &&
          existed->get<bool>()) {
        const auto value = iterator.value().find("value");
        if (value != iterator.value().end()) {
          preferences[name] = *value;
        } else {
          preferences[name] = nullptr;
        }
      } else {
        preferences.erase(name);
      }
    }
  }

  detail::WriteJsonAtomic(statePath, state);

  const fs::path installRoot = InstallRoot(notionData);
  if (ExistsAny(installRoot)) {
    detail::RemoveTree(installRoot);
  }
  detail::RemoveFile(recordPath);

  SetupOutcome outcome;
  outcome.changed = true;
  outcome.message =
      "Notion KEX removed. Your notes, other extensions, and secure Notion "
      "settings were retained. " +
      native.message;
  return outcome;
}

ExtensionStatus InspectExtension(const fs::path& notionExe,
                                 const fs::path& notionData) {
  ExtensionStatus status;
  const fs::path installRoot = InstallRoot(notionData);
  status.installRoot = detail::ToUtf8(installRoot);
  status.installed = detail::FileExists(installRoot / "manifest.json");
  if (detail::FileExists(notionExe)) {
    status.notionVersion = ProductVersion(notionExe);
  }
  if (!status.installed) {
    status.problems.push_back("Extension manifest is missing.");
  }

  const fs::path recordPath = RecordPath(notionData);
  if (detail::FileExists(recordPath)) {
    try {
      const nlohmann::ordered_json record =
          ReadJsonFile(recordPath, detail::kMaxMetadataBytes,
                       "Notion KEX installation record");
      if (record.is_object()) {
        const auto version = record.find("version");
        if (version != record.end() && version->is_string()) {
          status.version = version->get<std::string>();
        }
        const auto files = record.find("files");
        if (files != record.end() && files->is_object()) {
          for (auto iterator = files->begin(); iterator != files->end();
               ++iterator) {
            ExtensionFileStatus file;
            file.name = iterator.key();
            const fs::path target = installRoot / detail::FromUtf8(file.name);
            file.present = detail::FileExists(target);
            if (iterator.value().is_string()) {
              file.expectedHash = iterator.value().get<std::string>();
            }
            if (file.present && !file.expectedHash.empty()) {
              std::string hash;
              std::string error;
              const FileHashState state = HashFile(target, hash, error);
              file.hashMatches =
                  state == FileHashState::Hashed && hash == file.expectedHash;
            }
            if (!file.hashMatches) {
              status.problems.push_back("File missing or changed: " +
                                        file.name);
            }
            status.files.push_back(std::move(file));
          }
        }
      }
    } catch (const std::exception& exception) {
      status.problems.push_back(exception.what());
    }
  }

  try {
    const StatusResult native =
        Inspect(MakeRecordPaths(notionExe, notionData));
    status.nativeInstalled = native.installed;
    status.nativeMessage = native.message;
    if (!native.installed) {
      status.problems.push_back(native.message);
    }
  } catch (const std::exception& exception) {
    status.problems.push_back(exception.what());
  }

  status.healthy = status.problems.empty();
  return status;
}

}  // namespace notion_kex
