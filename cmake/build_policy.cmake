# The workspace's compile and link policy: debug-info level per build type and the linker.
# psxport's own build and every consumer include it; dependency prefixes receive its resolved flags.
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)
include(CheckLinkerFlag)

function(_psxport_require_lld)
  check_linker_flag(CXX "-fuse-ld=lld" PSXPORT_HAVE_LLD)
  if(NOT PSXPORT_HAVE_LLD)
    message(FATAL_ERROR
      "Clang builds link with lld, and ${CMAKE_CXX_COMPILER} cannot run -fuse-ld=lld. Install it:\n"
      "  Fedora: sudo dnf install lld\n"
      "  Debian/Ubuntu: sudo apt install lld\n"
      "  macOS: brew install lld\n"
      "  Windows: winget install LLVM.LLVM")
  endif()
  foreach(_kind IN ITEMS EXE SHARED MODULE)
    set(_flags "${CMAKE_${_kind}_LINKER_FLAGS}")
    if(NOT _flags MATCHES "(^| )-fuse-ld=lld( |$)")
      string(STRIP "${_flags} -fuse-ld=lld" _flags)
      set(CMAKE_${_kind}_LINKER_FLAGS "${_flags}" CACHE STRING "Linker flags" FORCE)
    endif()
  endforeach()
endfunction()

# Line tables keep backtraces at file:line; full -g is the Debug build's alone.
function(_psxport_debug_info_flags out)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(_lines "-gline-tables-only")
  else()
    set(_lines "-g1")
  endif()
  set(CMAKE_REQUIRED_FLAGS "${_lines} -gz=zstd")
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_source_compiles("int main() { return 0; }" PSXPORT_HAVE_GZ_ZSTD)
  if(PSXPORT_HAVE_GZ_ZSTD)
    set(${out} "${_lines} -gz=zstd" PARENT_SCOPE)
  else()
    set(${out} "${_lines}" PARENT_SCOPE)
  endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    _psxport_require_lld()
  endif()
  _psxport_debug_info_flags(_psxport_debug_lines)
  foreach(_lang IN ITEMS C CXX)
    set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "-O2 ${_psxport_debug_lines} -DNDEBUG" CACHE STRING
        "RelWithDebInfo flags (psxport build policy)" FORCE)
    set(CMAKE_${_lang}_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING
        "Release flags (psxport build policy)" FORCE)
    set(CMAKE_${_lang}_FLAGS_DEBUG "-g" CACHE STRING "Debug flags (psxport build policy)" FORCE)
  endforeach()
endif()
