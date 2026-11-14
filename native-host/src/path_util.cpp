#include "path_util.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace notion_kex::detail {
namespace {

bool IsHexChar(wchar_t value) {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

std::wstring Trim(std::wstring value) {
  const auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
              value.end());
  return value;
}

bool HasNumericToken(const std::wstring& name, const std::wstring& prefix,
                     const std::wstring& suffix) {
  if (name.size() != prefix.size() + 32 + suffix.size()) {
    return false;
  }
  if (name.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  for (std::size_t index = prefix.size(); index < prefix.size() + 32;
       ++index) {
    if (!IsHexChar(name[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string ToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::string ToUtf8(const std::filesystem::path& path) {
  return ToUtf8(path.native());
}

std::filesystem::path FromUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return std::filesystem::path(result);
}

std::string Win32Error(unsigned long code) {
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(code), 0,
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring message =
      (length != 0 && buffer != nullptr)
          ? std::wstring(buffer, static_cast<std::size_t>(length))
          : L"Windows error " + std::to_wstring(code);
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return ToUtf8(Trim(std::move(message)));
}

PathCheck CheckRegularPath(const std::filesystem::path& path,
                           std::string& error) {
  std::error_code code;
  std::filesystem::path full = std::filesystem::absolute(path, code);
  if (code) {
    error = "The path could not be resolved: " + code.message();
    return PathCheck::Error;
  }
  full = full.lexically_normal();

  const auto check = [&error](const std::filesystem::path& candidate) {
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD lastError = GetLastError();
      if (lastError == ERROR_FILE_NOT_FOUND ||
          lastError == ERROR_PATH_NOT_FOUND) {
        return PathCheck::Ok;
      }
      error = "The path could not be inspected: " + Win32Error(lastError);
      return PathCheck::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      error = "Native zoom paths must not contain symbolic links or junctions.";
      return PathCheck::ReparsePoint;
    }
    return PathCheck::Ok;
  };

  std::filesystem::path current = full.root_path();
  if (!current.empty()) {
    const PathCheck result = check(current);
    if (result != PathCheck::Ok) {
      return result;
    }
  }
  for (const auto& part : full) {
    if (part.empty() || part == full.root_name() ||
        part == full.root_directory()) {
      continue;
    }
    current /= part;
    const PathCheck result = check(current);
    if (result != PathCheck::Ok) {
      return result;
    }
  }
  return PathCheck::Ok;
}

bool IsUnderDirectory(const std::filesystem::path& candidate,
                      const std::filesystem::path& directory) {
  std::error_code leftError;
  std::error_code rightError;
  const std::filesystem::path left =
      std::filesystem::absolute(candidate, leftError).lexically_normal();
  const std::filesystem::path right =
      std::filesystem::absolute(directory, rightError).lexically_normal();
  if (leftError || rightError) {
    return false;
  }

  std::wstring leftText = left.native();
  std::wstring rightText = right.native();
  while (!rightText.empty() &&
         (rightText.back() == L'\\' || rightText.back() == L'/')) {
    rightText.pop_back();
  }
  if (rightText.empty() || leftText.size() <= rightText.size()) {
    return false;
  }
  if (CompareStringOrdinal(leftText.c_str(),
                           static_cast<int>(rightText.size()),
                           rightText.c_str(),
                           static_cast<int>(rightText.size()), TRUE) !=
      CSTR_EQUAL) {
    return false;
  }
  return leftText[rightText.size()] == L'\\' ||
         leftText[rightText.size()] == L'/';
}

bool EqualsPath(const std::filesystem::path& left,
                const std::filesystem::path& right) {
  std::error_code leftError;
  std::error_code rightError;
  const std::filesystem::path leftFull =
      std::filesystem::absolute(left, leftError).lexically_normal();
  const std::filesystem::path rightFull =
      std::filesystem::absolute(right, rightError).lexically_normal();
  if (leftError || rightError) {
    return false;
  }
  const std::wstring leftText = leftFull.native();
  const std::wstring rightText = rightFull.native();
  return CompareStringOrdinal(leftText.c_str(),
                              static_cast<int>(leftText.size()),
                              rightText.c_str(),
                              static_cast<int>(rightText.size()), TRUE) ==
         CSTR_EQUAL;
}

bool IsBackupLeafName(const std::filesystem::path& path) {
  return HasNumericToken(path.filename().native(),
                         L"app-asar-before-native-", L".bak");
}

bool IsRollbackLeafName(const std::filesystem::path& path) {
  return HasNumericToken(path.filename().native(), L"native-rollback-",
                         L".bak");
}

}  // namespace notion_kex::detail
