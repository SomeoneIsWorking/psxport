# Framework tools. Native binaries are CMake targets; RE and analysis automation is Python. The
# game-asset exporters (menu_bg_export, tex_export)
# moved to the consuming game repo (psx-tomba) with the assets they read.
# crt0_extract dumps an executable's crt0 through the shipping scanCrt0/planCrt0.
add_executable(crt0_extract crt0_extract.cpp)
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
