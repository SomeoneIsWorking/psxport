# Presentation dependencies are desktop pkg-config packages or Android imported CMake targets.  Keep
# that platform split here: renderer owners link one interface target and never probe host packages
# while CMake is cross-compiling.
include_guard(GLOBAL)

function(_psxport_android_target result name)
  foreach(candidate IN LISTS ARGN)
    if(TARGET "${candidate}")
      set("${result}" "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "psxport Android dependency '${name}' did not export an expected CMake target. "
    "Expected one of: ${ARGN}")
endfunction()

function(psxport_configure_presentation_dependencies)
  if(TARGET psxport_presentation_dependencies)
    return()
  endif()

  add_library(psxport_presentation_dependencies INTERFACE)

  if(ANDROID)
    set(PSXPORT_ANDROID_DEPENDENCY_PREFIX "" CACHE PATH
      "Prefix containing Android SDL3, SDL3_image, and FreeType CMake packages")
    if(NOT IS_DIRECTORY "${PSXPORT_ANDROID_DEPENDENCY_PREFIX}")
      message(FATAL_ERROR
        "psxport Android requires PSXPORT_ANDROID_DEPENDENCY_PREFIX. Build the title's Android "
        "dependency prefix through shared/android-port, then configure with "
        "-DPSXPORT_ANDROID_DEPENDENCY_PREFIX=<prefix>.")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${PSXPORT_ANDROID_DEPENDENCY_PREFIX}")

    find_package(SDL3 CONFIG REQUIRED)
    find_package(SDL3_image CONFIG REQUIRED)
    find_package(Freetype CONFIG REQUIRED)
    _psxport_android_target(_psxport_sdl3 "SDL3" SDL3::SDL3 SDL3::SDL3-shared SDL3::SDL3-static)
    _psxport_android_target(_psxport_sdl3_image "SDL3_image"
      SDL3_image::SDL3_image SDL3_image::SDL3_image-shared SDL3_image::SDL3_image-static)
    _psxport_android_target(_psxport_freetype "FreeType" Freetype::Freetype)
    target_link_libraries(psxport_presentation_dependencies INTERFACE
      "${_psxport_sdl3}" "${_psxport_sdl3_image}" "${_psxport_freetype}")
    return()
  endif()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 sdl3)
  pkg_check_modules(SDL3_IMAGE sdl3-image)
  pkg_check_modules(FREETYPE freetype2)
  if(NOT (SDL3_FOUND AND SDL3_IMAGE_FOUND AND FREETYPE_FOUND))
    set(_missing "")
    if(NOT SDL3_FOUND)
      string(APPEND _missing "  sdl3        — Fedora: SDL3-devel | Debian/Ubuntu: libsdl3-dev | macOS: brew install sdl3\n")
    endif()
    if(NOT SDL3_IMAGE_FOUND)
      string(APPEND _missing "  sdl3-image  — Fedora: SDL3_image-devel | Debian/Ubuntu: libsdl3-image-dev | macOS: brew install sdl3_image\n")
    endif()
    if(NOT FREETYPE_FOUND)
      string(APPEND _missing "  freetype2   — Fedora: freetype-devel | Debian/Ubuntu: libfreetype-dev | macOS: brew install freetype\n")
    endif()
    message(FATAL_ERROR "psxport: missing pkg-config dependencies:\n${_missing}"
                        "Install the package(s) above and re-run ./run.sh")
  endif()

  target_include_directories(psxport_presentation_dependencies INTERFACE
    "${SDL3_INCLUDE_DIRS}" "${SDL3_IMAGE_INCLUDE_DIRS}" "${FREETYPE_INCLUDE_DIRS}")
  target_compile_options(psxport_presentation_dependencies INTERFACE
    "${SDL3_CFLAGS_OTHER}" "${FREETYPE_CFLAGS_OTHER}")
  target_link_directories(psxport_presentation_dependencies INTERFACE
    "${SDL3_LIBRARY_DIRS}" "${SDL3_IMAGE_LIBRARY_DIRS}" "${FREETYPE_LIBRARY_DIRS}")
  target_link_libraries(psxport_presentation_dependencies INTERFACE
    "${SDL3_LIBRARIES}" "${SDL3_IMAGE_LIBRARIES}" "${FREETYPE_LIBRARIES}")
endfunction()
