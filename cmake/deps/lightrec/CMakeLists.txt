# Installs the pinned Lightrec (static) as the lightrec package: lightrec::lightrec.
cmake_minimum_required(VERSION 3.21)
project(psxport_lightrec C)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${PSXPORT_DEPENDENCY_SOURCE}" lightrec)
include(GNUInstallDirs)

set_property(TARGET lightrec APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "$<INSTALL_INTERFACE:include>")
install(TARGETS lightrec EXPORT lightrec ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT lightrec NAMESPACE lightrec:: FILE lightrecTargets.cmake DESTINATION lib/cmake/lightrec)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lightrecConfig.cmake"
  "include(\"\${CMAKE_CURRENT_LIST_DIR}/lightrecTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lightrecConfig.cmake" DESTINATION lib/cmake/lightrec)
