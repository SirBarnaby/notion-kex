#include "process.hpp"

#include "file_util.hpp"
#include "notion_kex/hash.hpp"
#include "path_util.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <stdexcept>

namespace notion_kex::detail {
namespace {

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool EqualsName(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                              right.c_str(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

bool ContainsName(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  for (std::size_t index = 0; index + needle.size() <= text.size(); ++index) {
    if (CompareStringOrdinal(text.c_str() + index,
                             static_cast<int>(needle.size()), needle.c_str(),
                             static_cast<int>(needle.size()), TRUE) ==
        CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

std::wstring QueryProcessPath(DWORD pid) {
  UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) {
    return {};
  }
  std::vector<wchar_t> buffer(32768);
  DWORD size = static_cast<DWORD>(buffer.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size)) {
    return {};
  }
  return std::wstring(buffer.data(), size);
}

std::wstring LocalAppData() {
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return {};
  }
  return std::wstring(buffer.data(), size);
}

[[noreturn]] void ThrowProcess(std::string_view message, DWORD code) {
  throw std::runtime_error(std::string(message) + ": " + Win32Error(code));
}

}  // namespace

std::vector<ProcessEntry> EnumerateProcesses() {
  std::vector<ProcessEntry> entries;
  UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) {
    return entries;
  }

  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) {
    return entries;
  }
  do {
    ProcessEntry process;
    process.pid = entry.th32ProcessID;
    process.name.assign(entry.szExeFile);
    process.path = QueryProcessPath(entry.th32ProcessID);
    entries.push_back(std::move(process));
    entry.dwSize = sizeof(entry);
  } while (Process32NextW(snapshot.get(), &entry));
  return entries;
}

bool IsNotionRunning(const fs::path& notionExe) {
  const std::wstring target = fs::absolute(notionExe).lexically_normal().wstring();
  for (const ProcessEntry& process : EnumerateProcesses()) {
    if (!EqualsName(process.name, L"Notion.exe")) {
      continue;
    }
    if (process.path.empty()) {
      return true;
    }
    if (EqualsPath(fs::path(process.path), fs::path(target))) {
      return true;
    }
  }
  return false;
}

bool IsAnyNotionRunning() {
  for (const ProcessEntry& process : EnumerateProcesses()) {
    if (EqualsName(process.name, L"Notion.exe")) {
      return true;
    }
  }
  return false;
}

bool IsUpdaterRunning() {
  const std::wstring localAppData = LocalAppData();
  const fs::path updaterRoot =
      localAppData.empty() ? fs::path() : fs::path(localAppData) / "Notion-Updater";
  for (const ProcessEntry& process : EnumerateProcesses()) {
    if (ContainsName(process.name, L"Notion Setup") ||
        ContainsName(process.name, L"Notion-Setup")) {
      return true;
    }
    if (!process.path.empty() && !updaterRoot.empty() &&
        IsUnderDirectory(fs::path(process.path), updaterRoot)) {
      return true;
    }
  }
  return false;
}

void AssertNotionClosed(const fs::path& notionExe, std::string_view message) {
  if (IsNotionRunning(notionExe)) {
    throw std::runtime_error(std::string(message));
  }
}

void AssertElectronFuse(const fs::path& notionExe) {
  UniqueHandle handle(CreateFileW(notionExe.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    ThrowProcess("The Notion executable could not be opened", GetLastError());
  }

  constexpr std::string_view kSentinel =
      "dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX";
  std::vector<char> buffer(262144);
  std::string carry;
  std::uint64_t offset = 0;

  for (;;) {
    DWORD read = 0;
    if (!ReadFile(handle.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      ThrowProcess("The Notion executable could not be read", GetLastError());
    }
    if (read == 0) {
      break;
    }
    std::string text = carry;
    text.append(buffer.data(), read);
    const std::size_t index = text.find(kSentinel);
    if (index != std::string::npos) {
      const std::uint64_t position =
          offset - carry.size() + index + kSentinel.size();
      LARGE_INTEGER seek = {};
      seek.QuadPart = static_cast<LONGLONG>(position);
      if (!SetFilePointerEx(handle.get(), seek, nullptr, FILE_BEGIN)) {
        ThrowProcess("The Notion executable could not be read", GetLastError());
      }
      unsigned char wire[7] = {};
      DWORD wireRead = 0;
      if (!ReadFile(handle.get(), wire, sizeof(wire), &wireRead, nullptr) ||
          wireRead != sizeof(wire)) {
        throw std::runtime_error("Unsupported Electron fuse wire.");
      }
      if (wire[0] != 1 || wire[1] < 6) {
        throw std::runtime_error("Unsupported Electron fuse wire.");
      }
      if (wire[6] != '0') {
        throw std::runtime_error(
            "This app enforces embedded ASAR integrity. Native zoom will not "
            "weaken or bypass it.");
      }
      return;
    }
    offset += read;
    carry = text.size() > 48 ? text.substr(text.size() - 48) : text;
  }
  throw std::runtime_error("Electron fuse state could not be verified.");
}

bool LaunchProcess(const fs::path& exe, const std::wstring& arguments,
                   const fs::path& workingDirectory) {
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  if (!arguments.empty()) {
    command += L" ";
    command += arguments;
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWNORMAL;
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr,
                      workingDirectory.c_str(), &startup, &process)) {
    ThrowProcess("Notion could not be started", GetLastError());
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool SpawnDetached(const fs::path& exe, const std::wstring& arguments,
                   const fs::path& workingDirectory) {
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  if (!arguments.empty()) {
    command += L" ";
    command += arguments;
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                      workingDirectory.c_str(), &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

std::wstring ToLowerInvariant(std::wstring value) {
  if (value.empty()) {
    return value;
  }
  const int required = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
  if (required > 0) {
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr,
        nullptr, 0);
    if (written > 0) {
      result.resize(static_cast<std::size_t>(written));
      return result;
    }
  }
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  return value;
}

std::string MakeMutexName(std::string_view prefix, const fs::path& asarPath) {
  const std::wstring lower = ToLowerInvariant(asarPath.wstring());
  const std::string hash = HashString(ToUtf8(lower));
  return std::string(prefix) + "-" + hash;
}

MutexGuard::MutexGuard(std::string name, std::string busyMessage) {
  const std::wstring wideName = FromUtf8(name).wstring();
  handle_ = CreateMutexW(nullptr, FALSE, wideName.c_str());
  if (handle_ == nullptr) {
    throw std::runtime_error("The native zoom lock could not be created: " +
                             Win32Error(GetLastError()));
  }
  const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(handle_), 0);
  if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
    owned_ = true;
    return;
  }
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  if (wait == WAIT_TIMEOUT) {
    throw std::runtime_error(busyMessage);
  }
  throw std::runtime_error("The native zoom lock could not be acquired: " +
                           Win32Error(GetLastError()));
}

MutexGuard::~MutexGuard() {
  if (handle_ == nullptr) {
    return;
  }
  if (owned_) {
    ReleaseMutex(static_cast<HANDLE>(handle_));
  }
  CloseHandle(static_cast<HANDLE>(handle_));
}

}  // namespace notion_kex::detail
