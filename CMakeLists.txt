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

# libchdr (CHD access for tools/discdump) comes from the Beetle PSX submodule;
# it bundles its own lzma (chdr-lzma). DuckStation was removed from this repo.
add_subdirectory(vendor/beetle-psx/deps/libchdr EXCLUDE_FROM_ALL)

add_subdirectory(common)
add_subdirectory(tools)

# The native PC port binary (scratch/bin/tomba2_port). Guarded so the lightweight discdump-only
# configure used by run.sh still works when SDL2/Vulkan/FreeType dev libs are absent.
find_package(Threads REQUIRED)
include(cmake/tomba2_port.cmake)
