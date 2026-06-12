add_executable(discdump discdump.cpp)
target_link_libraries(discdump PRIVATE psxport_common libchdr)
