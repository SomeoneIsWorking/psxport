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

# gte.c reaches for psxport's own runtime/psx/cpu/gte_state.h — which is why psxport builds it too, and why
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx # the flat root, kept for game/*.h comments
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/boot # boot
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/core # core
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/cpu # cpu
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/cpu/beetle # the Beetle GTE seam
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/frame # frame
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/gpu # gpu
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/present # present
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/input # input
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/cd # cd
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/audio # audio
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/movie # movie
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/debug # debug
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/config # config
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/hle # hle
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/platform # platform
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/psx/game)

# -fno-strict-aliasing: Mednafen's memory paths type-pun through MultiAccessSizeMem, and the vendor build
# uses it. Dropping it would let the optimiser reorder RAM accesses the reference must perform exactly.
target_compile_options(psxport_oracle PRIVATE -Wall -Wextra -O2 -g -fno-strict-aliasing)
target_link_libraries(psxport_oracle PUBLIC m)

# oracle_trace — runs a REAL game executable in the reference and writes a per-instruction trace, which is
# one side of milestone 2's differential. A trace FILE rather than a pipe or a symbol-prefixed archive: it
# keeps the reference in its own process (so it shares no state with the port), needs no IPC protocol, and
# is reproducible and human-readable, so "did the oracle change or did we?" stays answerable.
add_executable(oracle_trace oracle_trace.c)
# oracle_shim.h fixes its enums' base type, a C23 feature, for the C and C++ units that share its structs.
set_target_properties(psxport_oracle oracle_trace PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_link_libraries(oracle_trace PRIVATE psxport_oracle)
target_compile_options(oracle_trace PRIVATE -O2 -g)
# Snapshot/window tools remain separate diagnostic processes; the player never links this library.
target_compile_features(psxport_oracle PRIVATE cxx_std_20)
add_executable(oracle_window oracle_window.cpp)
target_compile_features(oracle_window PRIVATE cxx_std_20)
target_compile_options(oracle_window PRIVATE -Wall -Wextra)
target_link_libraries(oracle_window PRIVATE psxport_oracle)
