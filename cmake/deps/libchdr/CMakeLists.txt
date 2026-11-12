# Installs Beetle PSX's libchdr (static, bundled codecs) as the libchdr package: chdr::chdr-static.
cmake_minimum_required(VERSION 3.21)
project(psxport_libchdr C)

set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${PSXPORT_DEPENDENCY_SOURCE}" libchdr)

set(_libchdr_codecs chdr-lzma miniz zstd)
# The codec include directories serve libchdr's own compile; consumers see only its public headers.
foreach(_target IN LISTS _libchdr_codecs)
  get_target_property(_includes ${_target} INTERFACE_INCLUDE_DIRECTORIES)
  if(_includes)
    list(TRANSFORM _includes REPLACE "^(.+)$" "$<BUILD_INTERFACE:\\1>")
    set_target_properties(${_target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_includes}")
  endif()
endforeach()
set_target_properties(chdr-static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
  "$<BUILD_INTERFACE:${PSXPORT_DEPENDENCY_SOURCE}/include>;$<INSTALL_INTERFACE:include>")

install(TARGETS chdr-static ${_libchdr_codecs} EXPORT libchdr ARCHIVE DESTINATION lib)
install(DIRECTORY "${PSXPORT_DEPENDENCY_SOURCE}/include/" DESTINATION include)
install(EXPORT libchdr NAMESPACE chdr:: FILE libchdrTargets.cmake DESTINATION lib/cmake/libchdr)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libchdrConfig.cmake"
  "include(\"\${CMAKE_CURRENT_LIST_DIR}/libchdrTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libchdrConfig.cmake" DESTINATION lib/cmake/libchdr)
