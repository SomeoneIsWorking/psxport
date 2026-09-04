# Framework tools. Native binaries are CMake targets; RE and analysis automation is Python. The
# game-asset exporters (menu_bg_export, tex_export)
# moved to the consuming game repo (Tomba2Engine) with the assets they read.
# crt0_extract reports a PSX executable's crt0 boot group through the SHIPPING decoder (crt0_scan in
# runtime/psx/crt0_verify.h), so the extractor and the boot-time audit that checks its output can
# never drift apart. Header-only include; it needs lucent for the log macros crt0_verify.h uses.
add_executable(crt0_extract crt0_extract.cpp)
target_link_libraries(crt0_extract PRIVATE lucent::lucent)

add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common chdr-static)
target_include_directories(discdump PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)

# psxport_oracle + oracle_spike — the independent reference emulator (the vendored Mednafen PSX core
# without libretro.c). See docs/oracle.md.
add_subdirectory(oracle)

# The offline FMV exporter and its decoder regression suite call the same decoder that ships in
# psxport.  Keeping them as ordinary CMake targets gives the normal compile database, formatter, and
# clang-tidy gate ownership of their C++ sources; the Python build helper remains only a focused
# convenience entry point for users who do not want to build the complete framework.
find_package(ZLIB REQUIRED)

add_executable(fmv_export fmv_export/fmv_export.cpp)
target_link_libraries(fmv_export PRIVATE psxport ZLIB::ZLIB)
set_target_properties(fmv_export PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/fmv_export")

if(PSXPORT_BUILD_TESTS)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  add_executable(test_fmv_decode fmv_export/test_fmv_decode.cpp)
  target_link_libraries(test_fmv_decode PRIVATE psxport OpenSSL::Crypto ZLIB::ZLIB)
  set_target_properties(test_fmv_decode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/fmv_export")
  add_test(NAME fmv_decode_contract COMMAND test_fmv_decode)
endif()
