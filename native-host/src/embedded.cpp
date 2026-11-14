#include "notion_kex/embedded.hpp"

#include "file_util.hpp"
#include "notion_kex/hash.hpp"

#include <stdexcept>
#include <vector>

#include "embedded_files.hpp"

namespace notion_kex::embedded {
namespace {

std::vector<File> BuildFiles() {
  std::vector<File> files;
  files.reserve(generated::kEntryCount);
  for (std::size_t index = 0; index < generated::kEntryCount; ++index) {
    const generated::Entry& entry = generated::kEntries[index];
    files.push_back(File{entry.path, entry.data, entry.size});
  }
  return files;
}

std::string HashBytes(const std::uint8_t* data, std::size_t size) {
  std::string hex;
  std::string error;
  if (!notion_kex::HashBytes(data, size, hex, error)) {
    throw std::runtime_error("SHA-256 failed: " + error);
  }
  return hex;
}

}  // namespace

std::span<const File> Files() {
  static const std::vector<File> files = BuildFiles();
  return files;
}

const File* Find(std::string_view path) {
  for (const File& file : Files()) {
    if (file.path == path) {
      return &file;
    }
  }
  return nullptr;
}

std::string PayloadHash() {
  static const std::string hash = [] {
    const File* preload = Find("native/pinch.js");
    const File* main = Find("native/pinch-main.js");
    if (preload == nullptr || main == nullptr) {
      throw std::runtime_error("The embedded native payload is incomplete.");
    }
    const std::string joined =
        HashBytes(preload->data, preload->size) + "\n" +
        HashBytes(main->data, main->size);
    return detail::HashString(joined);
  }();
  return hash;
}

std::string ExtensionManifestJson() {
  const File* manifest = Find("extension/manifest.json");
  if (manifest == nullptr) {
    throw std::runtime_error("The embedded extension manifest is missing.");
  }
  return std::string(reinterpret_cast<const char*>(manifest->data),
                     manifest->size);
}

}  // namespace notion_kex::embedded
