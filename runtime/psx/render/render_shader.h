// render_shader.h — an SDL_GPU shader from the embedded SPIR-V.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace psx::render {

struct ShaderResources {
  Uint32 samplers = 0;
  Uint32 uniformBuffers = 0;
  Uint32 storageBuffers = 0;
};

// One stage's SPIR-V and the resources it binds.
struct ShaderCode {
  const std::uint32_t *code = nullptr;
  unsigned length = 0;
  ShaderResources resources;
};

// Ends the run when the device refuses the shader.
SDL_GPUShader *makeShader(SDL_GPUDevice *device, const ShaderCode &shader, SDL_GPUShaderStage stage);

} // namespace psx::render
