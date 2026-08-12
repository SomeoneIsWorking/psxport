# cmake/build_id.cmake — stamp the BUILD IDENTITY into a generated header. Run with `cmake -P` from a
# custom target that is part of ALL, so it re-runs at every BUILD rather than only at configure time.
#
# WHY THIS EXISTS: the producer claim DB records which build last EARNED each claim (kanban #91). A
# provenance stamp that is computed at CONFIGURE time is a lie waiting to happen — commit, rebuild
# without re-configuring, and every claim earned by the new code is filed under the old sha. So the
# identity is recomputed on every build and written WRITE-IF-DIFFERENT, which costs one `git describe`
# per build and forces no recompilation when nothing moved.
#
# WHY TWO IDS, and this is a CORRECTION to kanban #91's fix text, which asked only for a psxport stamp.
# A producer's DB key is decided by GAME code as much as by framework code — the fossil that motivated
# the card was created by Tomba2Engine's own `Render::resolvePerModeEmitter` (a GAME commit, 9c94008),
# and a framework-only stamp would have recorded the fossil and its re-earn under the SAME id. The
# mechanism would have been dead by construction. So both repos are described: the framework at
# PSXPORT_ROOT and the consuming project at APP_ROOT (they are the same directory when psxport is built
# standalone, and the writer collapses that case rather than printing it twice).
#
# THE NEGATIVE IS DESIGNED FIRST. There is no fallback that invents an identity: if git is missing, the
# directory is not a repo, or `git describe` fails, the id is the literal string `UNKNOWN(<reason>)`.
# Downstream MUST treat that as "no provenance", never as an id — an id that silently degrades to
# something id-shaped (a timestamp, "dev", the empty string) makes two different builds compare equal,
# which is exactly the false negative the DB exists to stop reporting.
#
# Inputs (all required, all -D on the `cmake -P` command line):
#   PSXPORT_ROOT  the framework repo root
#   APP_ROOT      the consuming project's root (CMAKE_SOURCE_DIR)
#   OUT           the header to write

foreach(_req PSXPORT_ROOT APP_ROOT OUT)
  if(NOT DEFINED ${_req})
    message(FATAL_ERROR "build_id.cmake: -D${_req} is required. Refusing to write a header with a "
                        "guessed identity — see the note above on why a degraded id is worse than none.")
  endif()
endforeach()

find_package(Git QUIET)

# Describe one repo, or say WHY it could not be described. Never returns something id-shaped on failure.
function(psxport_describe dir out_var)
  if(NOT Git_FOUND)
    set(${out_var} "UNKNOWN(no-git-executable)" PARENT_SCOPE)
    return()
  endif()
  if(NOT IS_DIRECTORY "${dir}")
    set(${out_var} "UNKNOWN(no-such-dir)" PARENT_SCOPE)
    return()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
    WORKING_DIRECTORY "${dir}"
    OUTPUT_VARIABLE _desc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR _desc STREQUAL "")
    # Most common real cause: the path is not inside a git work tree (a tarball export, a vendored copy
    # with no .git). Named rather than collapsed into a generic failure.
    set(${out_var} "UNKNOWN(git-describe-rc=${_rc})" PARENT_SCOPE)
    return()
  endif()
  set(${out_var} "${_desc}" PARENT_SCOPE)
endfunction()

psxport_describe("${PSXPORT_ROOT}" _fw)
psxport_describe("${APP_ROOT}" _app)

get_filename_component(_fw_real  "${PSXPORT_ROOT}" REALPATH)
get_filename_component(_app_real "${APP_ROOT}"     REALPATH)

# The composite is what lands in the claim file, so it must be ONE token (no spaces): the claim-file
# reader splits on whitespace. `app+fw`, or just the one id when the two roots are the same repo.
if(_fw_real STREQUAL _app_real)
  set(_composite "${_fw}")
else()
  set(_composite "${_app}+psxport-${_fw}")
endif()

set(_content
"// psxport_build_id.h — GENERATED at every build by cmake/build_id.cmake. Do not edit, do not commit.
//
// PSXPORT_BUILD_ID_COMPOSITE is the token written into the producer claim file as a claim's
// provenance. It is `<app-describe>+psxport-<framework-describe>`, collapsed to one id when the
// framework IS the project being built. An id of the form UNKNOWN(<reason>) means NO IDENTITY WAS
// AVAILABLE and must never be compared as if it were one.
#pragma once
#define PSXPORT_BUILD_ID_FRAMEWORK \"${_fw}\"
#define PSXPORT_BUILD_ID_APP       \"${_app}\"
#define PSXPORT_BUILD_ID_COMPOSITE \"${_composite}\"
")

# WRITE-IF-DIFFERENT. Without this every build would retouch the header and recompile its consumers.
set(_old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _content)
  file(WRITE "${OUT}" "${_content}")
  message(STATUS "psxport build id -> ${_composite}")
endif()
