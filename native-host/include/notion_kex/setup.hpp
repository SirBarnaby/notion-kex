#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace notion_kex {

struct SetupOutcome {
  bool changed = false;
  std::string message;
};

struct ExtensionFileStatus {
  std::string name;
  bool present = false;
  bool hashMatches = false;
  std::string expectedHash;
};

struct ExtensionStatus {
  bool installed = false;
  bool healthy = false;
  bool nativeInstalled = false;
  std::string version;
  std::string installRoot;
  std::string notionVersion;
  std::string nativeMessage;
  std::vector<ExtensionFileStatus> files;
  std::vector<std::string> problems;
};

// Installs the embedded extension, updates Notion's stored preferences and
// then applies the native zoom patch. Every check runs before the first
// profile write, so an unsupported app or an integrity fuse leaves the profile
// untouched.
SetupOutcome InstallExtension(const std::filesystem::path& notionExe,
                              const std::filesystem::path& notionData);

SetupOutcome UninstallExtension(const std::filesystem::path& notionExe,
                                const std::filesystem::path& notionData);

// Passive inspection. It never recovers a journal, writes a file or launches
// anything.
ExtensionStatus InspectExtension(const std::filesystem::path& notionExe,
                                 const std::filesystem::path& notionData);

}  // namespace notion_kex
