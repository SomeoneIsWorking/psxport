# psxport_oracle — the independent reference emulator, built from the vendored Mednafen PSX core
# without vendor/beetle-psx/libretro.c. A product-owned reference can share product mistakes.
#
# WHAT IS IN IT, and why it is this short. The glue surface was MEASURED by linking (2026-08-13), not
# estimated: a CPU that only has to STEP needs cpu.c, gte.c and the six PGXP translation units, and those
# leave exactly fifteen symbols undefined — which oracle_shim.c provides. I_STAT/I_MASK use Mednafen's
# irq.c and DPCR uses the register owner factored from dma.c; DMA channels/DICR, CD, GPU, timer, SIO and
# filestream remain outside this stepping library.
#
# ═══ WHY THIS IS A SEPARATE LIBRARY FROM `psxport`, AND THE COLLISION MILESTONE 2 MUST SOLVE ═══════════
# libpsxport ALREADY compiles gte.c (cmake/psxport.cmake) for its own GTE backend. Linking both archives
# into one executable would therefore hit duplicate symbols — and, worse, would have the reference and the
# thing being tested SHARING state, which destroys the independence that is the entire point. A consumer
# must resolve that deliberately, and the two honest options are: run the oracle in a separate PROCESS
# and compare over a pipe (also isolates determinism), or prefix the archive's symbols
# (`objcopy --prefix-symbols=oracle_`). See docs/oracle.md.
set(ORACLE_VENDOR_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/cpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/dma_dpcr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/gte.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/irq.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_cpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_gte.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_gpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_mem.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_value.c)
add_library(psxport_oracle STATIC
  oracle_shim.c
  oracle_snapshot.cpp
  oracle_boundary.cpp
  ${ORACLE_VENDOR_SOURCES})
# Keep the dependency warning policy scoped to dependency translation units.
set_source_files_properties(${ORACLE_VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# gte.c reaches for psxport's own runtime/psx/gte_state.h — which is why psxport builds it too, and why
# the oracle must have runtime/psx on its path. PGXP is RUNTIME-gated (`PGXP_GetModes()`), not
# compile-time, so its objects must be present even though the oracle never enables it.
target_include_directories(psxport_oracle PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/libretro-common/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/deps/libchdr/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx)

# -fno-strict-aliasing: Mednafen's memory paths type-pun through MultiAccessSizeMem, and the vendor build
# uses it. Dropping it would let the optimiser reorder RAM accesses the reference must perform exactly.
target_compile_options(psxport_oracle PRIVATE -Wall -Wextra -O2 -g -fno-strict-aliasing)
target_link_libraries(psxport_oracle PUBLIC m)

# oracle_spike — the proof that the library above actually executes injected MIPS, and refuses to hide a
# hardware access. It runs a POSITIVE program with hand-derived expected registers and a NEGATIVE program
# that must be reported as a hardware stop, prints a check PLAN up front, and REFUSES (exit 2) if the
# number of checks that ran differs from the number planned.
add_executable(oracle_spike oracle_spike.c)
target_link_libraries(oracle_spike PRIVATE psxport_oracle)
target_compile_options(oracle_spike PRIVATE -O2 -g)

# Standalone psxport registers this gate. An embedded consumer still gets the oracle targets but owns
# its own CTest surface unless it explicitly opts into the complete framework suite.
if(PSXPORT_BUILD_TESTS)
  add_test(NAME oracle_spike COMMAND oracle_spike)
endif()

# oracle_trace — runs a REAL game executable in the reference and writes a per-instruction trace, which is
# one side of milestone 2's differential. A trace FILE rather than a pipe or a symbol-prefixed archive: it
# keeps the reference in its own process (so it shares no state with the port), needs no IPC protocol, and
# is reproducible and human-readable, so "did the oracle change or did we?" stays answerable.
add_executable(oracle_trace oracle_trace.c)
target_link_libraries(oracle_trace PRIVATE psxport_oracle)
target_compile_options(oracle_trace PRIVATE -O2 -g)
if(PSXPORT_BUILD_TESTS)
  add_test(NAME crossvalidate_crt0_selftest
           COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                   ${CMAKE_CURRENT_SOURCE_DIR}/crossvalidate_crt0.py --selftest)
  add_test(NAME oracle_trace_selftest
           COMMAND ${CMAKE_COMMAND} -E env "${Python3_EXECUTABLE}"
                   ${CMAKE_CURRENT_SOURCE_DIR}/test_oracle_trace.py
                   $<TARGET_FILE:oracle_trace>
                   --scratch ${CMAKE_BINARY_DIR}/scratch/oracle-trace-selftest)
endif()

# NO mutation harness here. The retired mutation harness rebuilt the shim with one thing broken and
# require oracle_spike to fail. It earned its keep once — it is what exposed the unpopulated-FastMap and
# the missing RAM-mirror bugs — but it was a test OF A TEST, and USER, 2026-08-13: *"we have too much meta
# imo, so deep that the base gets ignored among all of it"*. The coverage that actually matters did not
# live in it: oracle_spike's own 22 checks are what catch a broken FastMap, a reset step clock, an
# unmirrored RAM window and a silently-swallowed device access. Deleting the harness loses the assurance
# that those checks would notice, not the checks themselves.

# Snapshot/window tools remain separate diagnostic processes; the player never links this library.
target_compile_features(psxport_oracle PRIVATE cxx_std_20)
add_executable(oracle_window oracle_window.cpp)
target_compile_features(oracle_window PRIVATE cxx_std_20)
target_compile_options(oracle_window PRIVATE -Wall -Wextra)
target_link_libraries(oracle_window PRIVATE psxport_oracle)
add_executable(test_oracle_snapshot test_oracle_snapshot.cpp)
target_compile_features(test_oracle_snapshot PRIVATE cxx_std_20)
target_compile_options(test_oracle_snapshot PRIVATE -Wall -Wextra)
target_link_libraries(test_oracle_snapshot PRIVATE psxport_oracle)
option(PSXPORT_ORACLE_PC_OBSERVER "Build the separate CPU observer diagnostic qualification" OFF)
if(PSXPORT_ORACLE_PC_OBSERVER)
  target_sources(psxport_oracle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/pc_observer.c)
  target_compile_definitions(psxport_oracle PRIVATE PSX_PC_OBSERVER=1)
  add_executable(test_console_pc_observer test_pc_observer.cpp)
  target_compile_features(test_console_pc_observer PRIVATE cxx_std_20)
  target_compile_options(test_console_pc_observer PRIVATE -Wall -Wextra)
  target_include_directories(test_console_pc_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
  target_link_libraries(test_console_pc_observer PRIVATE psxport_oracle)
  if(PSXPORT_BUILD_TESTS)
    add_test(NAME oracle_pc_observer COMMAND test_console_pc_observer)
    set_tests_properties(oracle_pc_observer PROPERTIES TIMEOUT 60)
  endif()
endif()
if(PSXPORT_BUILD_TESTS)
  add_test(NAME oracle_snapshot COMMAND test_oracle_snapshot ${CMAKE_CURRENT_SOURCE_DIR}/../../scratch/oracle-snapshot)
  add_test(NAME oracle_console_host
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_console_host.py)
  set_tests_properties(oracle_console_host PROPERTIES TIMEOUT 60)
  add_test(NAME oracle_console_observer
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_console_observer.py)
  set_tests_properties(oracle_console_observer PROPERTIES TIMEOUT 60)
  add_test(NAME oracle_compare
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_compare.py)
  set_tests_properties(oracle_compare PROPERTIES TIMEOUT 60)
endif()
