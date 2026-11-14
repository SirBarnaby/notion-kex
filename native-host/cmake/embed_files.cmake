# Generates a header with every embedded package file.
#
# Usage:
#   cmake -DROOT=<repository root> -DOUTPUT=<header> -P embed_files.cmake
#
# The embedded list is intentionally fixed. The native payload copy of
# native/pinch-main.js includes the update watcher hook; the legacy package
# copy and checksums.json are left untouched.

if(NOT ROOT OR NOT OUTPUT)
  message(FATAL_ERROR "ROOT and OUTPUT are required.")
endif()

set(relative_files
  extension/kex.css
  extension/kex.js
  extension/manifest.json
  extension/policy.js
  extension/settings.css
  extension/settings.html
  extension/settings.js
  native/pinch.js
  native/pinch-main.js)

file(WRITE "${OUTPUT}" "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace notion_kex::embedded::generated {\n\n")

set(index 0)
set(entries "")
foreach(relative IN LISTS relative_files)
  if(relative STREQUAL "native/pinch-main.js")
    set(source "${ROOT}/native-host/payloads/pinch-main.js")
  else()
    set(source "${ROOT}/${relative}")
  endif()
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Embedded source is missing: ${source}")
  endif()
  file(READ "${source}" hex HEX)
  if(hex STREQUAL "")
    file(APPEND "${OUTPUT}" "inline constexpr unsigned char kFile${index}[] = {0};\ninline constexpr std::size_t kFile${index}Size = 0;\n")
  else()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(APPEND "${OUTPUT}" "inline constexpr unsigned char kFile${index}[] = {${bytes}};\ninline constexpr std::size_t kFile${index}Size = sizeof(kFile${index});\n")
  endif()
  string(APPEND entries "  {\"${relative}\", kFile${index}, kFile${index}Size},\n")
  math(EXPR index "${index}+1")
endforeach()

file(APPEND "${OUTPUT}" "struct Entry {\n  const char* path;\n  const unsigned char* data;\n  std::size_t size;\n};\n\ninline constexpr Entry kEntries[] = {\n${entries}};\ninline constexpr std::size_t kEntryCount = ${index};\n\n}  // namespace notion_kex::embedded::generated\n")
