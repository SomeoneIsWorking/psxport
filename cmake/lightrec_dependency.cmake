# Resolve the maintained shared Lightrec checkout. The runtime is consumed from shared/ rather than
# copied into each port, and the pinned revision prevents accidentally accepting an unrelated tree.
#
# A PINNED WORKTREE IS RESOLVED FIRST, at <checkout>/scratch/pins/<revision>, and only then the plain
# checkout. Why: a plain checkout moves. A Lightrec commit landing under a running consumer would change
# what that consumer builds with no edit to it, and the revision check below would turn every such landing
# into a hard configure failure in a tree that was green a minute earlier. The pinned worktree is created
# by `tools/psxport_fetch.py --lightrec`, which reads PSXPORT_LIGHTREC_REVISION from THIS file, so the
# pin has exactly one home and the tool and the resolver cannot disagree about it.
#
# The revision and cleanliness checks below are NOT removed by that: they are what makes a plain
# checkout usable at all, and a pinned worktree that is dirty is refused by the same comparison.
include_guard(GLOBAL)

# Fork: https://github.com/SomeoneIsWorking/lightrec.git, upstream https://github.com/pcercuei/lightrec.git,
# upstream base 550f700c037e8713e4567227514424621cd39cd7. Forked for synchronous translation, per-block
# callbacks, exact counters, range invalidation and fallback classification (docs/migration.md).
# Deliberate deviation: the optimizer strips the compiler's divide guards (bnez/break 7, INT_MIN/-1
# break 6), so a guarded divide by zero or INT_MIN/-1 yields the MIPS LO/HI where an R3000A takes the
# break exception (docs/testprog.md, psx-crash issues 0033 and 0034).
set(PSXPORT_LIGHTREC_REVISION "c967628ecbc0cde312285b712be33f5608ee6fd8")
set(PSXPORT_LIGHTREC_PIN_ROOT "scratch/pins" CACHE STRING
    "Path, relative to a Lightrec checkout, holding one detached worktree per pinned revision")
set(PSXPORT_LIGHTREC_DIR "" CACHE PATH "Path to the maintained shared/lightrec checkout")

# The MAIN checkout of the git repository holding `dir`, or "" when `dir` is not in one. A pinned framework
# worktree (`<psxport>/scratch/pins/<sha>/`) and a port's linked worktree (`<title>/.claude/worktrees/<n>/`)
# sit at depths no fixed `../..` chain matches, so the shared workspace is found from the checkout they
# belong to — the same anchor tools/psxport_fetch.py uses for the shared framework checkout.
function(_psxport_main_checkout dir out)
  set(${out} "" PARENT_SCOPE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse --path-format=absolute --git-common-dir
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _common
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_result EQUAL 0 AND _common MATCHES "/\\.git$")
    cmake_path(GET _common PARENT_PATH _main)
    set(${out} "${_main}" PARENT_SCOPE)
  endif()
endfunction()

function(psxport_configure_lightrec_dependency)
  find_package(Git REQUIRED)
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
  foreach(_tree IN ITEMS "${PSXPORT_ROOT}" "${CMAKE_SOURCE_DIR}")
    _psxport_main_checkout("${_tree}" _main)
    if(_main)
      list(APPEND _candidates "${_main}/../shared/lightrec" "${_main}/../../shared/lightrec")
    endif()
  endforeach()

  # Each checkout yields its PINNED worktree first, then the checkout itself, so the ordering of the
  # explicit settings above is preserved: an explicit PSXPORT_LIGHTREC_DIR still wins, it just prefers
  # that directory's own pinned tree over the moving one.
  set(_attempted "")
  set(_resolved "")
  foreach(_candidate IN LISTS _candidates)
    cmake_path(ABSOLUTE_PATH _candidate NORMALIZE OUTPUT_VARIABLE _checkout)
    list(APPEND _attempted "${_checkout}/${PSXPORT_LIGHTREC_PIN_ROOT}/${PSXPORT_LIGHTREC_REVISION}")
    list(APPEND _attempted "${_checkout}")
    foreach(_probe IN ITEMS
            "${_checkout}/${PSXPORT_LIGHTREC_PIN_ROOT}/${PSXPORT_LIGHTREC_REVISION}"
            "${_checkout}")
      if(EXISTS "${_probe}/CMakeLists.txt" AND EXISTS "${_probe}/lightrec.h")
        set(_resolved "${_probe}")
        break()
      endif()
    endforeach()
    if(_resolved)
      break()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _attempted)

  if(NOT _resolved)
    list(JOIN _attempted "\n  - " _attempted_lines)
    message(FATAL_ERROR
      "psxport requires shared/lightrec at revision ${PSXPORT_LIGHTREC_REVISION}. Tried:\n"
      "  - ${_attempted_lines}\n"
      "Clone https://github.com/SomeoneIsWorking/lightrec.git into the shared workspace, or run\n"
      "  python3 tools/psxport_fetch.py --lightrec\n"
      "to create the pinned worktree at "
      "${PSXPORT_LIGHTREC_PIN_ROOT}/${PSXPORT_LIGHTREC_REVISION} beside it, or set "
      "PSXPORT_LIGHTREC_DIR.")
  endif()

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
      "found ${_revision}.\n"
      "Create the pinned worktree beside it with\n"
      "  python3 tools/psxport_fetch.py --lightrec\n"
      "which writes ${PSXPORT_LIGHTREC_PIN_ROOT}/${PSXPORT_LIGHTREC_REVISION} from the same pin this "
      "message quotes, so a commit landing in the shared checkout cannot change what a consumer builds.")
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
      "revision ${PSXPORT_LIGHTREC_REVISION}. It was NOT modified. Inspect it, then remove or commit "
      "the changes yourself and re-run.")
  endif()

  set(PSXPORT_LIGHTREC_SOURCE_DIR "${_resolved}" CACHE INTERNAL "The exact Lightrec tree this build compiles")

  # Lightning is resolved here, the way Lightrec's own build would, so the prefix is keyed by it.
  find_library(LIBLIGHTNING lightning REQUIRED)
  find_path(LIBLIGHTNING_INCLUDE_DIR NAMES lightning.h lightning/lightning.h REQUIRED)
  psxport_dependency_prefix(lightrec SOURCE "${_resolved}" PACKAGE lightrec RECIPE
    OPTIONS "-DLIBLIGHTNING=${LIBLIGHTNING}" "-DLIBLIGHTNING_INCLUDE_DIR=${LIBLIGHTNING_INCLUDE_DIR}")
endfunction()
