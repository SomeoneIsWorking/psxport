# psxport — a native/Lightrec PSX→PC runtime. The product library contains native platform owners
# and the runtime guest-execution boundary; offline derived guest source and selectable fallback
# engines are not product inputs.
cmake_minimum_required(VERSION 3.21)
project(psxport CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# libchdr (CHD access for psx::cd::Disc) — from the Beetle PSX submodule (bundles its own lzma).
add_subdirectory(vendor/beetle-psx/deps/libchdr EXCLUDE_FROM_ALL)

add_subdirectory(tools)

# The framework static library `psxport`.
include(cmake/psxport.cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(cmake/testprog.cmake)
endif()
