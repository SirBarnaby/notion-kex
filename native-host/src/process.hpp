#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace notion_kex::detail {

namespace fs = std::filesystem;

struct ProcessEntry {
  unsigned long pid = 0;
  std::wstring name;
  std::wstring path;
};

std::vector<ProcessEntry> EnumerateProcesses();

// True when a process named Notion.exe is running from the requested path. A
// process whose path cannot be queried counts as running, matching the
// PowerShell launcher's `-not $_.Path` rule.
bool IsNotionRunning(const fs::path& notionExe);
bool IsAnyNotionRunning();
bool IsUpdaterRunning();

void AssertNotionClosed(const fs::path& notionExe,
                        std::string_view message);
void AssertElectronFuse(const fs::path& notionExe);

bool LaunchProcess(const fs::path& exe, const std::wstring& arguments,
                   const fs::path& workingDirectory);
bool SpawnDetached(const fs::path& exe, const std::wstring& arguments,
                   const fs::path& workingDirectory);

std::wstring ToLowerInvariant(std::wstring value);
std::string MakeMutexName(std::string_view prefix,
                          const fs::path& asarPath);

// Owns a named mutex for the lifetime of the guard. A second holder receives
// the same message the PowerShell implementation uses.
class MutexGuard {
 public:
  MutexGuard(std::string name, std::string busyMessage);
  ~MutexGuard();

  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;

 private:
  void* handle_ = nullptr;
  bool owned_ = false;
};

}  // namespace notion_kex::detail
