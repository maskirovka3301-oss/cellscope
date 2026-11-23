cmake_minimum_required(VERSION 3.20)

project(CellScope LANGUAGES C)

if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(CELLSCOPE_BUILD_TESTS "Build the unit tests" ON)
option(CELLSCOPE_WIN_CONSOLE "Keep a console window on Windows for diagnostics" ON)
set(CELLSCOPE_CODESIGN_IDENTITY "-" CACHE STRING
    "macOS code-signing identity; '-' means ad-hoc")

find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
  add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
endif()

find_package(Freetype REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
endif()
if(NOT SODIUM_FOUND)
  find_path(SODIUM_INCLUDE_DIR NAMES sodium.h)
  find_library(SODIUM_LIBRARY NAMES sodium libsodium)
  if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR "libsodium was not found (install libsodium-dev or use vcpkg)")
  endif()
  add_library(PkgConfig::SODIUM UNKNOWN IMPORTED)
  set_target_properties(PkgConfig::SODIUM PROPERTIES
    IMPORTED_LOCATION "${SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}")
endif()

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE CELLSCOPE_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CELLSCOPE_GIT_COMMIT)
  set(CELLSCOPE_GIT_COMMIT "unknown")
endif()

add_library(cellcore STATIC
  src/util.c
  src/cellular.c
  src/telephony.c
  src/meter.c
  src/quality.c
  src/wifi.c
)
target_include_directories(cellcore PUBLIC src)
target_compile_definitions(cellcore PUBLIC
  _CRT_SECURE_NO_WARNINGS
  $<$<CONFIG:Release>:NDEBUG>
)
if(MSVC)
  target_compile_options(cellcore PRIVATE /W3)
else()
  target_compile_options(cellcore PRIVATE -Wall -Wextra -Wpedantic)
endif()
# glibc keeps sincosf/sqrtf/floorf in libm; propagate to the app and tests.
if(UNIX AND NOT APPLE)
  target_link_libraries(cellcore PUBLIC m)
endif()

set(CELLSCOPE_SOURCES
  src/main.c
  src/app.c
  src/app_ui.c
  src/symptom_ui.c
  src/anatomy_assets.c
  src/train.c
  src/ui.c
  src/gpu_renderer.c
  src/anatomy_3d.c
  src/muscle_codes.c
  src/symptom_catalog.c
  src/fonts.c
  src/proc.c
  src/runtime.c
  src/settings.c
  src/hfmodel.c
  src/server.c
  src/adb.c
  src/camera.c
  src/vlm.c
  src/stb_impl.c
  src/cloud.c
)

add_executable(cellscope ${CELLSCOPE_SOURCES})
if(APPLE)
  target_sources(cellscope PRIVATE src/camera_mac.m)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  find_library(OBJC_LIBRARY objc REQUIRED)
  target_link_libraries(cellscope PRIVATE ${AVFOUNDATION_FRAMEWORK} ${FOUNDATION_FRAMEWORK}
                        ${APPKIT_FRAMEWORK} ${OBJC_LIBRARY})
else()
  target_sources(cellscope PRIVATE src/camera_platform.c)
endif()
target_link_libraries(cellscope PRIVATE
  cellcore
  SDL3::SDL3
  Freetype::Freetype
  CURL::libcurl
  PkgConfig::SODIUM
  Threads::Threads
)
if(APPLE)
  target_link_libraries(cellscope PRIVATE "-framework Security")
elseif(WIN32)
  target_link_libraries(cellscope PRIVATE crypt32)
endif()
target_include_directories(cellscope PRIVATE
  src
  third_party
)
target_compile_definitions(cellscope PRIVATE
  CELLSCOPE_VERSION="development"
  CELLSCOPE_GIT_COMMIT="${CELLSCOPE_GIT_COMMIT}"
  CELLSCOPE_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  CELLSCOPE_SOURCE_SCRIPTS="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
  CELLSCOPE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
  $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)
if(APPLE)
  set_target_properties(cellscope PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "CellScope"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.cellscope.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0"
  )
endif()
if(WIN32 AND NOT CELLSCOPE_WIN_CONSOLE)
  set_target_properties(cellscope PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(MSVC)
  target_compile_options(cellscope PRIVATE /W3)
else()
  target_compile_options(cellscope PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_source_files_properties(src/stb_impl.c PROPERTIES COMPILE_OPTIONS "")

# Assets live next to the executable during development and inside the
# macOS bundle's Resources directory for distribution.
add_custom_command(TARGET cellscope POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:cellscope>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
          "$<TARGET_FILE_DIR:cellscope>/scripts"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minimind-v"
          "$<TARGET_FILE_DIR:cellscope>/third_party/minimind-v"
  COMMENT "Copying CellScope assets and MiniMind-V runtime"
)

if(APPLE)
  find_program(CODESIGN_EXECUTABLE codesign)
  if(CODESIGN_EXECUTABLE)
    add_custom_command(TARGET cellscope POST_BUILD
      COMMAND ${CODESIGN_EXECUTABLE} --force --deep
              --sign "${CELLSCOPE_CODESIGN_IDENTITY}"
              --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CellScope.entitlements"
              "$<TARGET_BUNDLE_DIR:cellscope>"
      COMMENT "Ad-hoc signing CellScope.app for camera permission"
    )
  endif()
endif()

install(TARGETS cellscope
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
install(DIRECTORY assets/ DESTINATION share/cellscope/assets)
install(DIRECTORY scripts/ DESTINATION share/cellscope/scripts)
install(DIRECTORY third_party/minimind-v DESTINATION share/cellscope/third_party)

add_executable(gpu_smoke tools/gpu_smoke.c)
target_link_libraries(gpu_smoke PRIVATE SDL3::SDL3)
if(APPLE)
  add_executable(gpu_ui_smoke tools/gpu_ui_smoke.c)
  target_link_libraries(gpu_ui_smoke PRIVATE SDL3::SDL3)
  add_executable(gpu_ui_batch tools/gpu_ui_batch.c)
  target_link_libraries(gpu_ui_batch PRIVATE SDL3::SDL3)
  add_executable(gpu_atlas_smoke tools/gpu_atlas_smoke.c)
  target_link_libraries(gpu_atlas_smoke PRIVATE SDL3::SDL3)
  add_executable(gpu_text_smoke tools/gpu_text_smoke.c src/fonts.c)
  target_include_directories(gpu_text_smoke PRIVATE src)
target_link_libraries(gpu_text_smoke PRIVATE cellcore SDL3::SDL3 Freetype::Freetype)
endif()
if(SDL3_shadercross_FOUND)
  target_link_libraries(gpu_smoke PRIVATE SDL3_shadercross::SDL3_shadercross)
  target_compile_definitions(gpu_smoke PRIVATE CELLSCOPE_HAVE_SHADERCROSS=1)
endif()

if(CELLSCOPE_BUILD_TESTS)
  enable_testing()
  add_executable(test_core
    tests/test_core.c
  )
  target_link_libraries(test_core PRIVATE cellcore)
  target_include_directories(test_core PRIVATE src)
  add_test(NAME core COMMAND test_core)
endif()
