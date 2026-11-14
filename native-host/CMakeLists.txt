cmake_minimum_required(VERSION 3.21)

project(
  notion_kex_native_host
  VERSION 1.0.0
  DESCRIPTION "Native Notion KEX host for Windows"
  LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "notion-kex native-host is a Windows-only component.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(kex_configure_target target)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(
    ${target}
    PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MSVC)
    target_compile_options(
      ${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# The passive status library is retained unchanged so the original status
# fixture suite keeps its exact build and evidence.
add_library(
  kex_status
  STATIC
  src/hash.cpp
  src/json_util.cpp
  src/path_util.cpp
  src/record.cpp
  src/sha256.cpp
  src/status.cpp)

target_include_directories(
  kex_status
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(kex_status PUBLIC bcrypt)
kex_configure_target(kex_status)

# Embed the extension, the preload payload and the native main-process payload
# into the executable. The native main payload lives under native-host and
# adds the update watcher hook; native/pinch-main.js and checksums.json remain
# untouched for the legacy PowerShell installer.
set(KEX_EMBEDDED_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_files.hpp")
set(KEX_EMBEDDED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/kex.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/kex.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/policy.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/settings.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/settings.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extension/settings.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/pinch.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/payloads/pinch-main.js")

add_custom_command(
  OUTPUT "${KEX_EMBEDDED_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND
    "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
    "-DOUTPUT=${KEX_EMBEDDED_HEADER}" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
  DEPENDS ${KEX_EMBEDDED_SOURCES}
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
  COMMENT "Embedding Notion KEX extension and payload files"
  VERBATIM)

add_custom_target(kex_embedded DEPENDS "${KEX_EMBEDDED_HEADER}")

add_library(
  kex_core
  STATIC
  src/asar.cpp
  src/embedded.cpp
  src/file_util.cpp
  src/launcher.cpp
  src/native_zoom.cpp
  src/process.cpp
  src/setup.cpp
  src/shortcuts.cpp)

target_include_directories(
  kex_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
          "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(
  kex_core
  PUBLIC kex_status
  PRIVATE ole32 oleaut32 uuid shell32 propsys version)
add_dependencies(kex_core kex_embedded)
kex_configure_target(kex_core)

add_executable(kex-status src/cli.cpp)
target_link_libraries(kex-status PRIVATE kex_status)
target_include_directories(
  kex-status
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_definitions(
  kex-status PRIVATE KEX_STATUS_VERSION="${PROJECT_VERSION}")
kex_configure_target(kex-status)

add_executable(notion-kex WIN32 src/notion_kex_cli.cpp)
set_target_properties(
  notion-kex
  PROPERTIES OUTPUT_NAME "Notion KEX"
             LINK_FLAGS "/ENTRY:wmainCRTStartup")
target_link_libraries(notion-kex PRIVATE kex_core user32)
target_include_directories(
  notion-kex
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_definitions(notion-kex PRIVATE KEX_VERSION="${PROJECT_VERSION}")
kex_configure_target(notion-kex)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    kex-status-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/native-host-status.cpp")
  target_link_libraries(kex-status-tests PRIVATE kex_status)
  target_include_directories(
    kex-status-tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_compile_definitions(
    kex-status-tests
    PRIVATE
      KEX_STATUS_VERSION="${PROJECT_VERSION}"
      KEX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
      KEX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
      KEX_BUILD_TYPE="$<CONFIG>")
  kex_configure_target(kex-status-tests)

  add_test(NAME native-host-status COMMAND kex-status-tests)
  set_tests_properties(native-host-status PROPERTIES TIMEOUT 180)

  add_executable(
    kex-native-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/native-host-full.cpp")
  target_link_libraries(kex-native-tests PRIVATE kex_core)
  target_include_directories(
    kex-native-tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_compile_definitions(
    kex-native-tests
    PRIVATE
      KEX_VERSION="${PROJECT_VERSION}"
      KEX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
      KEX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
      KEX_BUILD_TYPE="$<CONFIG>"
      KEX_CLI_PATH="$<TARGET_FILE:notion-kex>")
  target_link_libraries(
    kex-native-tests PRIVATE ole32 oleaut32 uuid shell32 propsys user32)
  add_dependencies(kex-native-tests notion-kex)
  kex_configure_target(kex-native-tests)

  add_test(NAME native-host-full COMMAND kex-native-tests)
  set_tests_properties(native-host-full PROPERTIES TIMEOUT 600)
endif()
