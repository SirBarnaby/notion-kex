#include "notion_kex/shortcuts.hpp"

#include "file_util.hpp"
#include "notion_kex/hash.hpp"
#include "path_util.hpp"
#include "process.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace notion_kex {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kAppUserModelId[] = L"com.electron.notion";
constexpr wchar_t kLauncherName[] = L"Notion KEX.exe";

std::string HResultMessage(HRESULT result) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0,
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring text =
      (length != 0 && buffer != nullptr)
          ? std::wstring(buffer, static_cast<std::size_t>(length))
          : L"HRESULT " + std::to_wstring(static_cast<unsigned long>(result));
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return detail::ToUtf8(text);
}

[[noreturn]] void ThrowHResult(std::string_view action, HRESULT result) {
  throw std::runtime_error(std::string(action) + ": " +
                           HResultMessage(result));
}

class ComScope {
 public:
  ComScope() {
    const HRESULT result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                    COINIT_DISABLE_OLE1DDE);
    if (result == S_OK || result == S_FALSE) {
      initialized_ = true;
    } else if (result == RPC_E_CHANGED_MODE) {
      initialized_ = false;
    } else {
      ThrowHResult("COM could not be initialized", result);
    }
  }
  ~ComScope() {
    if (initialized_) {
      CoUninitialize();
    }
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  bool initialized_ = false;
};

bool ContainsIgnoreCase(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  const auto lower = [](wchar_t value) {
    return static_cast<wchar_t>(std::towlower(value));
  };
  std::wstring haystack(text);
  std::wstring pattern(needle);
  std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
  std::transform(pattern.begin(), pattern.end(), pattern.begin(), lower);
  return haystack.find(pattern) != std::wstring::npos;
}

fs::path ModulePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD size =
      GetModuleFileNameW(nullptr, buffer.data(),
                         static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    throw std::runtime_error("The running executable path could not be read.");
  }
  return fs::path(std::wstring(buffer.data(), size));
}

fs::path KnownFolder(REFKNOWNFOLDERID identifier) {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(identifier, KF_FLAG_DEFAULT, nullptr,
                                  &value))) {
    return {};
  }
  fs::path result(value);
  CoTaskMemFree(value);
  return result;
}

fs::path EnvironmentPath(const wchar_t* name) {
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetEnvironmentVariableW(name, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return {};
  }
  return fs::path(std::wstring(buffer.data(), size));
}

bool ExistsAny(const fs::path& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsReparsePoint(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string HashOrThrow(const fs::path& path) {
  std::string hash;
  std::string error;
  const FileHashState state = HashFile(path, hash, error);
  if (state != FileHashState::Hashed) {
    throw std::runtime_error(state == FileHashState::Missing
                                 ? "The launcher executable is missing."
                                 : error);
  }
  return hash;
}

class ShellLink {
 public:
  ShellLink() {
    const HRESULT created =
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&link_));
    if (FAILED(created)) {
      ThrowHResult("The shortcut could not be created", created);
    }
    const HRESULT persisted = link_.As(&persist_);
    if (FAILED(persisted)) {
      ThrowHResult("The shortcut could not be opened", persisted);
    }
  }

  void Load(const fs::path& path) {
    const HRESULT result = persist_->Load(path.c_str(), STGM_READWRITE);
    if (FAILED(result)) {
      ThrowHResult("The shortcut could not be read", result);
    }
  }

  void Save(const fs::path& path) {
    const HRESULT result = persist_->Save(path.c_str(), TRUE);
    if (FAILED(result)) {
      ThrowHResult("The shortcut could not be saved", result);
    }
  }

  std::wstring Target() const {
    std::vector<wchar_t> buffer(32768);
    WIN32_FIND_DATAW data = {};
    const HRESULT result =
        link_->GetPath(buffer.data(), static_cast<int>(buffer.size()), &data,
                       SLGP_RAWPATH);
    if (FAILED(result)) {
      return {};
    }
    return std::wstring(buffer.data());
  }

  std::wstring Arguments() const {
    std::vector<wchar_t> buffer(32768);
    const HRESULT result =
        link_->GetArguments(buffer.data(), static_cast<int>(buffer.size()));
    if (FAILED(result)) {
      return {};
    }
    return std::wstring(buffer.data());
  }

  void PointTo(const fs::path& target, const std::wstring& arguments,
               const fs::path& workingDirectory, const fs::path& icon,
               const wchar_t* description) {
    const HRESULT pathResult = link_->SetPath(target.c_str());
    if (FAILED(pathResult)) {
      ThrowHResult("The shortcut target could not be set", pathResult);
    }
    const HRESULT argumentsResult = link_->SetArguments(arguments.c_str());
    if (FAILED(argumentsResult)) {
      ThrowHResult("The shortcut arguments could not be set", argumentsResult);
    }
    const HRESULT directoryResult =
        link_->SetWorkingDirectory(workingDirectory.c_str());
    if (FAILED(directoryResult)) {
      ThrowHResult("The shortcut working directory could not be set",
                   directoryResult);
    }
    const HRESULT iconResult = link_->SetIconLocation(icon.c_str(), 0);
    if (FAILED(iconResult)) {
      ThrowHResult("The shortcut icon could not be set", iconResult);
    }
    const HRESULT descriptionResult = link_->SetDescription(description);
    if (FAILED(descriptionResult)) {
      ThrowHResult("The shortcut description could not be set",
                   descriptionResult);
    }
    SetAppId();
  }

 private:
  void SetAppId() {
    ComPtr<IPropertyStore> store;
    if (FAILED(link_.As(&store))) {
      return;
    }
    const std::wstring identifier(kAppUserModelId);
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(InitPropVariantFromString(identifier.c_str(), &value))) {
      store->SetValue(PKEY_AppUserModel_ID, value);
      store->Commit();
      PropVariantClear(&value);
    }
  }

  ComPtr<IShellLinkW> link_;
  ComPtr<IPersistFile> persist_;
};

std::vector<fs::path> FindNotionShortcuts(const fs::path& root,
                                          bool recursive) {
  std::vector<fs::path> shortcuts;
  std::error_code error;
  if (!fs::exists(root, error) || error) {
    return shortcuts;
  }

  const auto consider = [&shortcuts](const fs::path& path) {
    if (path.extension() != L".lnk") {
      return;
    }
    if (!ContainsIgnoreCase(path.filename().wstring(), L"Notion")) {
      return;
    }
    shortcuts.push_back(path);
  };

  if (recursive) {
    try {
      for (const fs::directory_entry& entry :
           fs::recursive_directory_iterator(
               root, fs::directory_options::skip_permission_denied)) {
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && !fileError) {
          consider(entry.path());
        }
      }
    } catch (const fs::filesystem_error&) {
    }
  } else {
    try {
      for (const fs::directory_entry& entry :
           fs::directory_iterator(
               root, fs::directory_options::skip_permission_denied)) {
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && !fileError) {
          consider(entry.path());
        }
      }
    } catch (const fs::filesystem_error&) {
    }
  }
  return shortcuts;
}

nlohmann::ordered_json LoadManifest(const fs::path& path) {
  if (!detail::FileExists(path)) {
    nlohmann::ordered_json manifest;
    manifest["version"] = 1;
    manifest["shortcuts"] = nlohmann::ordered_json::array();
    manifest["created"] = nlohmann::ordered_json::array();
    return manifest;
  }
  try {
    nlohmann::ordered_json manifest =
        nlohmann::ordered_json::parse(detail::ReadFileText(
            path, detail::kMaxMetadataBytes, "The shortcut manifest"));
    if (!manifest.is_object()) {
      throw std::runtime_error("The shortcut restore manifest is invalid.");
    }
    if (!manifest.contains("shortcuts") ||
        !manifest["shortcuts"].is_array()) {
      manifest["shortcuts"] = nlohmann::ordered_json::array();
    }
    if (!manifest.contains("created") || !manifest["created"].is_array()) {
      manifest["created"] = nlohmann::ordered_json::array();
    }
    return manifest;
  } catch (const std::exception&) {
    throw std::runtime_error("The shortcut restore manifest is invalid.");
  }
}

bool HasShortcutEntry(const nlohmann::ordered_json& manifest,
                      const fs::path& path) {
  for (const auto& entry : manifest["shortcuts"]) {
    if (entry.is_object() && entry.contains("path") &&
        entry["path"].is_string() &&
        detail::EqualsPath(detail::FromUtf8(entry["path"].get<std::string>()),
                           path)) {
      return true;
    }
  }
  return false;
}

void AddShortcutBackup(nlohmann::ordered_json& manifest,
                       const fs::path& path, const fs::path& launcherRoot) {
  if (HasShortcutEntry(manifest, path)) {
    return;
  }
  const fs::path backup =
      launcherRoot /
      detail::FromUtf8("shortcut-" + detail::NewGuidString() + ".lnk");
  detail::CopyFileTo(path, backup);
  nlohmann::ordered_json entry;
  entry["path"] = detail::ToUtf8(path);
  entry["backup"] = detail::ToUtf8(backup);
  manifest["shortcuts"].push_back(std::move(entry));
}

void AddCreated(nlohmann::ordered_json& manifest, const fs::path& path,
                const char* kind) {
  nlohmann::ordered_json entry;
  entry["path"] = detail::ToUtf8(path);
  entry["kind"] = kind;
  manifest["created"].push_back(std::move(entry));
}

bool ManifestHasEntries(const nlohmann::ordered_json& manifest) {
  return !manifest["shortcuts"].empty() || !manifest["created"].empty();
}

void SaveManifest(const fs::path& path,
                  const nlohmann::ordered_json& manifest) {
  detail::WriteJsonAtomic(path, manifest);
}

void CreateShortcut(const fs::path& path, const fs::path& target,
                    const std::wstring& arguments,
                    const fs::path& workingDirectory, const fs::path& icon,
                    const wchar_t* description) {
  detail::CreateDirectories(path.parent_path());
  ShellLink link;
  link.PointTo(target, arguments, workingDirectory, icon, description);
  link.Save(path);
}

// Shortcuts made by the PowerShell installer start NotionKEX\tools\Launch.ps1.
// That script lives in the launcher folder that uninstall removes, so a
// restored wrapper is pointed straight at Notion.exe instead of left broken.
void RetargetLegacyWrapper(const fs::path& path,
                           const ShortcutLocations& locations) {
  try {
    ShellLink link;
    link.Load(path);
    if (!ContainsIgnoreCase(link.Arguments(),
                            L"NotionKEX\\tools\\Launch.ps1")) {
      return;
    }
    link.PointTo(locations.notionExe, L"", locations.notionExe.parent_path(),
                 locations.notionExe, L"Notion");
    link.Save(path);
  } catch (const std::exception&) {
    // The restored original stays as it was.
  }
}

}  // namespace

ShortcutLocations DefaultShortcutLocations(const fs::path& notionExe,
                                           const fs::path& launcherRoot) {
  ShortcutLocations locations;
  locations.desktop = KnownFolder(FOLDERID_Desktop);
  locations.startMenu = KnownFolder(FOLDERID_Programs);
  locations.startup = KnownFolder(FOLDERID_Startup);
  const fs::path appData = EnvironmentPath(L"APPDATA");
  if (!appData.empty()) {
    locations.taskbar = appData / "Microsoft" / "Internet Explorer" /
                        "Quick Launch" / "User Pinned" / "TaskBar";
  }
  locations.launcherRoot = launcherRoot;
  locations.notionExe = notionExe;
  locations.launcherExe = launcherRoot / kLauncherName;
  return locations;
}

ShortcutOutcome InstallShortcuts(const ShortcutLocations& locations) {
  if (locations.launcherRoot.empty() || locations.launcherExe.empty() ||
      locations.notionExe.empty()) {
    throw std::runtime_error("The shortcut locations are incomplete.");
  }
  if (IsReparsePoint(locations.launcherRoot)) {
    throw std::runtime_error(
        "Launcher folder must not be a symbolic link or junction.");
  }
  detail::CreateDirectories(locations.launcherRoot);

  ComScope com;
  const fs::path manifestPath = locations.launcherRoot / "shortcuts.json";
  nlohmann::ordered_json manifest = LoadManifest(manifestPath);

  ShortcutOutcome outcome;
  for (const fs::path& location :
       {locations.desktop, locations.startMenu, locations.taskbar}) {
    if (location.empty()) {
      continue;
    }
    for (const fs::path& path : FindNotionShortcuts(location, true)) {
      ShellLink link;
      try {
        link.Load(path);
      } catch (const std::exception&) {
        continue;
      }
      const std::wstring target = link.Target();
      const std::wstring arguments = link.Arguments();
      if (target.empty()) {
        continue;
      }
      const bool isLauncher =
          detail::EqualsPath(fs::path(target), locations.launcherExe);
      const bool isNotion =
          detail::EqualsPath(fs::path(target), locations.notionExe);
      const bool oldWrapper =
          ContainsIgnoreCase(arguments, L"NotionKEX\\tools\\Launch.ps1");
      if (isLauncher) {
        continue;
      }
      if (isNotion && !arguments.empty()) {
        continue;
      }
      if (!isNotion && !oldWrapper) {
        continue;
      }

      AddShortcutBackup(manifest, path, locations.launcherRoot);
      link.PointTo(locations.launcherExe, L"", locations.launcherRoot,
                   locations.notionExe, L"Notion");
      link.Save(path);
      ++outcome.updated;
    }
  }

  if (!locations.desktop.empty()) {
    const fs::path desktopShortcut =
        locations.desktop / "Notion KEX.lnk";
    if (!detail::FileExists(desktopShortcut)) {
      CreateShortcut(desktopShortcut, locations.launcherExe, L"",
                     locations.launcherRoot, locations.notionExe, L"Notion");
      AddCreated(manifest, desktopShortcut, "desktop");
      ++outcome.created;
    }
  }

  if (!locations.startup.empty()) {
    const fs::path startupShortcut =
        locations.startup / "Notion KEX.lnk";
    if (!detail::FileExists(startupShortcut)) {
      CreateShortcut(startupShortcut, locations.launcherExe,
                     L"--signin-check", locations.launcherRoot,
                     locations.notionExe, L"Notion KEX sign-in check");
      AddCreated(manifest, startupShortcut, "startup");
      ++outcome.created;
    }
  }

  if (ManifestHasEntries(manifest) || detail::FileExists(manifestPath)) {
    SaveManifest(manifestPath, manifest);
  }

  outcome.changed = outcome.updated > 0 || outcome.created > 0;
  outcome.message = "Updated " + std::to_string(outcome.updated) +
                    " Notion shortcut(s) and created " +
                    std::to_string(outcome.created) + " launcher shortcut(s).";
  return outcome;
}

ShortcutOutcome UninstallShortcuts(const ShortcutLocations& locations) {
  ComScope com;
  const fs::path manifestPath = locations.launcherRoot / "shortcuts.json";
  ShortcutOutcome outcome;
  nlohmann::ordered_json manifest;
  bool hasManifest = false;
  if (detail::FileExists(manifestPath)) {
    manifest = LoadManifest(manifestPath);
    hasManifest = true;
  }

  if (hasManifest) {
    for (const auto& entry : manifest["shortcuts"]) {
      if (!entry.is_object() || !entry.contains("path") ||
          !entry.contains("backup") || !entry["path"].is_string() ||
          !entry["backup"].is_string()) {
        continue;
      }
      const fs::path path = detail::FromUtf8(entry["path"].get<std::string>());
      const fs::path backup =
          detail::FromUtf8(entry["backup"].get<std::string>());
      if (!detail::FileExists(backup)) {
        continue;
      }
      bool restore = !detail::FileExists(path);
      if (!restore) {
        try {
          ShellLink link;
          link.Load(path);
          restore = detail::EqualsPath(fs::path(link.Target()),
                                       locations.launcherExe);
        } catch (const std::exception&) {
          restore = false;
        }
      }
      if (restore) {
        detail::CopyFileTo(backup, path);
        RetargetLegacyWrapper(path, locations);
        ++outcome.restored;
      }
      detail::RemoveFile(backup);
    }

    for (const auto& entry : manifest["created"]) {
      if (!entry.is_object() || !entry.contains("path") ||
          !entry["path"].is_string()) {
        continue;
      }
      const fs::path path = detail::FromUtf8(entry["path"].get<std::string>());
      if (!detail::FileExists(path)) {
        continue;
      }
      try {
        ShellLink link;
        link.Load(path);
        if (detail::EqualsPath(fs::path(link.Target()),
                               locations.launcherExe)) {
          detail::RemoveFile(path);
        }
      } catch (const std::exception&) {
      }
    }
  } else {
    for (const fs::path& path :
         {locations.desktop / "Notion KEX.lnk",
          locations.startup / "Notion KEX.lnk"}) {
      if (!detail::FileExists(path)) {
        continue;
      }
      try {
        ShellLink link;
        link.Load(path);
        if (detail::EqualsPath(fs::path(link.Target()),
                               locations.launcherExe)) {
          detail::RemoveFile(path);
        }
      } catch (const std::exception&) {
      }
    }
  }

  detail::RemoveFile(manifestPath);
  outcome.changed = outcome.restored > 0;
  outcome.message =
      "Restored " + std::to_string(outcome.restored) + " shortcut(s).";
  return outcome;
}

fs::path InstallLauncherExecutable(const fs::path& launcherRoot) {
  if (IsReparsePoint(launcherRoot)) {
    throw std::runtime_error(
        "Launcher folder must not be a symbolic link or junction.");
  }
  detail::CreateDirectories(launcherRoot);

  const fs::path source = ModulePath();
  const fs::path destination = launcherRoot / kLauncherName;
  if (detail::EqualsPath(source, destination)) {
    return destination;
  }

  // A previous install may have left a renamed running image behind.
  std::error_code error;
  for (const fs::directory_entry& entry :
       fs::directory_iterator(launcherRoot, error)) {
    if (error) {
      break;
    }
    const std::wstring name = entry.path().filename().wstring();
    if (name.rfind(L"Notion KEX.exe.old-", 0) == 0) {
      ::DeleteFileW(entry.path().c_str());
    }
  }

  if (ExistsAny(destination)) {
    const fs::path old =
        launcherRoot /
        detail::FromUtf8("Notion KEX.exe.old-" + detail::NewGuidString());
    if (!::MoveFileExW(destination.c_str(), old.c_str(),
                       MOVEFILE_REPLACE_EXISTING)) {
      throw std::runtime_error(
          "The existing launcher could not be replaced: " +
          detail::Win32Error(GetLastError()));
    }
    ::DeleteFileW(old.c_str());
  }

  const fs::path temporary =
      launcherRoot /
      detail::FromUtf8("Notion KEX.exe.new-" + detail::NewGuidString());
  try {
    detail::CopyFileTo(source, temporary);
    detail::MoveFileTo(temporary, destination);
  } catch (...) {
    detail::RemoveFile(temporary);
    throw;
  }

  if (HashOrThrow(destination) != HashOrThrow(source)) {
    throw std::runtime_error("Launcher installation verification failed.");
  }
  return destination;
}

void RemoveLauncherFolder(const fs::path& launcherRoot) {
  if (launcherRoot.empty()) {
    return;
  }
  const fs::path module = ModulePath();
  if (!detail::IsUnderDirectory(module, launcherRoot)) {
    if (ExistsAny(launcherRoot)) {
      detail::RemoveTree(launcherRoot);
    }
    return;
  }

  const fs::path temporary =
      fs::temp_directory_path() /
      detail::FromUtf8("NotionKEX-cleanup-" + detail::NewGuidString() +
                       ".exe");
  detail::CopyFileTo(module, temporary);
  const std::wstring arguments =
      L"--cleanup " + std::to_wstring(GetCurrentProcessId()) + L" \"" +
      launcherRoot.wstring() + L"\"";
  if (!detail::SpawnDetached(temporary, arguments,
                             fs::temp_directory_path())) {
    throw std::runtime_error("The launcher cleanup process could not start.");
  }
}

int RunCleanupProcess(unsigned long parentPid,
                      const fs::path& launcherRoot) {
  HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
  if (parent != nullptr) {
    WaitForSingleObject(parent, 60000);
    CloseHandle(parent);
  }

  for (int attempt = 0; attempt < 60; ++attempt) {
    try {
      if (ExistsAny(launcherRoot)) {
        detail::RemoveTree(launcherRoot);
      }
      break;
    } catch (...) {
      Sleep(500);
    }
  }
  return 0;
}

}  // namespace notion_kex
