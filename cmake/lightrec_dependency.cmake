# Resolve the maintained shared Lightrec checkout. The runtime is consumed from shared/ rather than
# copied into each port, and the pinned revision prevents accidentally accepting an unrelated tree.
include_guard(GLOBAL)

set(PSXPORT_LIGHTREC_REVISION "b764c4c9f4bc425a56bfc4c32333ff8200ce8ab9")
set(PSXPORT_LIGHTREC_DIR "" CACHE PATH "Path to the maintained shared/lightrec checkout")

function(psxport_configure_lightrec_dependency)
  set(_candidates "")
  if(PSXPORT_LIGHTREC_DIR)
    list(APPEND _candidates "${PSXPORT_LIGHTREC_DIR}")
  endif()
  if(DEFINED ENV{PSXPORT_LIGHTREC_DIR})
    list(APPEND _candidates "$ENV{PSXPORT_LIGHTREC_DIR}")
  endif()
  if(DEFINED ENV{SHARED_DIR})
    list(APPEND _candidates "$ENV{SHARED_DIR}/lightrec")
  endif()
  list(APPEND _candidates
    "${PSXPORT_ROOT}/../../shared/lightrec"
    "${PSXPORT_ROOT}/../../../shared/lightrec"
    "${PSXPORT_ROOT}/../../../../shared/lightrec"
    "${CMAKE_SOURCE_DIR}/../../shared/lightrec")

  set(_attempted "")
  set(_resolved "")
  foreach(_candidate IN LISTS _candidates)
    cmake_path(ABSOLUTE_PATH _candidate NORMALIZE OUTPUT_VARIABLE _absolute)
    list(APPEND _attempted "${_absolute}")
    if(EXISTS "${_absolute}/CMakeLists.txt" AND EXISTS "${_absolute}/lightrec.h")
      set(_resolved "${_absolute}")
      break()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _attempted)

  if(NOT _resolved)
    list(JOIN _attempted "\n  - " _attempted_lines)
    message(FATAL_ERROR
      "psxport requires shared/lightrec at revision ${PSXPORT_LIGHTREC_REVISION}. Tried:\n"
      "  - ${_attempted_lines}\n"
      "Clone https://github.com/SomeoneIsWorking/lightrec.git into the shared workspace or set "
      "PSXPORT_LIGHTREC_DIR.")
  endif()

  find_package(Git REQUIRED)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_resolved}" rev-parse HEAD
    RESULT_VARIABLE _git_result
    OUTPUT_VARIABLE _revision
    ERROR_VARIABLE _git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "cannot inspect shared/lightrec at ${_resolved}: ${_git_error}")
  endif()
  if(NOT _revision STREQUAL PSXPORT_LIGHTREC_REVISION)
    message(FATAL_ERROR
      "shared/lightrec revision mismatch at ${_resolved}: expected ${PSXPORT_LIGHTREC_REVISION}, "
      "found ${_revision}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_resolved}" status --porcelain --untracked-files=all
    RESULT_VARIABLE _status_result
    OUTPUT_VARIABLE _worktree_changes
    ERROR_VARIABLE _status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _status_result EQUAL 0)
    message(FATAL_ERROR "cannot inspect shared/lightrec worktree at ${_resolved}: ${_status_error}")
  endif()
  if(_worktree_changes)
    message(FATAL_ERROR
      "shared/lightrec at ${_resolved} has worktree changes; psxport requires the exact clean "
      "revision ${PSXPORT_LIGHTREC_REVISION}")
  endif()

  set(PSXPORT_LIGHTREC_DIR "${_resolved}" CACHE PATH "Path to shared/lightrec" FORCE)

  # A parent may already have added Lightrec, but target-name equality is not dependency identity.
  # Refuse an injected target unless it came from the exact source tree resolved and pinned above.
  if(TARGET lightrec)
    get_target_property(_target_source lightrec SOURCE_DIR)
    if(NOT _target_source OR _target_source STREQUAL "_target_source-NOTFOUND")
      message(FATAL_ERROR
        "existing lightrec target does not expose a source directory; expected ${_resolved}")
    endif()
    cmake_path(ABSOLUTE_PATH _target_source NORMALIZE OUTPUT_VARIABLE _target_source_absolute)
    if(NOT _target_source_absolute STREQUAL _resolved)
      message(FATAL_ERROR
        "existing lightrec target source mismatch: expected ${_resolved}, "
        "found ${_target_source_absolute}")
    endif()
    return()
  endif()

  # Lightrec's build declares these generic cache options. Scope the required values to its
  # add_subdirectory call, then restore the consumer's cache entries exactly (including absence).
  foreach(_option IN ITEMS BUILD_SHARED_LIBS BUILD_TESTING)
    get_property(_cache_entry_exists CACHE "${_option}" PROPERTY TYPE SET)
    set("_saved_${_option}_exists" "${_cache_entry_exists}")
    if(_cache_entry_exists)
      get_property("_saved_${_option}_value" CACHE "${_option}" PROPERTY VALUE)
      get_property("_saved_${_option}_type" CACHE "${_option}" PROPERTY TYPE)
      get_property("_saved_${_option}_help" CACHE "${_option}" PROPERTY HELPSTRING)
      get_property("_saved_${_option}_advanced" CACHE "${_option}" PROPERTY ADVANCED)
    endif()
  endforeach()

  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "Do not register dependency-owned tests in consumers" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  add_subdirectory("${_resolved}" "${CMAKE_BINARY_DIR}/shared_lightrec" EXCLUDE_FROM_ALL)

  foreach(_option IN ITEMS BUILD_SHARED_LIBS BUILD_TESTING)
    if(_saved_${_option}_exists)
      set(${_option} "${_saved_${_option}_value}" CACHE "${_saved_${_option}_type}"
        "${_saved_${_option}_help}" FORCE)
      set_property(CACHE "${_option}" PROPERTY ADVANCED "${_saved_${_option}_advanced}")
    else()
      unset(${_option} CACHE)
    endif()
  endforeach()
endfunction()
