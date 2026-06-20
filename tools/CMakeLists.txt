add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common chdr-static)
target_include_directories(discdump PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)

add_executable(menu_bg_export menu_bg_export.cpp)
target_link_libraries(menu_bg_export PRIVATE psxport_common chdr-static z)
target_include_directories(menu_bg_export PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)
