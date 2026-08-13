# psxport_oracle — the INDEPENDENT reference emulator, built from the vendored Mednafen PSX core WITHOUT
# vendor/beetle-psx/libretro.c. Milestone 1 of docs/plans/oracle-against-beetle.md, which exists because
# today's side-by-side compare uses our own `recomp_path` as its reference: a wrong assumption shared with
# `pc_faithful` reads as SUCCESS. USER, 2026-08-12: "oracle compare should be done against a verified
# emulator like beetle imo, not our unverified 'faithful' path".
#
# WHAT IS IN IT, and why it is this short. The glue surface was MEASURED by linking (2026-08-13), not
# estimated: a CPU that only has to STEP needs cpu.c, gte.c and the six PGXP translation units, and those
# leave exactly fifteen symbols undefined — which oracle_shim.c provides. The whole core needs 53; the CD,
# GPU, DMA, timer, SIO and filestream layers are simply not in the stepping path, so they are not built.
#
# ═══ WHY THIS IS A SEPARATE LIBRARY FROM `psxport`, AND THE COLLISION MILESTONE 2 MUST SOLVE ═══════════
# libpsxport ALREADY compiles gte.c (cmake/psxport.cmake) for its own GTE backend. Linking both archives
# into one executable would therefore hit duplicate symbols — and, worse, would have the reference and the
# thing being tested SHARING state, which destroys the independence that is the entire point. Milestone 2
# has to resolve that deliberately, and the two honest options are: run the oracle in a separate PROCESS
# and compare over a pipe (also isolates determinism), or prefix the archive's symbols
# (`objcopy --prefix-symbols=oracle_`). Recorded here rather than discovered later — see the plan.
add_library(psxport_oracle STATIC
  oracle_shim.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/cpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/mednafen/psx/gte.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_cpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_gte.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_gpu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_mem.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/beetle-psx/pgxp/pgxp_value.c)

# gte.c reaches for psxport's own runtime/recomp/gte_state.h — which is why psxport builds it too, and why
# the oracle must have runtime/recomp on its path. PGXP is RUNTIME-gated (`PGXP_GetModes()`), not
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/recomp)

# -fno-strict-aliasing: Mednafen's memory paths type-pun through MultiAccessSizeMem, and the vendor build
# uses it. Dropping it would let the optimiser reorder RAM accesses the reference must perform exactly.
target_compile_options(psxport_oracle PRIVATE -w -O2 -g -fno-strict-aliasing)
target_link_libraries(psxport_oracle PUBLIC m)

# oracle_spike — the proof that the library above actually executes injected MIPS, and refuses to hide a
# hardware access. It runs a POSITIVE program with hand-derived expected registers and a NEGATIVE program
# that must be reported as a hardware stop, prints a check PLAN up front, and REFUSES (exit 2) if the
# number of checks that ran differs from the number planned.
add_executable(oracle_spike oracle_spike.c)
target_link_libraries(oracle_spike PRIVATE psxport_oracle)
target_compile_options(oracle_spike PRIVATE -O2 -g)

# Registered unconditionally: the root CMakeLists calls enable_testing() before descending into tools/,
# and this is a GATE — an oracle that stopped executing injected code must fail `ctest`, not merely be
# available to run by hand. Guarding on BUILD_TESTING here made it vanish from ctest silently.
add_test(NAME oracle_spike COMMAND oracle_spike)

# oracle_trace — runs a REAL game executable in the reference and writes a per-instruction trace, which is
# one side of milestone 2's differential. A trace FILE rather than a pipe or a symbol-prefixed archive: it
# keeps the reference in its own process (so it shares no state with the port), needs no IPC protocol, and
# is reproducible and human-readable, so "did the oracle change or did we?" stays answerable.
add_executable(oracle_trace oracle_trace.c)
target_link_libraries(oracle_trace PRIVATE psxport_oracle)
target_compile_options(oracle_trace PRIVATE -O2 -g)

# The instrument-validation gate. `oracle_spike` passing proves the oracle works; THIS proves the spike
# could have said otherwise — it rebuilds the shim with the FastMap population loop disabled (in a
# throwaway copy under scratch/, never the shipping file) and requires the spike to FAIL against it. A
# checker that has only ever seen the case it expects is not an instrument, so this runs in ctest rather
# than living as a claim in a comment.
add_test(NAME oracle_spike_discriminates
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/prove_spike_can_fail.sh ${CMAKE_BINARY_DIR})
set_tests_properties(oracle_spike_discriminates PROPERTIES DEPENDS oracle_spike)
