#include "notion_kex/asar.hpp"

#include "file_util.hpp"
#include "notion_kex/hash.hpp"
#include "path_util.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace notion_kex {
namespace {

constexpr std::uint64_t kMaxHeaderSize = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxIntegrityBlock = 16ull * 1024ull * 1024ull;

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

bool IsTruthy(const nlohmann::ordered_json& value) {
  if (value.is_null()) {
    return false;
  }
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number()) {
    return value.get<double>() != 0.0;
  }
  if (value.is_string()) {
    return !value.get<std::string>().empty();
  }
  if (value.is_array()) {
    return !value.empty();
  }
  return true;
}

bool IsAsciiHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

std::string NormalizeHash(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    if (character >= 'A' && character <= 'F') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return character;
  });
  return value;
}

bool ParseUnsigned(const nlohmann::ordered_json& value, std::uint64_t& out) {
  if (value.is_number_unsigned()) {
    out = value.get<std::uint64_t>();
    return true;
  }
  if (value.is_number_integer()) {
    const std::int64_t number = value.get<std::int64_t>();
    if (number < 0) {
      return false;
    }
    out = static_cast<std::uint64_t>(number);
    return true;
  }
  if (!value.is_string()) {
    return false;
  }
  std::string text = value.get<std::string>();
  const auto notSpace = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(), notSpace));
  text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(),
             text.end());
  if (!text.empty() && text.front() == '+') {
    text.erase(text.begin());
  }
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return false;
  }
  out = parsed;
  return true;
}

nlohmann::ordered_json* FindEntry(nlohmann::ordered_json& header,
                                  const std::vector<std::string>& parts) {
  nlohmann::ordered_json* current = &header;
  for (const std::string& part : parts) {
    if (!current->is_object()) {
      return nullptr;
    }
    const auto files = current->find("files");
    if (files == current->end() || !files->is_object()) {
      return nullptr;
    }
    const auto member = files->find(part);
    if (member == files->end()) {
      return nullptr;
    }
    current = &(*member);
  }
  return current;
}

const nlohmann::ordered_json* FindEntry(
    const nlohmann::ordered_json& header,
    const std::vector<std::string>& parts) {
  const nlohmann::ordered_json* current = &header;
  for (const std::string& part : parts) {
    if (!current->is_object()) {
      return nullptr;
    }
    const auto files = current->find("files");
    if (files == current->end() || !files->is_object()) {
      return nullptr;
    }
    const auto member = files->find(part);
    if (member == files->end()) {
      return nullptr;
    }
    current = &(*member);
  }
  return current;
}

void AddEntries(const std::vector<std::uint8_t>& bytes,
                const nlohmann::ordered_json& directory,
                const std::string& prefix, int depth,
                std::uint64_t headerSize, std::vector<AsarEntry>& entries) {
  if (depth > 64) {
    throw std::runtime_error("Unsupported ASAR directory tree.");
  }
  if (!directory.is_object()) {
    throw std::runtime_error("Unsupported ASAR directory tree.");
  }
  const auto files = directory.find("files");
  if (files == directory.end() || !files->is_object()) {
    throw std::runtime_error("Unsupported ASAR directory tree.");
  }

  for (auto iterator = files->begin(); iterator != files->end(); ++iterator) {
    const std::string& name = iterator.key();
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name == "." || name == "..") {
      throw std::runtime_error("Invalid ASAR member name.");
    }
    const std::string memberPath =
        prefix.empty() ? name : prefix + "/" + name;
    const nlohmann::ordered_json& member = iterator.value();
    if (!member.is_object()) {
      throw std::runtime_error("Unsupported ASAR directory tree.");
    }

    const auto childFiles = member.find("files");
    if (childFiles != member.end() && IsTruthy(*childFiles)) {
      AddEntries(bytes, member, memberPath, depth + 1, headerSize, entries);
      continue;
    }

    const auto unpacked = member.find("unpacked");
    const auto link = member.find("link");
    if ((unpacked != member.end() && IsTruthy(*unpacked)) ||
        (link != member.end() && IsTruthy(*link))) {
      continue;
    }

    const auto offsetValue = member.find("offset");
    const auto sizeValue = member.find("size");
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    if (offsetValue == member.end() ||
        !ParseUnsigned(*offsetValue, offset) ||
        sizeValue == member.end() || !ParseUnsigned(*sizeValue, size)) {
      throw std::runtime_error("Invalid ASAR member bounds: " + memberPath);
    }

    const std::uint64_t bodyOffset = 8 + headerSize;
    if (bodyOffset > bytes.size() || offset > bytes.size() - bodyOffset ||
        size > bytes.size() - bodyOffset - offset) {
      throw std::runtime_error("Invalid ASAR member bounds: " + memberPath);
    }

    AsarEntry entry;
    entry.path = memberPath;
    entry.offset = offset;
    entry.size = size;
    std::size_t start = 0;
    while (start <= memberPath.size()) {
      const std::size_t slash = memberPath.find('/', start);
      const std::size_t end =
          slash == std::string::npos ? memberPath.size() : slash;
      entry.parts.push_back(memberPath.substr(start, end - start));
      if (slash == std::string::npos) {
        break;
      }
      start = slash + 1;
    }
    entries.push_back(std::move(entry));
  }
}

const AsarEntry* FindSingleEntry(const AsarArchive& archive,
                                 const std::string& memberPath) {
  const AsarEntry* found = nullptr;
  for (const AsarEntry& entry : archive.entries) {
    if (entry.path == memberPath) {
      if (found != nullptr) {
        return nullptr;
      }
      found = &entry;
    }
  }
  return found;
}

std::string MemberHash(const std::vector<std::uint8_t>& bytes) {
  std::string hash;
  std::string error;
  if (!HashBytes(bytes.data(), bytes.size(), hash, error)) {
    throw std::runtime_error("SHA-256 failed: " + error);
  }
  return hash;
}

}  // namespace

AsarArchive ParseAsarFile(const std::filesystem::path& path) {
  std::string error;
  const detail::PathCheck check = detail::CheckRegularPath(path, error);
  if (check == detail::PathCheck::ReparsePoint ||
      check == detail::PathCheck::Error) {
    throw std::runtime_error(error);
  }
  const detail::FileInfo info = detail::GetFileInfo(path);
  if (!info.exists) {
    throw std::runtime_error("The app archive is missing.");
  }
  if (info.size > detail::kMaxAsarBytes) {
    throw std::runtime_error("ASAR exceeds the supported package size.");
  }
  return ParseAsarBytes(
      detail::ReadFileBytes(path, detail::kMaxAsarBytes, "The app archive"),
      detail::ToUtf8(path));
}

AsarArchive ParseAsarBytes(std::vector<std::uint8_t> bytes,
                           const std::string& label) {
  if (bytes.size() < 16 || ReadU32(bytes, 0) != 4) {
    throw std::runtime_error("Unsupported ASAR header.");
  }
  const std::uint64_t headerSize = ReadU32(bytes, 4);
  const std::uint64_t jsonSize = ReadU32(bytes, 12);
  if (headerSize < 8 || headerSize > kMaxHeaderSize ||
      jsonSize > headerSize - 8 || headerSize - jsonSize - 8 > 3 ||
      8 + headerSize > bytes.size()) {
    throw std::runtime_error("Invalid ASAR bounds.");
  }

  AsarArchive archive;
  archive.headerSize = headerSize;
  archive.bodyOffset = 8 + headerSize;
  try {
    archive.header = nlohmann::ordered_json::parse(
        bytes.begin() + 16, bytes.begin() + 16 + jsonSize);
  } catch (const std::exception& exception) {
    throw std::runtime_error(std::string("Invalid ASAR header JSON: ") +
                             exception.what());
  }
  if (!archive.header.is_object()) {
    throw std::runtime_error("Unsupported ASAR directory tree.");
  }

  AddEntries(bytes, archive.header, "", 0, headerSize, archive.entries);
  archive.bytes = std::move(bytes);

  std::vector<const AsarEntry*> ordered;
  ordered.reserve(archive.entries.size());
  for (const AsarEntry& entry : archive.entries) {
    ordered.push_back(&entry);
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const AsarEntry* left, const AsarEntry* right) {
                     return left->offset < right->offset;
                   });
  std::uint64_t end = 0;
  for (const AsarEntry* entry : ordered) {
    if (entry->size == 0) {
      continue;
    }
    if (entry->offset < end) {
      throw std::runtime_error("Overlapping ASAR members are unsupported.");
    }
    end = entry->offset + entry->size;
  }
  (void)label;
  return archive;
}

std::vector<std::uint8_t> GetAsarMember(const AsarArchive& archive,
                                        const std::string& memberPath) {
  const AsarEntry* entry = FindSingleEntry(archive, memberPath);
  if (entry == nullptr) {
    throw std::runtime_error(
        "Required packed ASAR member is unavailable: " + memberPath);
  }
  const std::uint64_t start = archive.bodyOffset + entry->offset;
  std::vector<std::uint8_t> data(
      archive.bytes.begin() + static_cast<std::ptrdiff_t>(start),
      archive.bytes.begin() +
          static_cast<std::ptrdiff_t>(start + entry->size));

  const nlohmann::ordered_json* object =
      FindEntry(archive.header, entry->parts);
  if (object == nullptr || !object->is_object()) {
    throw std::runtime_error(
        "Required packed ASAR member is unavailable: " + memberPath);
  }
  const auto integrity = object->find("integrity");
  if (integrity != object->end() && !integrity->is_null()) {
    if (!integrity->is_object()) {
      throw std::runtime_error("ASAR member integrity mismatch: " + memberPath);
    }
    const auto algorithm = integrity->find("algorithm");
    const auto hash = integrity->find("hash");
    if (algorithm == integrity->end() || !algorithm->is_string() ||
        hash == integrity->end() || !hash->is_string()) {
      throw std::runtime_error("ASAR member integrity mismatch: " + memberPath);
    }
    std::string expected = hash->get<std::string>();
    if (expected.size() != 64 ||
        !std::all_of(expected.begin(), expected.end(), IsAsciiHex)) {
      throw std::runtime_error("ASAR member integrity mismatch: " + memberPath);
    }
    const std::string algorithmName = algorithm->get<std::string>();
    std::string algorithmLower = algorithmName;
    std::transform(algorithmLower.begin(), algorithmLower.end(),
                   algorithmLower.begin(), [](char character) {
                     return static_cast<char>(std::tolower(
                         static_cast<unsigned char>(character)));
                   });
    if (algorithmLower != "sha256" ||
        NormalizeHash(expected) != MemberHash(data)) {
      throw std::runtime_error("ASAR member integrity mismatch: " + memberPath);
    }
  }
  return data;
}

std::vector<std::uint8_t> SetAsarMember(
    const AsarArchive& archive, const std::string& memberPath,
    const std::vector<std::uint8_t>& replacement) {
  const AsarEntry* entry = FindSingleEntry(archive, memberPath);
  if (entry == nullptr) {
    throw std::runtime_error("Unexpected ASAR replacement target.");
  }
  if (entry->size > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()) ||
      replacement.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error("ASAR member is too large.");
  }
  const std::int64_t delta =
      static_cast<std::int64_t>(replacement.size()) -
      static_cast<std::int64_t>(entry->size);

  nlohmann::ordered_json header = archive.header;
  for (const AsarEntry& other : archive.entries) {
    if (other.offset <= entry->offset) {
      continue;
    }
    nlohmann::ordered_json* object = FindEntry(header, other.parts);
    if (object == nullptr || !object->is_object()) {
      throw std::runtime_error("Unexpected ASAR replacement target.");
    }
    if (delta < 0 &&
        other.offset < static_cast<std::uint64_t>(-delta)) {
      throw std::runtime_error("ASAR member offset became negative.");
    }
    const std::uint64_t moved =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(other.offset) +
                                   delta);
    (*object)["offset"] = std::to_string(moved);
  }

  nlohmann::ordered_json* target = FindEntry(header, entry->parts);
  if (target == nullptr || !target->is_object()) {
    throw std::runtime_error("Unexpected ASAR replacement target.");
  }
  (*target)["size"] = static_cast<std::uint64_t>(replacement.size());
  const auto integrity = target->find("integrity");
  if (integrity != target->end() && !integrity->is_null()) {
    if (!integrity->is_object()) {
      throw std::runtime_error("Unsupported ASAR integrity block size.");
    }
    const auto blockValue = integrity->find("blockSize");
    std::uint64_t blockSize = 0;
    if (blockValue == integrity->end() ||
        !ParseUnsigned(*blockValue, blockSize) || blockSize < 1 ||
        blockSize > kMaxIntegrityBlock) {
      throw std::runtime_error("Unsupported ASAR integrity block size.");
    }
    nlohmann::ordered_json blocks = nlohmann::ordered_json::array();
    for (std::size_t offset = 0; offset < replacement.size();
         offset += static_cast<std::size_t>(blockSize)) {
      const std::size_t length = std::min<std::size_t>(
          static_cast<std::size_t>(blockSize), replacement.size() - offset);
      std::string hash;
      std::string error;
      if (!HashBytes(replacement.data() + offset, length, hash, error)) {
        throw std::runtime_error("SHA-256 failed: " + error);
      }
      blocks.push_back(hash);
    }
    (*integrity)["hash"] = MemberHash(replacement);
    (*integrity)["blocks"] = std::move(blocks);
  }

  const std::string json = header.dump();
  const std::uint64_t padded =
      ((static_cast<std::uint64_t>(json.size()) + 3) / 4) * 4;
  if (padded > kMaxHeaderSize - 8) {
    throw std::runtime_error("ASAR header exceeds the supported size.");
  }

  std::vector<std::uint8_t> output;
  output.reserve(16 + static_cast<std::size_t>(padded) +
                 static_cast<std::size_t>(entry->offset) +
                 replacement.size() +
                 (archive.bytes.size() -
                  static_cast<std::size_t>(archive.bodyOffset + entry->offset +
                                           entry->size)));
  AppendU32(output, 4);
  AppendU32(output, static_cast<std::uint32_t>(8 + padded));
  AppendU32(output, static_cast<std::uint32_t>(4 + padded));
  AppendU32(output, static_cast<std::uint32_t>(json.size()));
  output.insert(output.end(), json.begin(), json.end());
  output.resize(16 + static_cast<std::size_t>(padded), 0);

  const std::uint64_t prefixStart = archive.bodyOffset;
  const std::uint64_t prefixEnd = archive.bodyOffset + entry->offset;
  output.insert(output.end(),
                archive.bytes.begin() +
                    static_cast<std::ptrdiff_t>(prefixStart),
                archive.bytes.begin() +
                    static_cast<std::ptrdiff_t>(prefixEnd));
  output.insert(output.end(), replacement.begin(), replacement.end());
  const std::uint64_t tail =
      archive.bodyOffset + entry->offset + entry->size;
  output.insert(output.end(),
                archive.bytes.begin() + static_cast<std::ptrdiff_t>(tail),
                archive.bytes.end());
  return output;
}

void AssertAsarEdits(const AsarArchive& before, const AsarArchive& after,
                     const std::vector<std::string>& memberPaths) {
  if (before.entries.size() != after.entries.size()) {
    throw std::runtime_error("ASAR inventory changed.");
  }
  for (const AsarEntry& entry : before.entries) {
    if (std::find(memberPaths.begin(), memberPaths.end(), entry.path) !=
        memberPaths.end()) {
      continue;
    }
    const std::vector<std::uint8_t> first = GetAsarMember(before, entry.path);
    const std::vector<std::uint8_t> second = GetAsarMember(after, entry.path);
    if (MemberHash(first) != MemberHash(second)) {
      throw std::runtime_error("Unrelated ASAR member changed: " + entry.path);
    }
  }
}

std::string AsarMemberHash(const AsarArchive& archive,
                           const std::string& memberPath) {
  return MemberHash(GetAsarMember(archive, memberPath));
}

}  // namespace notion_kex
