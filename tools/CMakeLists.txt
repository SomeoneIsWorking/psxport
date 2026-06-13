add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common chdr-static)
target_include_directories(discdump PRIVATE
  ${CMAKE_SOURCE_DIR}/vendor/beetle-psx/deps/libchdr/include)
