# The framework test program (docs/testprog.md): testprog/guest/testprog.c built for mipsel into a PS-X EXE, and
# psxtest_port, the product that runs it as a title. Only psxport's own build defines it.
set(PSXPORT_TESTPROG_ROOT "${CMAKE_BINARY_DIR}/testprog")
set(PSXPORT_TESTPROG_HEADER "${PSXPORT_TESTPROG_ROOT}/include/testprog_identity.h")
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(PSXPORT_TESTPROG_CLANG "${CMAKE_C_COMPILER}")
else()
  find_program(PSXPORT_TESTPROG_CLANG clang REQUIRED)
endif()
add_custom_command(
  OUTPUT "${PSXPORT_TESTPROG_HEADER}" "${PSXPORT_TESTPROG_ROOT}/psxtest/PSXT_000.01"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/testprog/build_testprog.py"
          --clang "${PSXPORT_TESTPROG_CLANG}"
          --source "${CMAKE_CURRENT_SOURCE_DIR}/testprog/guest/testprog.c"
          --object "${PSXPORT_TESTPROG_ROOT}/testprog.o"
          --root "${PSXPORT_TESTPROG_ROOT}"
          --header "${PSXPORT_TESTPROG_HEADER}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/testprog/guest/testprog.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/testprog/build_testprog.py"
  COMMENT "Building the test program PS-X EXE"
  VERBATIM)
add_custom_target(psxtest_exe DEPENDS "${PSXPORT_TESTPROG_HEADER}")

psxport_add_product(
  psxtest_port
  CATALOG psx::testprog::createCatalog
  HEADER catalog.h
  SOURCES testprog/host/catalog.cpp
          testprog/host/debug_menu.cpp
          testprog/host/facts.cpp
          testprog/host/frame_driver.cpp
          testprog/host/runtime.cpp
          testprog/host/square_producer.cpp)
add_dependencies(psxtest_port psxtest_exe gen_gpu_shaders)
target_include_directories(psxtest_port PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/testprog/host"
                                                "${PSXPORT_TESTPROG_ROOT}/include")
set_target_properties(psxtest_port PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
