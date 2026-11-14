#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace notion_kex::detail {

enum class HashFileState {
  Missing,
  Hashed,
  Error,
};

HashFileState HashFileIfPresent(const std::filesystem::path& path,
                                std::string& hex, std::string& error);
bool HashBytes(const void* data, std::size_t size, std::string& hex,
               std::string& error);

}  // namespace notion_kex::detail
