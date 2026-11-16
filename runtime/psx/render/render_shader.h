// render_shader.h — an SDL_GPU shader from the embedded SPIR-V or Metal source.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace psx::render {

struct ShaderResources {
  Uint32 samplers = 0;
  Uint32 uniformBuffers = 0;
  Uint32 storageBuffers = 0;
};

// One stage's SPIR-V, its Metal source where the build translated it (PSXPORT_MSL in the generated header), and the
// resources it binds. A Metal-only variant has no SPIR-V.
struct ShaderCode {
  const std::uint32_t *code = nullptr;
  unsigned length = 0;
  ShaderResources resources;
  const unsigned char *msl = nullptr; // NUL-terminated; `mslLength` excludes the NUL
  unsigned mslLength = 0;
};

// The form `device` takes: Metal source on a device that takes it, else SPIR-V. Ends the run when the device
// refuses the shader or the build carries no form it takes.
SDL_GPUShader *makeShader(SDL_GPUDevice *device, const ShaderCode &shader, SDL_GPUShaderStage stage);

} // namespace psx::render
