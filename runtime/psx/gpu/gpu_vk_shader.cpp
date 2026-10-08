// gpu_vk_shader.cpp — SDL_GPU shader creation.
#include "gpu_vk_shader.h"

#include "gpu_vk_check.h"

namespace psx::gpu {

SDL_GPUShader *makeShader(SDL_GPUDevice *device,
                          const std::uint32_t *code,
                          unsigned length,
                          SDL_GPUShaderStage stage,
                          ShaderResources resources) {
  SDL_GPUShaderCreateInfo info = {};
  info.code_size = length;
  info.code = reinterpret_cast<const Uint8 *>(code);
  info.entrypoint = "main";
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.stage = stage;
  info.num_samplers = resources.samplers;
  info.num_uniform_buffers = resources.uniformBuffers;
  info.num_storage_buffers = resources.storageBuffers;
  SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
  GPUCHK(shader, "SDL_CreateGPUShader");
  return shader;
}

} // namespace psx::gpu
