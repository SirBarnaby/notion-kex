#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace notion_kex {

struct AsarEntry {
  std::string path;
  std::vector<std::string> parts;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct AsarArchive {
  std::vector<std::uint8_t> bytes;
  nlohmann::ordered_json header;
  std::uint64_t headerSize = 0;
  std::uint64_t bodyOffset = 0;
  std::vector<AsarEntry> entries;
};

AsarArchive ParseAsarFile(const std::filesystem::path& path);
AsarArchive ParseAsarBytes(std::vector<std::uint8_t> bytes,
                           const std::string& label);

std::vector<std::uint8_t> GetAsarMember(const AsarArchive& archive,
                                        const std::string& memberPath);
std::vector<std::uint8_t> SetAsarMember(
    const AsarArchive& archive, const std::string& memberPath,
    const std::vector<std::uint8_t>& replacement);
void AssertAsarEdits(const AsarArchive& before, const AsarArchive& after,
                     const std::vector<std::string>& memberPaths);
std::string AsarMemberHash(const AsarArchive& archive,
                           const std::string& memberPath);

}  // namespace notion_kex
