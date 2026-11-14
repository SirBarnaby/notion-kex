#include "file_util.hpp"

#include "notion_kex/hash.hpp"
#include "path_util.hpp"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

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

std::string Win32Message(DWORD code) { return Win32Error(code); }

[[noreturn]] void ThrowIo(std::string_view label, DWORD code) {
  throw std::runtime_error(std::string(label) + ": " + Win32Message(code));
}

}  // namespace

std::vector<std::uint8_t> ReadFileBytes(const fs::path& path,
                                        std::uint64_t maxBytes,
                                        std::string_view label) {
  UniqueHandle handle(CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    ThrowIo(label, GetLastError());
  }

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(handle.get(), &size)) {
    ThrowIo(label, GetLastError());
  }
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
    throw std::runtime_error(std::string(label) +
                             " exceeds the supported size.");
  }

  std::vector<std::uint8_t> bytes(
      static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    if (!ReadFile(handle.get(), bytes.data() + offset, request, &read,
                  nullptr)) {
      ThrowIo(label, GetLastError());
    }
    if (read == 0) {
      throw std::runtime_error(std::string(label) +
                               " ended unexpectedly.");
    }
    offset += read;
  }
  return bytes;
}

std::string ReadFileText(const fs::path& path, std::uint64_t maxBytes,
                         std::string_view label) {
  const std::vector<std::uint8_t> bytes =
      ReadFileBytes(path, maxBytes, label);
  return std::string(reinterpret_cast<const char*>(bytes.data()),
                     bytes.size());
}

void WriteFileBytes(const fs::path& path,
                    const std::vector<std::uint8_t>& bytes) {
  UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    ThrowIo("The file could not be written", GetLastError());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    if (!WriteFile(handle.get(), bytes.data() + offset, request, &written,
                   nullptr)) {
      ThrowIo("The file could not be written", GetLastError());
    }
    if (written == 0) {
      throw std::runtime_error("The file could not be written.");
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle.get())) {
    ThrowIo("The file could not be flushed", GetLastError());
  }
}

void WriteFileText(const fs::path& path, std::string_view text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  WriteFileBytes(path, bytes);
}

void WriteJsonAtomic(const fs::path& path,
                     const nlohmann::ordered_json& value) {
  const std::string text = value.dump(4);
  const fs::path temporary =
      fs::path(path.wstring() + L"." + FromUtf8(NewGuidString()).wstring() +
               L".tmp");
  try {
    WriteFileText(temporary, text);
    if (FileExists(path)) {
      ReplaceFileWith(temporary, path);
    } else {
      MoveFileTo(temporary, path);
    }
  } catch (...) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    throw;
  }
}

void CopyFileTo(const fs::path& source, const fs::path& destination) {
  if (!::CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
    ThrowIo("The file could not be copied", GetLastError());
  }
}

void ReplaceFileWith(const fs::path& source, const fs::path& destination) {
  if (!::ReplaceFileW(destination.c_str(), source.c_str(), nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
    ThrowIo("The file could not be replaced", GetLastError());
  }
}

void MoveFileTo(const fs::path& source, const fs::path& destination) {
  if (!::MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ThrowIo("The file could not be moved", GetLastError());
  }
}

FileInfo GetFileInfo(const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return {};
    }
    ThrowIo("The file could not be inspected", code);
  }
  FileInfo info;
  info.exists = true;
  info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
              data.nFileSizeLow;
  info.writeTime = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime)
                    << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
  return info;
}

bool FileExists(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void CreateDirectories(const fs::path& path) {
  std::error_code error;
  fs::create_directories(path, error);
  if (error) {
    throw std::runtime_error("The directory could not be created: " +
                             error.message());
  }
}

void RemoveFile(const fs::path& path) {
  if (::DeleteFileW(path.c_str())) {
    return;
  }
  const DWORD code = GetLastError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return;
  }
  ThrowIo("The file could not be removed", code);
}

void RemoveTree(const fs::path& path) {
  std::error_code error;
  fs::remove_all(path, error);
  if (error) {
    throw std::runtime_error("The directory could not be removed: " +
                             error.message());
  }
}

std::string NewGuidString() {
  GUID guid = {};
  if (FAILED(CoCreateGuid(&guid))) {
    throw std::runtime_error("A unique identifier could not be created.");
  }
  wchar_t buffer[33] = {};
  swprintf(buffer, 33,
           L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned int>(guid.Data1),
           static_cast<unsigned int>(guid.Data2),
           static_cast<unsigned int>(guid.Data3),
           static_cast<unsigned int>(guid.Data4[0]),
           static_cast<unsigned int>(guid.Data4[1]),
           static_cast<unsigned int>(guid.Data4[2]),
           static_cast<unsigned int>(guid.Data4[3]),
           static_cast<unsigned int>(guid.Data4[4]),
           static_cast<unsigned int>(guid.Data4[5]),
           static_cast<unsigned int>(guid.Data4[6]),
           static_cast<unsigned int>(guid.Data4[7]));
  return ToUtf8(std::wstring(buffer));
}

std::string UtcNowIso() {
  FILETIME fileTime = {};
  GetSystemTimePreciseAsFileTime(&fileTime);
  SYSTEMTIME time = {};
  FileTimeToSystemTime(&fileTime, &time);
  const std::uint64_t ticks =
      (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) |
      fileTime.dwLowDateTime;
  const unsigned int fraction =
      static_cast<unsigned int>(ticks % 10000000ull);
  wchar_t buffer[40] = {};
  swprintf(buffer, 40,
           L"%04u-%02u-%02uT%02u:%02u:%02u.%07uZ",
           static_cast<unsigned int>(time.wYear),
           static_cast<unsigned int>(time.wMonth),
           static_cast<unsigned int>(time.wDay),
           static_cast<unsigned int>(time.wHour),
           static_cast<unsigned int>(time.wMinute),
           static_cast<unsigned int>(time.wSecond), fraction);
  return ToUtf8(std::wstring(buffer));
}

std::string LocalTimestamp() {
  SYSTEMTIME time = {};
  GetLocalTime(&time);
  wchar_t buffer[24] = {};
  swprintf(buffer, 24, L"%04u%02u%02u-%02u%02u%02u",
           static_cast<unsigned int>(time.wYear),
           static_cast<unsigned int>(time.wMonth),
           static_cast<unsigned int>(time.wDay),
           static_cast<unsigned int>(time.wHour),
           static_cast<unsigned int>(time.wMinute),
           static_cast<unsigned int>(time.wSecond));
  return ToUtf8(std::wstring(buffer));
}

bool IsValidUtf8(const std::vector<std::uint8_t>& bytes) {
  return IsValidUtf8(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool IsValidUtf8(std::string_view bytes) {
  if (bytes.empty()) {
    return true;
  }
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                             static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

std::string HashString(std::string_view bytes) {
  std::string hex;
  std::string error;
  if (!HashBytes(bytes.data(), bytes.size(), hex, error)) {
    throw std::runtime_error("SHA-256 failed: " + error);
  }
  return hex;
}

std::string HashBytesVector(const std::vector<std::uint8_t>& bytes) {
  std::string hex;
  std::string error;
  if (!HashBytes(bytes.data(), bytes.size(), hex, error)) {
    throw std::runtime_error("SHA-256 failed: " + error);
  }
  return hex;
}

}  // namespace notion_kex::detail
