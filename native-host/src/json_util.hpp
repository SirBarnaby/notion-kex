#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace notion_kex::detail {

constexpr std::size_t kMaxMetadataBytes = 1024 * 1024;
constexpr int kMaxJsonDepth = 64;

// Reads a metadata file with the same 1 MiB bound as tools/NativeZoom.ps1.
// The path must not contain reparse points. Returns false with a stable error.
bool ReadBoundedJsonFile(const std::filesystem::path& path,
                         nlohmann::json& out, std::string& error);

}  // namespace notion_kex::detail
