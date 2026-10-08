add_library(psxport_common STATIC env.cpp env.h)
target_include_directories(psxport_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
