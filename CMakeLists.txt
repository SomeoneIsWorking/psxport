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
add_subdirectory(tools)

# The framework static library `psxport` (+ option(PSXPORT_BUILD_SMOKE) + the standalone agnosticism smoke).
include(cmake/psxport.cmake)
