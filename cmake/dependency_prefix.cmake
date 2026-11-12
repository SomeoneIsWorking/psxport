# Third-party dependencies built once per source content and toolchain into a shared install prefix
# outside every repository, then consumed through find_package. A consumer configure builds a missing
# prefix under a lock; every later configure with the same key only finds it.
include_guard(GLOBAL)

if(DEFINED ENV{PSXPORT_DEPENDENCY_ROOT})
  set(_psxport_dependency_root_default "$ENV{PSXPORT_DEPENDENCY_ROOT}")
elseif(DEFINED ENV{XDG_CACHE_HOME})
  set(_psxport_dependency_root_default "$ENV{XDG_CACHE_HOME}/psxport/deps")
elseif(WIN32 AND DEFINED ENV{LOCALAPPDATA})
  set(_psxport_dependency_root_default "$ENV{LOCALAPPDATA}/psxport/deps")
else()
  set(_psxport_dependency_root_default "$ENV{HOME}/.cache/psxport/deps")
endif()
set(PSXPORT_DEPENDENCY_ROOT "${_psxport_dependency_root_default}" CACHE PATH
    "Shared install prefixes of psxport's third-party dependencies")
set(_PSXPORT_DEPENDENCY_RECIPES "${CMAKE_CURRENT_LIST_DIR}/deps")

# The toolchain and flag variables a prefix is built with and keyed by.
set(_PSXPORT_DEPENDENCY_FORWARDED
  CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_C_FLAGS CMAKE_CXX_FLAGS
  CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS
  CMAKE_MAKE_PROGRAM CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_PREFIX_PATH CMAKE_FIND_ROOT_PATH
  CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_MSVC_RUNTIME_LIBRARY
  ANDROID_ABI ANDROID_PLATFORM ANDROID_STL ANDROID_NDK)

# Content identity of a source tree: every tracked and untracked-unignored file, by path and hash.
function(_psxport_source_identity dir out_hash out_files)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ls-files --cached --others --exclude-standard
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _listing
    ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "cannot list dependency sources in ${dir}: ${_error}")
  endif()
  string(REPLACE "\n" ";" _relative "${_listing}")
  list(REMOVE_DUPLICATES _relative)
  list(SORT _relative)
  set(_manifest "")
  set(_files "")
  foreach(_file IN LISTS _relative)
    if(EXISTS "${dir}/${_file}" AND NOT IS_DIRECTORY "${dir}/${_file}")
      file(SHA256 "${dir}/${_file}" _file_hash)
      string(APPEND _manifest "${_file_hash} ${_file}\n")
      list(APPEND _files "${dir}/${_file}")
    endif()
  endforeach()
  string(SHA256 _hash "${_manifest}")
  set(${out_hash} "${_hash}" PARENT_SCOPE)
  set(${out_files} "${_files}" PARENT_SCOPE)
endfunction()

# psxport_dependency_prefix(<name> SOURCE <dir> PACKAGE <package> [RECIPE] [OPTIONS -D...])
# Builds <dir> (or, with RECIPE, cmake/deps/<name> over <dir>) into its keyed prefix when missing and
# finds <package> there. The imported targets are promoted to global so every consumer directory
# links them.
function(psxport_dependency_prefix name)
  cmake_parse_arguments(PARSE_ARGV 1 DEP "RECIPE" "SOURCE;PACKAGE" "OPTIONS")
  if(NOT DEP_SOURCE OR NOT DEP_PACKAGE OR DEP_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "psxport_dependency_prefix(<name> SOURCE <dir> PACKAGE <package> [RECIPE] [OPTIONS ...])")
  endif()
  find_package(Git REQUIRED)
  cmake_path(ABSOLUTE_PATH DEP_SOURCE NORMALIZE OUTPUT_VARIABLE _source)
  _psxport_source_identity("${_source}" _identity _inputs)
  set(_project "${_source}")
  set(_definitions "")
  if(DEP_RECIPE)
    set(_project "${_PSXPORT_DEPENDENCY_RECIPES}/${name}")
    file(SHA256 "${_project}/CMakeLists.txt" _recipe)
    string(APPEND _identity " ${_recipe}")
    list(APPEND _inputs "${_project}/CMakeLists.txt")
    list(APPEND _definitions "-DPSXPORT_DEPENDENCY_SOURCE=${_source}")
  endif()

  get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(_multi_config OR NOT CMAKE_BUILD_TYPE)
    set(_config "Release")
  else()
    set(_config "${CMAKE_BUILD_TYPE}")
  endif()
  string(TOUPPER "${_config}" _config_upper)
  list(APPEND _definitions "-DCMAKE_BUILD_TYPE=${_config}")
  foreach(_variable IN LISTS _PSXPORT_DEPENDENCY_FORWARDED
          ITEMS CMAKE_C_FLAGS_${_config_upper} CMAKE_CXX_FLAGS_${_config_upper})
    if(DEFINED ${_variable} AND NOT "${${_variable}}" STREQUAL "")
      string(REPLACE ";" "\\;" _value "${${_variable}}")
      list(APPEND _definitions "-D${_variable}=${_value}")
    endif()
  endforeach()
  list(APPEND _definitions ${DEP_OPTIONS})

  string(SHA256 _key "${name}\n${_identity}\n${CMAKE_GENERATOR}\n${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n${_definitions}")
  string(SUBSTRING "${_key}" 0 16 _key)
  set(_prefix "${PSXPORT_DEPENDENCY_ROOT}/${name}/${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${_config}-${_key}")
  set(_stamp "${_prefix}/psxport-dependency.stamp")

  file(MAKE_DIRECTORY "${PSXPORT_DEPENDENCY_ROOT}/${name}")
  file(LOCK "${_prefix}.lock" GUARD FUNCTION TIMEOUT 7200)
  if(NOT EXISTS "${_stamp}")
    set(_build "${_prefix}.build")
    set(_staging "${_prefix}.staging")
    set(_log "${_prefix}.log")
    file(REMOVE_RECURSE "${_build}" "${_staging}" "${_prefix}")
    message(STATUS "psxport: building ${name} into ${_prefix}")
    foreach(_step IN ITEMS configure build install)
      if(_step STREQUAL "configure")
        set(_command "${CMAKE_COMMAND}" -S "${_project}" -B "${_build}" -G "${CMAKE_GENERATOR}"
            "-DCMAKE_INSTALL_PREFIX=${_staging}" ${_definitions})
      elseif(_step STREQUAL "build")
        set(_command "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}" --parallel)
      else()
        set(_command "${CMAKE_COMMAND}" --install "${_build}" --config "${_config}")
      endif()
      execute_process(COMMAND ${_command} RESULT_VARIABLE _result OUTPUT_VARIABLE _output
                      ERROR_VARIABLE _output)
      file(APPEND "${_log}" "${_output}")
      if(NOT _result EQUAL 0)
        message(FATAL_ERROR "psxport: ${name} ${_step} failed; the log is ${_log}")
      endif()
    endforeach()
    file(TOUCH "${_staging}/psxport-dependency.stamp")
    file(RENAME "${_staging}" "${_prefix}")
    file(REMOVE_RECURSE "${_build}")
    file(REMOVE "${_log}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_stamp}" ${_inputs})

  get_directory_property(_before IMPORTED_TARGETS)
  unset(${DEP_PACKAGE}_DIR CACHE)
  find_package(${DEP_PACKAGE} CONFIG REQUIRED PATHS "${_prefix}" NO_DEFAULT_PATH)
  get_directory_property(_after IMPORTED_TARGETS)
  list(REMOVE_ITEM _after ${_before})
  foreach(_target IN LISTS _after)
    set_target_properties(${_target} PROPERTIES IMPORTED_GLOBAL TRUE)
  endforeach()
endfunction()
