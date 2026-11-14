#include "json_util.hpp"

#include "path_util.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace notion_kex::detail {
namespace {

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool CheckDepth(const std::string& text, int maxDepth, std::string& error) {
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char value : text) {
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (value == '\\') {
        escaped = true;
      } else if (value == '"') {
        inString = false;
      }
      continue;
    }
    if (value == '"') {
      inString = true;
    } else if (value == '{' || value == '[') {
      ++depth;
      if (depth > maxDepth) {
        error = "Native zoom metadata nesting exceeds the supported depth.";
        return false;
      }
    } else if ((value == '}' || value == ']') && depth > 0) {
      --depth;
    }
  }
  return true;
}

bool ReadBoundedBytes(const std::filesystem::path& path,
                      std::size_t maxBytes, std::string& out,
                      std::string& error) {
  const PathCheck check = CheckRegularPath(path, error);
  if (check == PathCheck::ReparsePoint || check == PathCheck::Error) {
    return false;
  }

  UniqueHandle handle(CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    error = "Native zoom metadata could not be opened: " +
            Win32Error(GetLastError());
    return false;
  }

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(handle.get(), &size)) {
    error = "Native zoom metadata size could not be read: " +
            Win32Error(GetLastError());
    return false;
  }
  if (size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
    error = "Native zoom metadata exceeds the supported size.";
    return false;
  }

  out.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < out.size()) {
    DWORD read = 0;
    const DWORD request =
        static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 20));
    if (!ReadFile(handle.get(), out.data() + offset, request, &read, nullptr)) {
      error = "Native zoom metadata could not be read: " +
              Win32Error(GetLastError());
      return false;
    }
    if (read == 0) {
      error = "Native zoom metadata ended unexpectedly.";
      return false;
    }
    offset += read;
  }
  return true;
}

}  // namespace

bool ReadBoundedJsonFile(const std::filesystem::path& path,
                         nlohmann::json& out, std::string& error) {
  std::string bytes;
  if (!ReadBoundedBytes(path, kMaxMetadataBytes, bytes, error)) {
    return false;
  }

  if (bytes.size() >= 3 &&
      static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.erase(0, 3);
  }

  if (!CheckDepth(bytes, kMaxJsonDepth, error)) {
    return false;
  }

  try {
    out = nlohmann::json::parse(bytes);
  } catch (const std::exception& exception) {
    error = std::string("Native zoom metadata is not valid JSON: ") +
            exception.what();
    return false;
  }
  return true;
}

}  // namespace notion_kex::detail
