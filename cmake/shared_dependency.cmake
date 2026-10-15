# First-party shared libraries consumed LIVE, like psxport itself: the workspace's sibling
# `shared/<repo>` checkout wins, so an edit there is in every port's next build with no bump. A machine
# without the workspace (a fresh clone, CI) builds the `vendor/<repo>` submodule instead, which
# tools/psxport_fetch.py and scripts/bootstrap_workspace.py initialise. Both are added after RmlUi,
# whose targets they link.
include_guard(GLOBAL)

set(PSXPORT_BUG_REPORT_DIR "" CACHE PATH "Path to a shared/bug-report checkout (default: workspace sibling)")
set(PSXPORT_SETUP_UI_DIR "" CACHE PATH "Path to a shared/setup-ui checkout (default: workspace sibling)")

# Adds shared/<repo> (or vendor/<repo>); `marker` is a file that proves a checkout is that library.
function(psxport_add_shared_dependency repo marker)
  string(TOUPPER "${repo}" _variable)
  string(REPLACE "-" "_" _variable "${_variable}")
  string(TOLOWER "${_variable}" _build)
  set(_candidates "")
  if(PSXPORT_${_variable}_DIR)
    list(APPEND _candidates "${PSXPORT_${_variable}_DIR}")
  endif()
  if(DEFINED ENV{SHARED_DIR})
    list(APPEND _candidates "$ENV{SHARED_DIR}/${repo}")
  endif()
  # A game reaches psxport through its `external/psxport` symlink, so the workspace siblings are
  # found from the checkout's real location, not the symlinked spelling.
  file(REAL_PATH "${PSXPORT_ROOT}" _psxport_checkout)
  list(APPEND _candidates
    "${_psxport_checkout}/../../shared/${repo}"
    "${_psxport_checkout}/../../../shared/${repo}"
    "${PSXPORT_ROOT}/vendor/${repo}")

  set(_resolved "")
  foreach(_candidate IN LISTS _candidates)
    cmake_path(ABSOLUTE_PATH _candidate NORMALIZE OUTPUT_VARIABLE _checkout)
    if(EXISTS "${_checkout}/CMakeLists.txt" AND EXISTS "${_checkout}/${marker}")
      set(_resolved "${_checkout}")
      break()
    endif()
  endforeach()
  if(NOT _resolved)
    list(JOIN _candidates "\n  - " _tried)
    message(FATAL_ERROR
      "psxport requires the ${repo} library. Tried:\n  - ${_tried}\n"
      "Initialise the submodule with\n"
      "  git -C ${PSXPORT_ROOT} submodule update --init vendor/${repo}\n"
      "or clone https://github.com/SomeoneIsWorking/${repo}.git into the shared workspace.")
  endif()
  message(STATUS "psxport: ${repo} from ${_resolved}")
  add_subdirectory("${_resolved}" "${CMAKE_BINARY_DIR}/${_build}_build" EXCLUDE_FROM_ALL)
endfunction()
