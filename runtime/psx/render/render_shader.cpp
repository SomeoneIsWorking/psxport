// render_shader.cpp — SDL_GPU shader creation.
#include "render_shader.h"

#include "render_check.h"

namespace psx::render {

SDL_GPUShader *makeShader(SDL_GPUDevice *device, const ShaderCode &shader, SDL_GPUShaderStage stage) {
  SDL_GPUShaderCreateInfo info = {};
  info.code_size = shader.length;
  info.code = reinterpret_cast<const Uint8 *>(shader.code);
  info.entrypoint = "main";
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.stage = stage;
  info.num_samplers = shader.resources.samplers;
  info.num_uniform_buffers = shader.resources.uniformBuffers;
  info.num_storage_buffers = shader.resources.storageBuffers;
  SDL_GPUShader *created = SDL_CreateGPUShader(device, &info);
  GPUCHK(created, "SDL_CreateGPUShader");
  return created;
}

} // namespace psx::render
