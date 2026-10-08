# Resolve shared/bug-report, the in-app bug report library (https://github.com/SomeoneIsWorking/bug-report).
#
# It is first-party and consumed LIVE, like psxport itself: the workspace's sibling `shared/bug-report`
# checkout wins, so an edit there is in every port's next build with no bump. A machine without the
# workspace (a fresh clone, CI) builds the `vendor/bug-report` submodule instead, which is what
# tools/psxport_fetch.py and scripts/bootstrap_workspace.py initialise.
#
# It must be added AFTER RmlUi: the library links the `RmlUi::Core` target this build already has.
include_guard(GLOBAL)

set(PSXPORT_BUG_REPORT_DIR "" CACHE PATH "Path to a shared/bug-report checkout (default: workspace sibling)")

function(psxport_configure_bug_report_dependency)
  set(_candidates "")
  if(PSXPORT_BUG_REPORT_DIR)
    list(APPEND _candidates "${PSXPORT_BUG_REPORT_DIR}")
  endif()
  if(DEFINED ENV{SHARED_DIR})
    list(APPEND _candidates "$ENV{SHARED_DIR}/bug-report")
  endif()
  # A game reaches psxport through its `external/psxport` symlink, so the workspace siblings are
  # found from the checkout's real location, not the symlinked spelling.
  file(REAL_PATH "${PSXPORT_ROOT}" _psxport_checkout)
  list(APPEND _candidates
    "${_psxport_checkout}/../../shared/bug-report"
    "${_psxport_checkout}/../../../shared/bug-report"
    "${PSXPORT_ROOT}/vendor/bug-report")

  set(_resolved "")
  foreach(_candidate IN LISTS _candidates)
    cmake_path(ABSOLUTE_PATH _candidate NORMALIZE OUTPUT_VARIABLE _checkout)
    if(EXISTS "${_checkout}/CMakeLists.txt" AND EXISTS "${_checkout}/include/bug_report/draft.h")
      set(_resolved "${_checkout}")
      break()
    endif()
  endforeach()
  if(NOT _resolved)
    list(JOIN _candidates "\n  - " _tried)
    message(FATAL_ERROR
      "psxport requires the bug-report library. Tried:\n  - ${_tried}\n"
      "Initialise the submodule with\n"
      "  git -C ${PSXPORT_ROOT} submodule update --init vendor/bug-report\n"
      "or clone https://github.com/SomeoneIsWorking/bug-report.git into the shared workspace.")
  endif()
  message(STATUS "psxport: bug-report from ${_resolved}")
  add_subdirectory("${_resolved}" "${CMAKE_BINARY_DIR}/bug_report_build" EXCLUDE_FROM_ALL)
endfunction()
