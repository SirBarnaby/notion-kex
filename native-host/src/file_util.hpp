#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace notion_kex::detail {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaxAsarBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxMetadataBytes = 1024ull * 1024ull;
constexpr std::uint64_t kMaxStateBytes = 64ull * 1024ull * 1024ull;

struct FileInfo {
  bool exists = false;
  std::uint64_t size = 0;
  std::uint64_t writeTime = 0;
};

// Reads a whole file with a hard upper bound. Missing files and oversized files
// are reported as exceptions with stable messages.
std::vector<std::uint8_t> ReadFileBytes(const fs::path& path,
                                        std::uint64_t maxBytes,
                                        std::string_view label);
std::string ReadFileText(const fs::path& path, std::uint64_t maxBytes,
                         std::string_view label);

void WriteFileBytes(const fs::path& path,
                    const std::vector<std::uint8_t>& bytes);
void WriteFileText(const fs::path& path, std::string_view text);

// Writes JSON through a sibling temporary file. Existing destinations are
// replaced atomically; new destinations are moved into place.
void WriteJsonAtomic(const fs::path& path, const nlohmann::ordered_json& value);

void CopyFileTo(const fs::path& source, const fs::path& destination);
void ReplaceFileWith(const fs::path& source, const fs::path& destination);
void MoveFileTo(const fs::path& source, const fs::path& destination);

FileInfo GetFileInfo(const fs::path& path);
bool FileExists(const fs::path& path);
bool DirectoryExists(const fs::path& path);
void CreateDirectories(const fs::path& path);
void RemoveFile(const fs::path& path);
void RemoveTree(const fs::path& path);

std::string NewGuidString();
std::string UtcNowIso();
std::string LocalTimestamp();

bool IsValidUtf8(const std::vector<std::uint8_t>& bytes);
bool IsValidUtf8(std::string_view bytes);

std::string HashString(std::string_view bytes);
std::string HashBytesVector(const std::vector<std::uint8_t>& bytes);

}  // namespace notion_kex::detail
