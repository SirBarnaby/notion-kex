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
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef KEX_VERSION
#define KEX_VERSION "0.1.0"
#endif

namespace {

namespace fs = std::filesystem;

struct Options {
  std::string mode = "launch";
  bool modeExplicit = false;
  fs::path notionExe;
  fs::path notionData;
  fs::path launcherRoot;
  bool json = false;
  bool noLaunch = false;
  bool noShortcuts = false;
  bool quiet = false;
  bool help = false;
  unsigned long cleanupPid = 0;
  fs::path cleanupFolder;
};

struct CommandResult {
  int exitCode = 0;
  std::string text;
  bool error = false;
};

std::wstring Widen(std::string_view text) {
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

void WriteUtf8(HANDLE handle, std::string_view text) {
  if (text.empty() || handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) != 0) {
    const std::wstring wide = Widen(text);
    if (wide.empty()) {
      return;
    }
    DWORD written = 0;
    WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()),
                  &written, nullptr);
    return;
  }
  DWORD written = 0;
  WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written,
            nullptr);
}

void ShowMessage(std::string_view text, bool error) {
  const std::wstring wide = Widen(text);
  if (wide.empty()) {
    return;
  }
  MessageBoxW(nullptr, wide.c_str(), L"Notion KEX",
              MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

// A pipe or a regular file means the caller asked for machine-readable output,
// even when no console is attached. Character devices such as NUL are not
// treated as writable output.
bool IsWritableStream(HANDLE handle) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const DWORD type = GetFileType(handle);
  return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
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

std::string LowerAscii(std::wstring value) {
  std::string result;
  result.reserve(value.size());
  for (const wchar_t character : value) {
    if (character >= L'A' && character <= L'Z') {
      result.push_back(static_cast<char>(character - L'A' + L'a'));
    } else if (character < 128) {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('?');
    }
  }
  return result;
}

bool ParseUnsignedLong(const std::wstring& text, unsigned long& out) {
  if (text.empty()) {
    return false;
  }
  wchar_t* end = nullptr;
  const unsigned long value = wcstoul(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != L'\0') {
    return false;
  }
  out = value;
  return true;
}

Options DefaultOptions() {
  Options options;
  const fs::path localAppData = EnvironmentPath(L"LOCALAPPDATA");
  const fs::path appData = EnvironmentPath(L"APPDATA");
  options.notionExe =
      localAppData / "Programs" / "Notion" / "Notion.exe";
  options.notionData = appData / "Notion";
  options.launcherRoot = localAppData / "NotionKEX";
  return options;
}

bool ParseArguments(int argc, wchar_t** argv, Options& options,
                    std::string& error) {
  options = DefaultOptions();
  bool modeSeen = false;
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    const auto next = [&](fs::path& target) {
      if (index + 1 >= argc) {
        error = "The option " + LowerAscii(argument) + " requires a value.";
        return false;
      }
      target = fs::path(argv[++index]);
      return true;
    };

    if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
      options.help = true;
      continue;
    }
    if (argument == L"--json") {
      options.json = true;
      continue;
    }
    if (argument == L"--no-launch") {
      options.noLaunch = true;
      continue;
    }
    if (argument == L"--no-shortcuts") {
      options.noShortcuts = true;
      continue;
    }
    if (argument == L"--quiet") {
      options.quiet = true;
      continue;
    }
    if (argument == L"--notion-exe") {
      if (!next(options.notionExe)) {
        return false;
      }
      continue;
    }
    if (argument == L"--notion-data") {
      if (!next(options.notionData)) {
        return false;
      }
      continue;
    }
    if (argument == L"--launcher-root") {
      if (!next(options.launcherRoot)) {
        return false;
      }
      continue;
    }
    if (argument == L"--cleanup") {
      if (index + 2 >= argc) {
        error = "The cleanup mode requires a process id and a folder.";
        return false;
      }
      options.mode = "cleanup";
      if (!ParseUnsignedLong(argv[++index], options.cleanupPid)) {
        error = "The cleanup process id is invalid.";
        return false;
      }
      options.cleanupFolder = fs::path(argv[++index]);
      modeSeen = true;
      continue;
    }
    if (!argument.empty() && argument.front() == L'-') {
      error = "Unknown option: " + LowerAscii(argument);
      return false;
    }
    if (modeSeen) {
      error = "Unexpected argument: " + LowerAscii(argument);
      return false;
    }
    options.mode = LowerAscii(argument);
    options.modeExplicit = true;
    modeSeen = true;
  }
  return true;
}

std::string HelpText() {
  return
      "Notion KEX " KEX_VERSION "\n"
      "\n"
      "Usage: Notion KEX.exe [mode] [options]\n"
      "\n"
      "Modes:\n"
      "  launch              Repair if needed, then start Notion (default).\n"
      "  status              Report extension, native zoom and launcher state.\n"
      "  install             Install the extension, native zoom and shortcuts.\n"
      "  uninstall           Restore shortcuts and remove Notion KEX.\n"
      "  repair              Reapply the native zoom patch with Notion closed.\n"
      "  validate            Check whether this app archive is supported.\n"
      "  watch               Hidden update watcher used by the zoom payload.\n"
      "  signin-check        Hidden startup repair check.\n"
      "  shortcut-install    Install or refresh launcher shortcuts only.\n"
      "  shortcut-uninstall  Restore launcher shortcuts only.\n"
      "\n"
      "Options:\n"
      "  --notion-exe <path>    Notion.exe path.\n"
      "  --notion-data <path>   Notion profile directory.\n"
      "  --launcher-root <path> Notion KEX launcher directory.\n"
      "  --json                 Print machine-readable output.\n"
      "  --no-launch            Do not start Notion after install/launch.\n"
      "  --no-shortcuts         Leave desktop, startup and pinned shortcuts unchanged.\n"
      "  --quiet                Suppress success output.\n"
      "  --help                 Show this help.\n";
}

nlohmann::ordered_json NativeStatusJson(const notion_kex::StatusResult& status) {
  nlohmann::ordered_json json;
  json["code"] = notion_kex::StatusCodeName(status.code);
  json["installed"] = status.installed;
  json["message"] = status.message;
  json["detail"] = status.detail;
  json["asarPath"] = notion_kex::detail::ToUtf8(status.paths.asarPath);
  json["recordRoot"] = notion_kex::detail::ToUtf8(status.paths.recordRoot);
  json["currentHash"] = status.currentHash;
  json["expectedHash"] = status.expectedHash;
  json["originalHash"] = status.originalHash;
  json["backupPath"] = status.backupPath;
  json["recoveryState"] = status.recoveryState;
  return json;
}

nlohmann::ordered_json ExtensionStatusJson(
    const notion_kex::ExtensionStatus& status) {
  nlohmann::ordered_json json;
  json["installed"] = status.installed;
  json["healthy"] = status.healthy;
  json["version"] = status.version;
  json["installRoot"] = status.installRoot;
  json["notionVersion"] = status.notionVersion;
  json["nativeInstalled"] = status.nativeInstalled;
  json["nativeMessage"] = status.nativeMessage;
  nlohmann::ordered_json files = nlohmann::ordered_json::array();
  for (const notion_kex::ExtensionFileStatus& file : status.files) {
    nlohmann::ordered_json entry;
    entry["name"] = file.name;
    entry["present"] = file.present;
    entry["hashMatches"] = file.hashMatches;
    entry["expectedHash"] = file.expectedHash;
    files.push_back(std::move(entry));
  }
  json["files"] = std::move(files);
  json["problems"] = status.problems;
  return json;
}

CommandResult StatusCommand(const Options& options) {
  const notion_kex::ExtensionStatus extension =
      notion_kex::InspectExtension(options.notionExe, options.notionData);
  const notion_kex::StatusResult native = notion_kex::Inspect(
      notion_kex::MakeRecordPaths(options.notionExe, options.notionData));
  const fs::path launcherExe = options.launcherRoot / "Notion KEX.exe";
  const bool launcherInstalled = notion_kex::detail::FileExists(launcherExe);

  CommandResult result;
  result.exitCode =
      extension.healthy && native.installed ? 0 : 1;
  result.error = result.exitCode != 0;

  if (options.json) {
    nlohmann::ordered_json json;
    json["extension"] = ExtensionStatusJson(extension);
    json["native"] = NativeStatusJson(native);
    nlohmann::ordered_json launcher;
    launcher["root"] = notion_kex::detail::ToUtf8(options.launcherRoot);
    launcher["exe"] = notion_kex::detail::ToUtf8(launcherExe);
    launcher["installed"] = launcherInstalled;
    json["launcher"] = std::move(launcher);
    result.text = json.dump(2);
    return result;
  }

  std::string text = "Notion KEX";
  if (!extension.version.empty()) {
    text += " " + extension.version;
  }
  text += "\nExtension installed: ";
  text += extension.installed ? "True" : "False";
  text += "\nLocation: " + extension.installRoot;
  if (!extension.notionVersion.empty()) {
    text += "\nNotion version: " + extension.notionVersion;
  }
  if (!extension.version.empty()) {
    text += "\nInstalled package: " + extension.version;
  }
  for (const notion_kex::ExtensionFileStatus& file : extension.files) {
    if (!file.hashMatches) {
      text += "\nWarning: file missing or changed: " + file.name;
    }
  }
  text += "\n" + native.message;
  if (!native.detail.empty()) {
    text += "\n" + native.detail;
  }
  text += "\nLauncher installed: ";
  text += launcherInstalled ? "True" : "False";
  text += "\nRuntime check: View -> Extensions -> Notion KEX. A stored "
          "extension alone does not prove the native permission is enabled.";
  for (const std::string& problem : extension.problems) {
    if (problem != native.message) {
      text += "\nWarning: " + problem;
    }
  }
  result.text = std::move(text);
  return result;
}

CommandResult InstallCommand(const Options& options) {
  CommandResult result;
  try {
    const fs::path launcherExe =
        notion_kex::InstallLauncherExecutable(options.launcherRoot);
    const notion_kex::SetupOutcome setup =
        notion_kex::InstallExtension(options.notionExe, options.notionData);
    std::string shortcutMessage = "Shortcuts were left unchanged.";
    if (!options.noShortcuts) {
      const notion_kex::ShortcutLocations locations =
          notion_kex::DefaultShortcutLocations(options.notionExe,
                                               options.launcherRoot);
      const notion_kex::ShortcutOutcome shortcuts =
          notion_kex::InstallShortcuts(locations);
      shortcutMessage = shortcuts.message;
    }

    std::string text = setup.message + "\n" + shortcutMessage;
    text += "\nLauncher: " + notion_kex::detail::ToUtf8(launcherExe);
    text +=
        "\nOne-time native permission: focus the Notion desktop window, press "
        "and release Alt, then choose Debug -> Extensions Enabled.";
    text +=
        "\nThis is the application menu, separate from a page or workspace "
        "menu. Then Quit and reopen Notion.";
    text +=
        "\nSettings: View -> Extensions -> Notion KEX. Pinch with two fingers "
        "to zoom around the pointer.";
    text +=
        "\nAfter a Notion app update, the launcher repairs native pinch on the "
        "next full quit.";
    if (!options.noLaunch) {
      notion_kex::StartNotion(options.notionExe);
    }
    result.text = std::move(text);
    return result;
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult UninstallCommand(const Options& options) {
  CommandResult result;
  try {
    const fs::path installRecord =
        options.notionData / "Notion KEX" / "install.json";
    if (!notion_kex::detail::FileExists(installRecord)) {
      throw std::runtime_error(
          "No Notion KEX installation record was found.");
    }

    const notion_kex::SetupOutcome setup =
        notion_kex::UninstallExtension(options.notionExe, options.notionData);
    std::string shortcutMessage = "Shortcuts were left unchanged.";
    if (!options.noShortcuts) {
      const notion_kex::ShortcutLocations locations =
          notion_kex::DefaultShortcutLocations(options.notionExe,
                                               options.launcherRoot);
      const notion_kex::ShortcutOutcome shortcuts =
          notion_kex::UninstallShortcuts(locations);
      shortcutMessage = shortcuts.message;
    }
    notion_kex::RemoveLauncherFolder(options.launcherRoot);

    result.text = setup.message + "\n" + shortcutMessage;
    return result;
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult RepairCommand(const Options& options) {
  try {
    const notion_kex::RecordPaths paths =
        notion_kex::MakeRecordPaths(options.notionExe, options.notionData);
    const notion_kex::NativeZoomOutcome outcome =
        notion_kex::RepairNativeZoom(paths, options.notionExe);
    return CommandResult{0, outcome.message, false};
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult ValidateCommand(const Options& options) {
  try {
    const notion_kex::RecordPaths paths =
        notion_kex::MakeRecordPaths(options.notionExe, options.notionData);
    const notion_kex::NativeZoomOutcome outcome =
        notion_kex::ValidateNativeZoom(paths, options.notionExe);
    return CommandResult{0, outcome.message, false};
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult ShortcutInstallCommand(const Options& options) {
  try {
    notion_kex::InstallLauncherExecutable(options.launcherRoot);
    const notion_kex::ShortcutLocations locations =
        notion_kex::DefaultShortcutLocations(options.notionExe,
                                             options.launcherRoot);
    const notion_kex::ShortcutOutcome outcome =
        notion_kex::InstallShortcuts(locations);
    return CommandResult{0, outcome.message, false};
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult ShortcutUninstallCommand(const Options& options) {
  try {
    const notion_kex::ShortcutLocations locations =
        notion_kex::DefaultShortcutLocations(options.notionExe,
                                             options.launcherRoot);
    const notion_kex::ShortcutOutcome outcome =
        notion_kex::UninstallShortcuts(locations);
    return CommandResult{0, outcome.message, false};
  } catch (const std::exception& exception) {
    return CommandResult{1, exception.what(), true};
  }
}

CommandResult LaunchCommand(const Options& options) {
  notion_kex::LauncherOptions launcher;
  launcher.notionExe = options.notionExe;
  launcher.notionData = options.notionData;
  launcher.launcherRoot = options.launcherRoot;
  launcher.noLaunch = options.noLaunch;
  const notion_kex::LauncherResult outcome = notion_kex::RunLauncher(launcher);
  return CommandResult{outcome.exitCode, outcome.message,
                       outcome.exitCode != 0};
}

CommandResult WatchCommand(const Options& options) {
  notion_kex::LauncherOptions launcher;
  launcher.notionExe = options.notionExe;
  launcher.notionData = options.notionData;
  launcher.launcherRoot = options.launcherRoot;
  const notion_kex::LauncherResult outcome = notion_kex::RunWatcher(launcher);
  return CommandResult{outcome.exitCode, outcome.message,
                       outcome.exitCode != 0};
}

CommandResult SignInCommand(const Options& options) {
  notion_kex::LauncherOptions launcher;
  launcher.notionExe = options.notionExe;
  launcher.notionData = options.notionData;
  launcher.launcherRoot = options.launcherRoot;
  const notion_kex::LauncherResult outcome =
      notion_kex::RunSignInCheck(launcher);
  return CommandResult{outcome.exitCode, outcome.message,
                       outcome.exitCode != 0};
}

fs::path RunningModulePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return {};
  }
  return fs::path(std::wstring(buffer.data(), size));
}

// A double-click on a downloaded copy, outside the installed launcher folder,
// is a request to set up or update Notion KEX. The installed launcher, every
// shortcut and every explicit mode keep their existing meaning.
bool IsSetupDoubleClick(const Options& options) {
  if (options.modeExplicit || options.json || options.quiet) {
    return false;
  }
  const fs::path module = RunningModulePath();
  return !module.empty() &&
         !notion_kex::detail::IsUnderDirectory(module, options.launcherRoot);
}

int Ask(std::string_view text, UINT buttons) {
  return MessageBoxW(nullptr, Widen(text).c_str(), L"Notion KEX",
                     buttons | MB_SETFOREGROUND);
}

// Returns false when the user cancels. Notion is never closed on the user's
// behalf; the dialog waits until they quit it from the tray.
bool WaitForNotionClosed(const Options& options) {
  while (notion_kex::detail::IsNotionRunning(options.notionExe)) {
    const int answer = Ask(
        "Notion is still running.\n\nRight-click the Notion icon in the "
        "notification area, choose Quit, then click Retry.",
        MB_RETRYCANCEL | MB_ICONWARNING);
    if (answer != IDRETRY) {
      return false;
    }
  }
  return true;
}

int RunSetupDoubleClick(const Options& options) {
  const bool installed = notion_kex::detail::FileExists(
      options.notionData / "Notion KEX" / "install.json");
  const std::string prompt =
      installed
          ? "Update Notion KEX to version " KEX_VERSION "?\n\nYour settings "
            "are kept. Notion needs to be closed while the update runs."
          : "Install Notion KEX " KEX_VERSION " into the Notion desktop app?"
            "\n\nIt adds comfortable page margins, a smooth caret and "
            "touchpad pinch zoom. Notion needs to be closed while it installs."
            " Run this file again any time to update.";
  if (Ask(prompt, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
    return 0;
  }
  if (!WaitForNotionClosed(options)) {
    return 0;
  }
  const CommandResult result = InstallCommand(options);
  ShowMessage(result.text, result.error);
  return result.exitCode;
}

CommandResult Dispatch(const Options& options) {
  if (options.mode == "launch") {
    return LaunchCommand(options);
  }
  if (options.mode == "status") {
    return StatusCommand(options);
  }
  if (options.mode == "install") {
    return InstallCommand(options);
  }
  if (options.mode == "uninstall") {
    return UninstallCommand(options);
  }
  if (options.mode == "repair") {
    return RepairCommand(options);
  }
  if (options.mode == "validate") {
    return ValidateCommand(options);
  }
  if (options.mode == "watch") {
    return WatchCommand(options);
  }
  if (options.mode == "signin-check") {
    return SignInCommand(options);
  }
  if (options.mode == "shortcut-install") {
    return ShortcutInstallCommand(options);
  }
  if (options.mode == "shortcut-uninstall") {
    return ShortcutUninstallCommand(options);
  }
  return CommandResult{2,
                       "Unknown mode: " + options.mode + "\n\n" + HelpText(),
                       true};
}

bool IsSilentMode(const std::string& mode) {
  return mode == "watch" || mode == "signin-check" || mode == "cleanup";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options options;
  std::string parseError;
  if (!ParseArguments(argc, argv, options, parseError)) {
    ShowMessage(parseError + "\n\n" + HelpText(), true);
    return 2;
  }
  if (options.help) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleWindow() != nullptr || IsWritableStream(output)) {
      WriteUtf8(output, HelpText());
    } else {
      ShowMessage(HelpText(), false);
    }
    return 0;
  }
  if (options.mode == "cleanup") {
    return notion_kex::RunCleanupProcess(options.cleanupPid,
                                         options.cleanupFolder);
  }

  AttachConsole(ATTACH_PARENT_PROCESS);
  const bool console = GetConsoleWindow() != nullptr;
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE errors = GetStdHandle(STD_ERROR_HANDLE);
  const bool redirected = IsWritableStream(output);

  if (!console && !redirected && IsSetupDoubleClick(options)) {
    return RunSetupDoubleClick(options);
  }

  const CommandResult result = Dispatch(options);

  if (!options.quiet && !result.text.empty()) {
    if (options.json || console || redirected) {
      WriteUtf8(result.error ? errors : output, result.text + "\n");
    } else if (!IsSilentMode(options.mode)) {
      if (options.mode == "launch") {
        if (result.error) {
          ShowMessage(result.text, true);
        }
      } else {
        ShowMessage(result.text, result.error);
      }
    }
  }
  return result.exitCode;
}
