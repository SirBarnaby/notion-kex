#include "notion_kex/status.hpp"

#include "path_util.hpp"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifndef KEX_STATUS_VERSION
#define KEX_STATUS_VERSION "0.1.0"
#endif

namespace {

namespace fs = std::filesystem;

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
    if (!WriteFile(handle, bytes + offset, request, &written, nullptr) ||
        written == 0) {
      break;
    }
    offset += written;
  }
}

void PrintUsage(HANDLE handle) {
  WriteUtf8(
      handle,
      "Usage: kex-status [options]\n"
      "\n"
      "Read-only inspection of the Notion KEX native zoom state. This component\n"
      "never repairs, writes, launches or watches anything.\n"
      "\n"
      "Paths (choose one of each):\n"
      "  --asar <path>          Explicit app.asar path.\n"
      "  --notion-exe <path>    Derive <dir>/resources/app.asar from Notion.exe.\n"
      "  --record-root <path>   Explicit \"Notion KEX\" metadata directory.\n"
      "  --notion-data <path>   Derive <path>/Notion KEX from the Notion data dir.\n"
      "\n"
      "Output:\n"
      "  --json                 Emit one JSON object instead of key/value text.\n"
      "  --help, -h             Show this help.\n"
      "  --version              Show the component version.\n"
      "\n"
      "Exit codes:\n"
      "  0 installed\n"
      "  1 not installed\n"
      "  2 usage error\n"
      "  3 archive changed\n"
      "  4 invalid record\n"
      "  5 recovery required\n"
      "  6 invalid recovery\n");
}

fs::path AbsoluteNormal(const fs::path& path) {
  std::error_code error;
  fs::path result = fs::absolute(path, error);
  if (error) {
    result = path;
  }
  return result.lexically_normal();
}

struct Options {
  std::optional<fs::path> asar;
  std::optional<fs::path> notionExe;
  std::optional<fs::path> recordRoot;
  std::optional<fs::path> notionData;
  bool json = false;
  bool help = false;
  bool version = false;
};

bool TakePath(int argc, wchar_t** argv, int& index, std::string_view label,
              std::optional<fs::path>& target, std::string& error) {
  if (target.has_value()) {
    error = "Option " + std::string(label) + " was supplied more than once.";
    return false;
  }
  if (index + 1 >= argc) {
    error = "Option " + std::string(label) + " requires a path.";
    return false;
  }
  target = fs::path(argv[++index]);
  return true;
}

bool ParseArguments(int argc, wchar_t** argv, Options& options,
                    std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
      options.help = true;
    } else if (argument == L"--version") {
      options.version = true;
    } else if (argument == L"--json") {
      options.json = true;
    } else if (argument == L"--asar") {
      if (!TakePath(argc, argv, index, "--asar", options.asar, error)) {
        return false;
      }
    } else if (argument == L"--notion-exe") {
      if (!TakePath(argc, argv, index, "--notion-exe", options.notionExe,
                    error)) {
        return false;
      }
    } else if (argument == L"--record-root") {
      if (!TakePath(argc, argv, index, "--record-root", options.recordRoot,
                    error)) {
        return false;
      }
    } else if (argument == L"--notion-data") {
      if (!TakePath(argc, argv, index, "--notion-data", options.notionData,
                    error)) {
        return false;
      }
    } else {
      error = "Unknown option. Run kex-status --help for usage.";
      return false;
    }
  }
  return true;
}

bool ResolvePaths(const Options& options, notion_kex::RecordPaths& paths,
                  std::string& error) {
  if (options.asar.has_value() && options.notionExe.has_value()) {
    error = "--asar and --notion-exe are mutually exclusive.";
    return false;
  }
  if (options.recordRoot.has_value() && options.notionData.has_value()) {
    error = "--record-root and --notion-data are mutually exclusive.";
    return false;
  }
  if (!options.asar.has_value() && !options.notionExe.has_value()) {
    error = "Specify --asar or --notion-exe.";
    return false;
  }
  if (!options.recordRoot.has_value() && !options.notionData.has_value()) {
    error = "Specify --record-root or --notion-data.";
    return false;
  }

  if (options.asar.has_value()) {
    paths.asarPath = *options.asar;
  } else {
    paths.asarPath = options.notionExe->parent_path() / "resources" / "app.asar";
  }
  if (options.recordRoot.has_value()) {
    paths.recordRoot = *options.recordRoot;
  } else {
    paths.recordRoot = *options.notionData / "Notion KEX";
  }

  paths.asarPath = AbsoluteNormal(paths.asarPath);
  paths.recordRoot = AbsoluteNormal(paths.recordRoot);
  return true;
}

nlohmann::json BuildJson(const notion_kex::StatusResult& result) {
  nlohmann::json json;
  json["component"] = "kex-status";
  json["version"] = KEX_STATUS_VERSION;
  json["status"] = notion_kex::StatusCodeName(result.code);
  json["installed"] = result.installed;
  json["message"] = result.message;
  json["detail"] = result.detail;
  json["asarPath"] = notion_kex::detail::ToUtf8(result.paths.asarPath);
  json["recordRoot"] = notion_kex::detail::ToUtf8(result.paths.recordRoot);
  json["recordPath"] = notion_kex::detail::ToUtf8(result.paths.recordPath());
  json["pendingPath"] = notion_kex::detail::ToUtf8(result.paths.pendingPath());
  json["currentHash"] = result.currentHash;
  json["expectedHash"] = result.expectedHash;
  json["originalHash"] = result.originalHash;
  json["backupPath"] = result.backupPath;
  json["recoveryState"] = result.recoveryState;
  return json;
}

std::string BuildText(const notion_kex::StatusResult& result) {
  std::ostringstream output;
  output << "status: " << notion_kex::StatusCodeName(result.code) << "\n";
  output << "installed: " << (result.installed ? "true" : "false") << "\n";
  output << "message: " << result.message << "\n";
  if (!result.detail.empty()) {
    output << "detail: " << result.detail << "\n";
  }
  output << "asar: " << notion_kex::detail::ToUtf8(result.paths.asarPath)
         << "\n";
  output << "record-root: "
         << notion_kex::detail::ToUtf8(result.paths.recordRoot) << "\n";
  output << "record: "
         << notion_kex::detail::ToUtf8(result.paths.recordPath()) << "\n";
  output << "pending: "
         << notion_kex::detail::ToUtf8(result.paths.pendingPath()) << "\n";
  if (!result.currentHash.empty()) {
    output << "current-hash: " << result.currentHash << "\n";
  }
  if (!result.expectedHash.empty()) {
    output << "expected-hash: " << result.expectedHash << "\n";
  }
  if (!result.originalHash.empty()) {
    output << "original-hash: " << result.originalHash << "\n";
  }
  if (!result.backupPath.empty()) {
    output << "backup: " << result.backupPath << "\n";
  }
  if (!result.recoveryState.empty()) {
    output << "recovery: " << result.recoveryState << "\n";
  }
  return output.str();
}

int ExitCodeFor(notion_kex::StatusCode code) {
  switch (code) {
    case notion_kex::StatusCode::Installed:
      return 0;
    case notion_kex::StatusCode::NotInstalled:
      return 1;
    case notion_kex::StatusCode::ArchiveChanged:
      return 3;
    case notion_kex::StatusCode::InvalidRecord:
      return 4;
    case notion_kex::StatusCode::RecoveryRequired:
      return 5;
    case notion_kex::StatusCode::InvalidRecovery:
      return 6;
  }
  return 7;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options options;
  std::string error;
  if (!ParseArguments(argc, argv, options, error)) {
    WriteUtf8(GetStdHandle(STD_ERROR_HANDLE),
              "kex-status: " + error + "\n\n");
    PrintUsage(GetStdHandle(STD_ERROR_HANDLE));
    return 2;
  }

  if (options.help) {
    PrintUsage(GetStdHandle(STD_OUTPUT_HANDLE));
    return 0;
  }
  if (options.version) {
    WriteUtf8(GetStdHandle(STD_OUTPUT_HANDLE),
              std::string("kex-status ") + KEX_STATUS_VERSION + "\n");
    return 0;
  }

  notion_kex::RecordPaths paths;
  if (!ResolvePaths(options, paths, error)) {
    WriteUtf8(GetStdHandle(STD_ERROR_HANDLE),
              "kex-status: " + error + "\n\n");
    PrintUsage(GetStdHandle(STD_ERROR_HANDLE));
    return 2;
  }

  const notion_kex::StatusResult result = notion_kex::Inspect(paths);
  const std::string output =
      options.json ? BuildJson(result).dump(2) + "\n" : BuildText(result);
  WriteUtf8(GetStdHandle(STD_OUTPUT_HANDLE), output);
  return ExitCodeFor(result.code);
}
