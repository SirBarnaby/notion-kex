#pragma once

#include <filesystem>
#include <string>

namespace notion_kex::detail {

enum class PathCheck {
  Ok,
  ReparsePoint,
  Error,
};

// Rejects symbolic links, junctions and other reparse points in any existing
// component of the path. Missing components are allowed.
PathCheck CheckRegularPath(const std::filesystem::path& path,
                           std::string& error);

bool IsUnderDirectory(const std::filesystem::path& candidate,
                      const std::filesystem::path& directory);

bool EqualsPath(const std::filesystem::path& left,
                const std::filesystem::path& right);

std::string ToUtf8(const std::filesystem::path& path);
std::string ToUtf8(const std::wstring& value);
std::filesystem::path FromUtf8(const std::string& value);

std::string Win32Error(unsigned long code);

bool IsBackupLeafName(const std::filesystem::path& path);
bool IsRollbackLeafName(const std::filesystem::path& path);

}  // namespace notion_kex::detail
