#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace notion_kex::embedded {

struct File {
  std::string_view path;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

std::span<const File> Files();
const File* Find(std::string_view path);

// SHA-256 of the two native payload hashes joined by a single LF, matching
// tools/NativeZoom.ps1. The preload payload is hashed first.
std::string PayloadHash();

std::string ExtensionManifestJson();

}  // namespace notion_kex::embedded
