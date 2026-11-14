#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace notion_kex {

enum class FileHashState {
  Missing,
  Hashed,
  Error,
};

// Hashes a file with SHA-256. A missing file is reported distinctly so callers
// can model a missing app archive without treating it as a hard error.
FileHashState HashFile(const std::filesystem::path& path, std::string& hex,
                       std::string& error);

bool HashBytes(const void* data, std::size_t size, std::string& hex,
               std::string& error);

}  // namespace notion_kex
