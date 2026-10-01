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

add_test(NAME test_sync_submodules
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_sync_submodules.py")
set_tests_properties(test_sync_submodules PROPERTIES TIMEOUT 60)

# The cross-process half of the save-state determinism proof. `test_state_determinism` itself checks
# only that the synthetic guest program moves guest RAM, because Lightrec allows one initialized
# machine per process and a second `Core` in the same process cannot exist to compare against. This
# driver runs the save/load/control phases in separate processes and applies both verdicts — including
# the one that stops "save == load" from passing because the state file was ignored. It is registered
# separately rather than inside the C++ binary because the separation is the point.
add_test(NAME state_determinism_across_processes
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/test_state_determinism_process.py"
                 "${CMAKE_BINARY_DIR}/tests/test_state_determinism")
set_tests_properties(state_determinism_across_processes PROPERTIES
  ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
  # Each phase builds a machine and writes a ~3.9 MB whole-machine state, three times over.
  TIMEOUT 300)

# The GLOB above gives `test_call_original_resuming` its resume-until-return cases, but the bounded
# refusal is a SEPARATE ctest entry, and the reason is mechanical rather than stylistic: the bound
# calls `std::abort()`, so running it in the same process would take the whole executable down and
# there would be nothing left to assert against. Its `--never-returns` mode installs a SIGABRT handler
# and exits 0 when the refusal fired, so this entry is an ORDINARY test and needs no inverted
# expectation.
#
# WILL_FAIL would have been the wrong tool, and specifically so: ctest reports an abnormal termination
# as an EXCEPTION rather than as a nonzero result, so WILL_FAIL turns the segfault the first attempt
# produced into a red row rather than a green one. The refusal has to be observed by the test process
# and re-expressed as an ordinary exit.
#
# Without this entry the bound is never exercised at all: a bound that is never reached is a hang
# wearing the costume of a fix, and the positive cases pass whether or not it exists.
add_test(NAME test_call_original_resuming_bound
         COMMAND test_call_original_resuming --never-returns)
# THE ASSERTION IS THE MESSAGE, NOT THE EXIT CODE, and both the obvious alternatives were wrong here.
#
# Registered plainly — as this entry was in its first version — ctest expects exit 0, and the binary
# returns 0 whenever the bound does NOT fire (the `return 0` at the end of the `--never-returns` path IS
# the "the bound did not fire" report). So the entry passed while testing the opposite of the comment above
# it, which claimed that without it "the bound is never exercised at all". A test that passes when the
# behaviour is wrong is worse than no test, because it gets read as coverage.
#
# `WILL_FAIL TRUE` inverts that, and is ALSO wrong: **the binary exits 0 even when the bound fires.** The
# framework does call `std::abort()` on the bound — the refusal is emitted, naming the return address,
# the 64 host turns and the cycles — but the process still exits 0, so an exit-code assertion observes
# nothing and `WILL_FAIL` reports red on correct behaviour. Asserting the OBSERVABLE instead makes the
# test pass exactly when the named refusal appears and fail when the call returns, which is the property
# this entry is for, and it does not depend on an exit code this process does not honour.
set_tests_properties(test_call_original_resuming_bound PROPERTIES
  ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
  TIMEOUT 60
  PASS_REGULAR_EXPRESSION "consumed 64 host turn\\(s\\).*without returning")

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
# Measured 2026-09-19, after discovery was changed to WALK the tree: 60 files, of which 2 are imported
# modules and 58 runnable tools; 12 have a selftest (10 pass, 2 need fixtures), 43 have none, 3 are
# Ghidra-only. The earlier figure of "24 framework tools" came from a single-directory listing that never
# descended into tools/port, tools/oracle, tools/formats or tools/mips -- more than half the tree was
# outside a report whose entire point is an honest denominator. That backlog is the finding, not this test.
add_test(NAME tool_selftests_classifier
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/tool_selftests.py --selftest)

# The "does it look right" checks a difference count structurally cannot make: widescreen that changes
# nothing, and an fps60 that inserts a duplicate frame. Only the tool's own verdict selftest is hermetic;
# driving a port needs a disc and a GPU, so that invocation is per-repo and by hand (see its --help).
add_test(NAME looks_right_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/looks_right.py --selftest)

# The fps60 endpoint/owner analysis. Its hermetic part is the attribution: given a constructed
# fps60seq log, does a tile go to the SMALLEST run covering it rather than to whichever run came
# first? Crediting the first one hands every tile to the full-screen sky fill and quietly zeroes
# every other row, which is a wrong answer that looks like a clean one. Walking a real capture needs
# a disc and a GPU, so that invocation is per-repo and by hand (see its --help).
add_test(NAME fps60_check_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/fps60_check.py --selftest)

# The single-image geometry instrument. It had a --selftest and NO ctest, so nothing ran it -- the
# same "a selftest nobody runs is not a check" gap its own runner was written to close. It is named
# here rather than left to tool_selftests.py because that sweep is deliberately not a ctest (see above):
# it reaches this directory now, but several of its tools need a disc, a server, or Ghidra.
add_test(NAME present_geometry_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/present_geometry.py --selftest)

# The PAIRED widescreen check: is the wide picture a projection widening or a stretch? Its sibling
# above measures one picture's shape and is invariant under a whole-frame stretch, which is the
# failure this one exists to catch, so both are registered. The hermetic part is the verdict on two
# constructed pictures; comparing a real title's 4:3 and wide captures needs a disc and a GPU, so
# that invocation is per-repo and by hand (see its --help).
add_test(NAME widescreen_pair_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/widescreen_pair.py --selftest)

add_test(NAME gpu_shader_generator
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_gpu_shaders.py --selftest)

# The guest-code-to-readable-C pipeline's selftest. It is hermetic -- no disc, no image, no Ghidra --
# because every refusal is driven through the shipping modules with an injected command runner, and
# both Ghidra-side scripts import nothing at module level so their decision logic is tested directly.
#
# It is registered HERE, and not left to tool_selftests.py, because that sweep deliberately is not a
# ctest (it reaches tools/ and several of its tools need a disc, a server, or Ghidra) and
# ghidra_decomp.py -- the script this replaces -- was reached by it only as a HOST_ONLY import
# failure, which is a weaker gate than "the body-presence check is wrong".
add_test(NAME decomp_pipeline_selftest
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_decomp_pipeline.py)

# The negative half: a selftest that can only print "ok" is not a test. This copies tools/ and
# tests/ into a temporary directory, removes ONE defence at a time, and requires the selftest above
# to go red on its own subject, with an untouched control green. It needs no Ghidra and no image.
add_test(NAME decomp_pipeline_must_fail
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/selftest_must_fail.py)
set_tests_properties(decomp_pipeline_must_fail PROPERTIES TIMEOUT 900)

# The overlay controls: ONE reader for a resident PS-X EXE and for a module, the SHA-1 gate, and WHAT
# A WRONG LOAD BASE COSTS counted rather than asserted. Hermetic (no disc, no Ghidra) apart from
# reading vagrant's authenticated overlays, and it SKIPS-with-a-said-why when they are absent rather
# than passing a comparison it did not make.
add_test(NAME decomp_overlay_controls
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/controls_overlay.py
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scratch/overlay-controls)
# The module half needs an AUTHENTICATED overlay, which is per-repo and git-ignored, so the path comes
# from the ENVIRONMENT AT RUN TIME, not at configure time. Baking $ENV{...} into the generated
# CTestTestfile.cmake writes this machine's absolute path into a build artefact and then needs a
# re-configure on every other machine; the test reads the variable itself instead.
#
# Absent, the control prints "CONTROLS PARTIAL: 3 of 18 checks ran" and exits 0: a skipped comparison
# is not a passing one, and ctest sees only the exit code, so the DENOMINATOR is what a reader has to
# notice. Run the tool directly with the variable set for the full 18.
set_tests_properties(decomp_overlay_controls PROPERTIES TIMEOUT 300)

# The no-return fixture: does Ghidra's non-return truncation ACTUALLY reproduce, and does
# PSXPORT_CLEAR_NORETURN=all remove it? This one launches Ghidra (~30 s, ~550 MB), which is why it is
# a separate ctest rather than part of the 0.3 s hermetic selftest. It exists because the defence was
# ASSERTED AND UNEXERCISED: on Spyro 1 the analyzer marked 0 of 673 functions, so nothing had ever
# been seen to fail. It is skipped-with-a-said-why when there is no Ghidra, never silently passed.
add_test(NAME decomp_noreturn_fixture
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/noreturn_fixture_run.py)
set_tests_properties(decomp_noreturn_fixture PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
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
  ENVIRONMENT "PSXPORT_LIGHTREC_DIR=${PSXPORT_LIGHTREC_DIR};PSXPORT_FIXTURE_LIGHTNING_LIBRARY=${LIBLIGHTNING};PSXPORT_FIXTURE_LIGHTNING_INCLUDE=${LIBLIGHTNING_INCLUDE_DIR};PSXPORT_FIXTURE_C_COMPILER=${CMAKE_C_COMPILER};PSXPORT_FIXTURE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")

add_test(NAME lightrec_pinned_worktree
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_lightrec_pinned_worktree.py)
# Fixture-only: it builds its own throwaway Lightrec repository and sets its own pin, so it is
# independent of the resolved PSXPORT_LIGHTREC_DIR and never touches the shared checkout.

add_test(NAME psx_exe_reader
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_psx_exe.py)

add_test(NAME ram_disassembler_cli
         COMMAND "${Python3_EXECUTABLE}" ${CMAKE_CURRENT_SOURCE_DIR}/test_disasm.py)
set_tests_properties(ram_disassembler_cli PROPERTIES TIMEOUT 60)

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

# The accept step for every new native override reads the differential's report through this gate, so
# its failure classes (mismatch, zero samples, all-incomparable, missing selector, incomplete report) are
# each a test, beside the clean report that makes them mean something.
add_test(NAME override_differential_gate
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/port/test_override_differential_gate.py)

add_test(NAME lightning_prefix_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_project_lightning_prefix.py)

add_test(NAME lightrec_path_policy
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_project_lightrec_path.py)

add_test(NAME guest_task_unsupported_exit
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_guest_task_exit.py"
                 "$<TARGET_FILE:test_guest_task_lifecycle>")

add_test(NAME linux_ci_setup_policy
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/../.github/actions/setup-linux/test_setup.py")

# framework_fetch_behaviour — the file a port SHIPS, against real git repositories and no network: a
# sibling framework checkout becomes the relative symlink every port follows LIVE, a machine with no
# sibling checkout still gets a usable framework by cloning, a real directory at external/psxport is
# never destroyed, and the sibling checkout's working tree AND .git/modules stay byte-unchanged. Issue
# 0142 is the destruction that last clause exists for: `--auto` from a linked worktree deleted the SHARED
# checkout's vendor/beetle-psx and .git/modules/vendor/beetle-psx.
add_test(NAME framework_fetch_behaviour
         COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_psxport_fetch.py)

# settings_persistence needs no registration here: `tests/CMakeLists.txt` GLOBs `test_*.cpp` and
# creates both the executable and the ctest entry per file. It is named in this comment because the
# DEFECT it covers is not obvious from the filename, and because a reader looking for "where is the
# settings-persistence gate" should not have to know the glob.
#
# MEASURED 2026-09-27: `PSXPORT_SETTINGS` is both the launch-configuration input and the persistence
# target, and every agent run and the launcher point it at a TRACKED file. Opening the options overlay
# therefore rewrote `spyro/tools/shipping_settings.ini` with a live serialisation of the mod state:
# `aspect=1` became `aspect=3` (ASPECT_AUTO, which resolves to the sink's aspect, so 4:3 headless) and
# 21 lines of documentation were deleted. Every later run silently lost widescreen.
