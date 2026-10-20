# Framework tools. Native binaries are CMake targets; RE and analysis automation is Python. The
# game-asset exporters (menu_bg_export, tex_export)
# moved to the consuming game repo (Tomba2Engine) with the assets they read.
# crt0_extract reports a PSX executable's crt0 boot group through the SHIPPING decoder (crt0_scan in
# runtime/psx/boot/crt0_verify.h), so the extractor and the boot-time audit that checks its output can
# never drift apart. Header-only include; it needs lucent for the log macros crt0_verify.h uses.
add_executable(crt0_extract crt0_extract.cpp)
# psxport (not just lucent) for its PUBLIC include path: crt0_verify.h and crt0_boot.h reach across the
# boot/ and core/ directories by basename, and a tool that spells a header's DIRECTORY can never see a
# header that moves. Linking the static library exports the whole subsystem include path and pulls in no
# code the extractor does not call.
target_link_libraries(crt0_extract PRIVATE psxport psxport_internal lucent::lucent)

add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport)

# psxport_oracle — the independent reference emulator (the vendored Mednafen PSX core
# without libretro.c). See docs/oracle.md.
add_subdirectory(oracle)

# The offline FMV exporter calls the same decoder that ships in psxport; the Python build helper
# remains a convenience entry point for users who do not want to build the complete framework.
add_executable(fmv_export fmv_export/fmv_export.cpp)
target_link_libraries(fmv_export PRIVATE psxport)
set_target_properties(fmv_export PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/fmv_export")
