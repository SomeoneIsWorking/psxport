# Framework tools. discdump (CHD/ISO extraction) is the one CMake target; the recompiler + RE/analysis
# tooling are Python/shell scripts run in place. The game-asset exporters (menu_bg_export, tex_export)
# moved to the consuming game repo (Tomba2Engine) with the assets they read.
# crt0_extract reports a PSX executable's crt0 boot group through the SHIPPING decoder (crt0_scan in
# runtime/recomp/crt0_verify.h), so the extractor and the boot-time audit that checks its output can
# never drift apart. Header-only include; it needs lucent for the log macros crt0_verify.h uses.
add_executable(crt0_extract crt0_extract.cpp)
target_link_libraries(crt0_extract PRIVATE lucent::lucent)

add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common chdr-static)
target_include_directories(discdump PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)

# psxport_oracle + oracle_spike — the INDEPENDENT reference emulator (the vendored Mednafen PSX core
# without libretro.c), so a differential compare stops using our own pc_faithful path as its reference.
# docs/plans/oracle-against-beetle.md.
add_subdirectory(oracle)
