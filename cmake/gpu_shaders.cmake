# Build one consumer-owned SDL_GPU shader header.
#
# Each configured build must own both the header and its dependency stamp. Registering a source-tree
# header as a BYPRODUCT lets `clean` in any consumer delete the input of every other consumer.
function(psxport_add_gpu_shaders psxport_root target)
  set(shader_runtime_dir ${psxport_root}/runtime/recomp)
  file(GLOB shader_sources CONFIGURE_DEPENDS
    ${shader_runtime_dir}/shaders_gpu/*.vert
    ${shader_runtime_dir}/shaders_gpu/*.frag
    ${shader_runtime_dir}/shaders_gpu/*.glsl)

  set(shader_header ${CMAKE_CURRENT_BINARY_DIR}/psxport_generated/gpu_vk_shaders.h)
  set(shader_stamp ${CMAKE_CURRENT_BINARY_DIR}/psxport_gpu_shaders.stamp)
  set(shader_generator ${psxport_root}/tools/gen_gpu_shaders.py)

  # The stamp owns dependency freshness; the generated header is replaced only when its bytes
  # change. A timestamp-only shader edit recompiles GLSL without rebuilding header consumers.
  add_custom_command(OUTPUT ${shader_stamp}
    BYPRODUCTS ${shader_header}
    COMMAND ${shader_generator} --output ${shader_header} --stamp ${shader_stamp}
    DEPENDS ${shader_sources} ${shader_generator}
    WORKING_DIRECTORY ${psxport_root}
    COMMENT "Generating SDL_GPU SPIR-V header (gpu_vk_shaders.h)"
    VERBATIM)

  # Makefile generators do not rebuild a missing BYPRODUCT from a current stamp. This cheap
  # existence guard invokes the same authoritative generator before any C++ target can include it.
  add_custom_target(gen_gpu_shaders
    COMMAND ${shader_generator} --output ${shader_header} --stamp ${shader_stamp}
    DEPENDS ${shader_stamp}
    WORKING_DIRECTORY ${psxport_root}
    VERBATIM)

  target_sources(${target} PRIVATE ${shader_header})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(${target} gen_gpu_shaders)
endfunction()
