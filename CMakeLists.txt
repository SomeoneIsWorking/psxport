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

# libchdr (CHD access for tools/discdump) — from the Beetle PSX submodule (bundles its own lzma).
add_subdirectory(vendor/beetle-psx/deps/libchdr EXCLUDE_FROM_ALL)

add_subdirectory(common)

# A standalone psxport configure owns and runs the framework suite. An embedded consumer owns its own
# CTest surface: registering psxport tests there advertises executables that a consumer target did not
# build, and makes framework cpp_style inspect the consumer with framework policy. Explicit ON remains
# available to a consumer that deliberately builds and runs the complete framework suite.
option(PSXPORT_BUILD_TESTS "Register the complete psxport CTest suite" ${PROJECT_IS_TOP_LEVEL})
if(PSXPORT_BUILD_TESTS)
  enable_testing()
endif()
add_subdirectory(tools)

# The framework static library `psxport` (+ option(PSXPORT_BUILD_SMOKE) + the standalone agnosticism smoke).
include(cmake/psxport.cmake)

# Framework test suite — `ctest --test-dir build`. Must come after psxport.cmake (the tests link
# `psxport`). Embedded consumers default this off even if they add_subdirectory the framework root.
if(PSXPORT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
