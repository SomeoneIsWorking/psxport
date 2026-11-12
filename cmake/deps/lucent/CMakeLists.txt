# Installs Lucent as the lucent package: lucent::lucent, with the consumer's environment names baked in.
cmake_minimum_required(VERSION 3.21)
project(psxport_lucent CXX)

add_subdirectory("${PSXPORT_DEPENDENCY_SOURCE}" lucent)

install(TARGETS lucent EXPORT lucent ARCHIVE DESTINATION lib)
install(DIRECTORY "${PSXPORT_DEPENDENCY_SOURCE}/include/" DESTINATION include)
install(EXPORT lucent NAMESPACE lucent:: FILE lucentTargets.cmake DESTINATION lib/cmake/lucent)
set(_lucent_config "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\n")
if(LUCENT_FORMAT_BACKEND STREQUAL "fmt")
  string(APPEND _lucent_config "find_dependency(fmt CONFIG)\n")
endif()
string(APPEND _lucent_config "include(\"\${CMAKE_CURRENT_LIST_DIR}/lucentTargets.cmake\")\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lucentConfig.cmake" "${_lucent_config}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lucentConfig.cmake" DESTINATION lib/cmake/lucent)
