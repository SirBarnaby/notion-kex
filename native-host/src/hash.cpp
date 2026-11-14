#include "notion_kex/hash.hpp"

#include "sha256.hpp"

namespace notion_kex {

FileHashState HashFile(const std::filesystem::path& path, std::string& hex,
                       std::string& error) {
  switch (detail::HashFileIfPresent(path, hex, error)) {
    case detail::HashFileState::Missing:
      return FileHashState::Missing;
    case detail::HashFileState::Hashed:
      return FileHashState::Hashed;
    case detail::HashFileState::Error:
      return FileHashState::Error;
  }
  return FileHashState::Error;
}

bool HashBytes(const void* data, std::size_t size, std::string& hex,
               std::string& error) {
  return detail::HashBytes(data, size, hex, error);
}

}  // namespace notion_kex
