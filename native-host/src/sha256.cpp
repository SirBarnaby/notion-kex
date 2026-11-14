#include "sha256.hpp"

#include "path_util.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace notion_kex::detail {
namespace {

constexpr std::size_t kChunkSize = 1024 * 1024;
constexpr unsigned long long kMaxArchiveBytes = 512ull * 1024 * 1024;

std::string NtStatusText(NTSTATUS status) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(status));
  return buffer;
}

std::string ToHex(const std::vector<unsigned char>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '\0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2] = kDigits[bytes[index] >> 4];
    result[index * 2 + 1] = kDigits[bytes[index] & 0x0F];
  }
  return result;
}

class Sha256State {
 public:
  Sha256State() = default;
  Sha256State(const Sha256State&) = delete;
  Sha256State& operator=(const Sha256State&) = delete;
  ~Sha256State() { Reset(); }

  bool Init(std::string& error) {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
      error = "BCryptOpenAlgorithmProvider failed: " + NtStatusText(status);
      return false;
    }

    DWORD objectSize = 0;
    DWORD result = 0;
    status = BCryptGetProperty(
        algorithm_, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &result, 0);
    if (!BCRYPT_SUCCESS(status)) {
      error = "BCryptGetProperty(object) failed: " + NtStatusText(status);
      return false;
    }

    DWORD digestSize = 0;
    status = BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&digestSize),
                               sizeof(digestSize), &result, 0);
    if (!BCRYPT_SUCCESS(status)) {
      error = "BCryptGetProperty(hash) failed: " + NtStatusText(status);
      return false;
    }

    object_.resize(objectSize);
    digest_.resize(digestSize);
    digestSize_ = digestSize;
    status = BCryptCreateHash(algorithm_, &hash_, object_.data(),
                              static_cast<ULONG>(object_.size()), nullptr, 0,
                              0);
    if (!BCRYPT_SUCCESS(status)) {
      error = "BCryptCreateHash failed: " + NtStatusText(status);
      return false;
    }
    return true;
  }

  bool Update(const void* data, std::size_t size, std::string& error) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
      const std::size_t chunk = std::min(kChunkSize, size - offset);
      const NTSTATUS status =
          BCryptHashData(hash_,
                         const_cast<PUCHAR>(bytes + offset),
                         static_cast<ULONG>(chunk), 0);
      if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptHashData failed: " + NtStatusText(status);
        return false;
      }
      offset += chunk;
    }
    return true;
  }

  bool Finish(std::string& hex, std::string& error) {
    const NTSTATUS status =
        BCryptFinishHash(hash_, digest_.data(), digestSize_, 0);
    if (!BCRYPT_SUCCESS(status)) {
      error = "BCryptFinishHash failed: " + NtStatusText(status);
      return false;
    }
    hex = ToHex(digest_);
    Reset();
    return true;
  }

 private:
  void Reset() {
    if (hash_ != nullptr) {
      BCryptDestroyHash(hash_);
      hash_ = nullptr;
    }
    if (algorithm_ != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
    }
    object_.clear();
    digest_.clear();
    digestSize_ = 0;
  }

  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<unsigned char> object_;
  std::vector<unsigned char> digest_;
  DWORD digestSize_ = 0;
};

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

}  // namespace

HashFileState HashFileIfPresent(const std::filesystem::path& path,
                                std::string& hex, std::string& error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND ||
        lastError == ERROR_PATH_NOT_FOUND) {
      return HashFileState::Missing;
    }
    error = "The app archive could not be inspected: " +
            Win32Error(lastError);
    return HashFileState::Error;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    error = "The app archive path is a directory.";
    return HashFileState::Error;
  }

  UniqueHandle handle(CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    error = "The app archive could not be opened: " + Win32Error(GetLastError());
    return HashFileState::Error;
  }

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(handle.get(), &size)) {
    error = "The app archive size could not be read: " +
            Win32Error(GetLastError());
    return HashFileState::Error;
  }
  if (size.QuadPart > static_cast<LONGLONG>(kMaxArchiveBytes)) {
    error = "ASAR exceeds the supported package size.";
    return HashFileState::Error;
  }

  Sha256State state;
  if (!state.Init(error)) {
    return HashFileState::Error;
  }

  std::vector<unsigned char> buffer(kChunkSize);
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(handle.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      error = "The app archive could not be read: " + Win32Error(GetLastError());
      return HashFileState::Error;
    }
    if (read == 0) {
      break;
    }
    if (!state.Update(buffer.data(), read, error)) {
      return HashFileState::Error;
    }
  }

  if (!state.Finish(hex, error)) {
    return HashFileState::Error;
  }
  return HashFileState::Hashed;
}

bool HashBytes(const void* data, std::size_t size, std::string& hex,
               std::string& error) {
  Sha256State state;
  if (!state.Init(error)) {
    return false;
  }
  if (size != 0 && !state.Update(data, size, error)) {
    return false;
  }
  return state.Finish(hex, error);
}

}  // namespace notion_kex::detail
