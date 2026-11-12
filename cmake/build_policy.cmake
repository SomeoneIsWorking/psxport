# The workspace's compile policy: debug-info level per build type.
# psxport's own build and every consumer include it; dependency prefixes receive its resolved flags.
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

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
  _psxport_debug_info_flags(_psxport_debug_lines)
  foreach(_lang IN ITEMS C CXX)
    set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "-O2 ${_psxport_debug_lines} -DNDEBUG" CACHE STRING
        "RelWithDebInfo flags (psxport build policy)" FORCE)
    set(CMAKE_${_lang}_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING
        "Release flags (psxport build policy)" FORCE)
    set(CMAKE_${_lang}_FLAGS_DEBUG "-g" CACHE STRING "Debug flags (psxport build policy)" FORCE)
  endforeach()
endif()
