// gpu_vk_shader.h — an SDL_GPU shader from the embedded SPIR-V.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace psx::gpu {

struct ShaderResources {
  Uint32 samplers = 0;
  Uint32 uniformBuffers = 0;
  Uint32 storageBuffers = 0;
};

// Ends the run when the device refuses the shader.
SDL_GPUShader *makeShader(SDL_GPUDevice *device,
                          const std::uint32_t *code,
                          unsigned length,
                          SDL_GPUShaderStage stage,
                          ShaderResources resources);

} // namespace psx::gpu
