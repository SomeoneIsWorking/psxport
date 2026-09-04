# psxport framework tests — one ctest entry per test file, DISCOVERED BY GLOB.
#
# Adding a test means adding ONE file, `tests/test_<thing>.{c,cpp}`, and nothing else. There is
# deliberately no explicit source list here: several agents add tests to this directory
# concurrently, and an explicit list turns every new test into a conflicting edit to this one shared
# file. Do not "fix" this into a list.
#
# Each file is its own executable with its own main() (see tests/testutil.h) and its own ctest entry,
# so one crashing test cannot take the rest of the suite down with it.
#
# GLOB caveat: CONFIGURE_DEPENDS makes the build system re-check this glob at BUILD time, so a newly
# added test file is picked up by a plain `cmake --build build`. If your generator does not support
# it, or the new test does not appear, re-run `cmake -S . -B build` to re-configure.
file(GLOB PSXPORT_TEST_SRCS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

if(NOT PSXPORT_TEST_SRCS)
  # Not "silently zero tests": an empty tests/ means the framework gate covers nothing, and that
  # must be loud rather than a green `ctest` run with "No tests were found".
  message(WARNING "psxport: tests/ contains no test_*.c/.cpp — `ctest` will gate NOTHING.")
endif()

foreach(test_src IN LISTS PSXPORT_TEST_SRCS)
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  # Match the framework's C++20 contract so tests compile public headers exactly as libpsxport.a.
  set_target_properties(${test_name} PROPERTIES
    # C++20, matching the library. 17 was a trap: the house rule is that diagnostics go through
    # lucent, and <lucent/log.h> needs std::format_string — so the first test of any logging unit
    # would have failed to COMPILE, in a suite whose whole job is to be the thing you trust.
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # Linking libpsxport gives a test the whole framework plus its PUBLIC include dirs. It is a static
  # archive, so only the objects a test actually references are pulled in — a test of one unit does
  # not drag in the renderer. A test that reaches a unit which calls into the game substrate will
  # fail to LINK on title-owned symbols; that is the framework/game seam telling you the unit is not
  # framework-testable in isolation, not a bug in the harness.
  target_link_libraries(${test_name} PRIVATE psxport)

  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES
    # Tests are HERMETIC: no disc, no GPU, no window. The dummy SDL drivers are a backstop so a test
    # that accidentally initialises SDL cannot open a window or grab the audio device on a dev box —
    # they are not a licence to write a test that needs a display.
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    # A hermetic unit test finishes in well under a second. The cap exists so a deadlock (the coro
    # tests can genuinely hang on a regression) is reported as a red test instead of wedging ctest.
    TIMEOUT 60)
endforeach()

list(LENGTH PSXPORT_TEST_SRCS _n_tests)
message(STATUS "psxport tests: ${_n_tests} discovered in ${CMAKE_CURRENT_SOURCE_DIR}")

# Repository policy: whole-tree architecture references plus Clang formatting, structure caps, and
# clang-tidy against every first-party C++ translation unit represented in the real compile database.
# These are normal tests, not hooks.
add_test(NAME repository_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_cpp_style.py
                 --root ${CMAKE_CURRENT_SOURCE_DIR}/..
                 --repository-only)
add_test(NAME cpp_style
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_cpp_style.py
                 --root ${CMAKE_SOURCE_DIR}
                 --compile-commands ${CMAKE_BINARY_DIR})
add_test(NAME cpp_style_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_cpp_style.py --selftest)

# tool_selftests — run every psxport tool's own --selftest, and NAME the tools that have none.
# Added because toystory2/tools/overlay_map.py sat COMMITTED with its constant fold disabled while its own
# --selftest reported 2/7 the whole time: nothing in any repo ever ran a tool selftest. This target runs
# the runner's OWN fixture selftest (7 constructed classes, so the classifier is checked against known
# answers) — that part is hermetic and belongs in ctest. Sweeping the real tools/ directory is NOT wired
# in here: several tools need a disc, a live debug server, or Ghidra, so a ctest over them would be
# environment-dependent and would fail for reasons that are not defects. Run that sweep by hand:
#     uv run --frozen python scripts/tool_selftests.py tools
# Measured 2026-08-13: of 24 framework tools, only 2 have a selftest that runs and passes here, 17 have
# none at all, and 3 are Ghidra-only. That backlog is the finding, not this test.
add_test(NAME tool_selftests_classifier
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/tool_selftests.py --selftest)

# The "does it look right" checks a difference count structurally cannot make: widescreen that changes
# nothing, and an fps60 that inserts a duplicate frame. Only the tool's own verdict selftest is hermetic;
# driving a port needs a disc and a GPU, so that invocation is per-repo and by hand (see its --help).
add_test(NAME looks_right_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/looks_right.py --selftest)

add_test(NAME gpu_shader_generator
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_gpu_shaders.py --selftest)

add_test(NAME gpu_shader_build_ownership
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_gpu_shader_build_ownership.py)

add_test(NAME cmake_test_ownership
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_cmake_test_ownership.py)

add_test(NAME lightrec_dependency_ownership
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_lightrec_dependency_ownership.py)
set_tests_properties(cmake_test_ownership lightrec_dependency_ownership PROPERTIES
  ENVIRONMENT "PSXPORT_LIGHTREC_DIR=${PSXPORT_LIGHTREC_DIR}")

add_test(NAME psx_exe_reader
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_psx_exe.py)

add_test(NAME execution_boundary_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_execution_boundary.py --selftest)

set(_execution_boundary_args
    --root ${CMAKE_CURRENT_SOURCE_DIR}/..
    --cmake ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/psxport.cmake)
if(TARGET psxport_smoke)
  list(APPEND _execution_boundary_args --binary $<TARGET_FILE:psxport_smoke>)
endif()
add_test(NAME execution_boundary_product
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_execution_boundary.py
                 ${_execution_boundary_args})

add_test(NAME launch_environment_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/test_launch_environment.py)

add_test(NAME consumer_verify_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/test_consumer_verify.py)

add_test(NAME lightning_prefix_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_project_lightning_prefix.py)

add_test(NAME lightrec_path_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_project_lightrec_path.py)
