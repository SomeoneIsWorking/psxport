# Framework tools. discdump (CHD/ISO extraction) is the one CMake target; the recompiler + RE/analysis
# tooling are Python/shell scripts run in place. The game-asset exporters (menu_bg_export, tex_export)
# moved to the consuming game repo (Tomba2Engine) with the assets they read.
add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common chdr-static)
target_include_directories(discdump PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)
