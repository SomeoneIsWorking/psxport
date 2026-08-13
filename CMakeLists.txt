# psxport — a PSX→PC static-recompilation FRAMEWORK. This repo builds the game-agnostic pieces: the
# recompiler tooling, the runtime/recomp substrate (interp + HW backends + SBS harness + VK renderer),
# and the psxport STATIC library a game links. A game (e.g. Tomba2Engine) vendors this repo as a
# submodule, provides GameConfig/GameHooks (runtime/recomp/game_iface.h) + the recompiled substrate
# (generated/), and links libpsxport. See docs/porting-a-new-psx-game.md.
cmake_minimum_required(VERSION 3.21)
project(psxport CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)
find_package(zstd CONFIG QUIET)
if(NOT TARGET zstd::libzstd_shared)
  find_library(ZSTD_LIBRARY zstd REQUIRED)
  add_library(zstd::libzstd_shared SHARED IMPORTED)
  set_target_properties(zstd::libzstd_shared PROPERTIES IMPORTED_LOCATION "${ZSTD_LIBRARY}")
endif()
find_package(Threads REQUIRED)

# libchdr (CHD access for tools/discdump) — from the Beetle PSX submodule (bundles its own lzma).
add_subdirectory(vendor/beetle-psx/deps/libchdr EXCLUDE_FROM_ALL)

add_subdirectory(common)

# enable_testing() must precede any subdirectory that calls add_test(), and tools/oracle does — its
# oracle_spike is a gate, not a demo. Registered here rather than in tests/ because it links
# psxport_oracle (the independent reference emulator) and not `psxport`, so it does not belong to the
# globbed framework suite. A silently-unregistered test is worse than none: `ctest -R oracle` answered
# "No tests were found!!!" while the binary passed 12/12, which reads as a missing feature.
enable_testing()
add_subdirectory(tools)

# The framework static library `psxport` (+ option(PSXPORT_BUILD_SMOKE) + the standalone agnosticism smoke).
include(cmake/psxport.cmake)

# Framework test suite — `ctest --test-dir build`. Must come after psxport.cmake (the tests link
# `psxport`). Tests are globbed in tests/CMakeLists.txt, so adding one edits no shared file.
# This is only wired in a STANDALONE psxport configure; a consuming game includes cmake/psxport.cmake
# directly and never processes this file. See docs/project-map.md ("Tests").
# enable_testing() is called above tools/, which needs it too.
add_subdirectory(tests)
