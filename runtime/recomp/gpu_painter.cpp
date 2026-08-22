#include "gpu_vk_internal.h"

#include "gpu_vk_device.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <lucent/log.h>
#include <stdlib.h>

namespace {

SDL_GPUTexture *createTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo &info, const char *name) {
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
  if (!texture) {
    lucent::error("gpu", "SDL_CreateGPUTexture({}) failed: {}", name, SDL_GetError());
    abort();
  }
  return texture;
}

} // namespace

void GpuVkState::ensure_painter_targets(int width, int height) {
  if (s_painter_color && s_painter_w == width && s_painter_h == height) {
    return;
  }
  SDL_GPUDevice *device = GpuDevice::sInstance->s_dev;
  if (s_painter_color) {
    SDL_ReleaseGPUTexture(device, s_painter_color);
  }
  if (s_painter_rgba) {
    SDL_ReleaseGPUTexture(device, s_painter_rgba);
  }
  if (s_painter_depth) {
    SDL_ReleaseGPUTexture(device, s_painter_depth);
  }

  SDL_GPUTextureCreateInfo color = {};
  color.type = SDL_GPU_TEXTURETYPE_2D;
  color.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  color.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  color.width = width;
  color.height = height;
  color.layer_count_or_depth = 1;
  color.num_levels = 1;
  s_painter_color = createTexture(device, color, "painter packed color");
  color.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
  s_painter_rgba = createTexture(device, color, "painter blend color");

  SDL_GPUTextureCreateInfo depth = {};
  depth.type = SDL_GPU_TEXTURETYPE_2D;
  depth.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  depth.width = width;
  depth.height = height;
  depth.layer_count_or_depth = 1;
  depth.num_levels = 1;
  s_painter_depth = createTexture(device, depth, "painter depth");
  s_painter_w = width;
  s_painter_h = height;
}

bool GpuVkState::painter_begin(uint32_t object) {
  if (!object || s_painter_active || s_painter_ranges >= 256) {
    return false;
  }
  s_painter_active = 1;
  s_painter_object[s_painter_ranges] = object;
  s_painter_first[s_painter_ranges] = s_painter_cmd_n;
  return true;
}

bool GpuVkState::painter_end() {
  if (!s_painter_active) {
    return false;
  }
  s_painter_count[s_painter_ranges] = s_painter_cmd_n - s_painter_first[s_painter_ranges];
  s_painter_active = 0;
  ++s_painter_ranges;
  return !s_painter_overflow && s_painter_count[s_painter_ranges - 1] > 0;
}

bool GpuVkState::painter_command(int material, int first, int count, int semi, int blend) {
  if (!s_painter_active || count <= 0) {
    return false;
  }
  // Never coalesce across an object boundary or a material/blend-state transition: command order is
  // the producer's painter order, and each semitransparent command consumes the preceding packed result.
  if (!semi && s_painter_cmd_n > s_painter_first[s_painter_ranges]) {
    const int previous = s_painter_cmd_n - 1;
    if (s_painter_cmd_material[previous] == material && s_painter_cmd_gouraud[previous] == s_painter_item_gouraud &&
        s_painter_cmd_dither[previous] == s_painter_item_dither && s_painter_cmd_semi[previous] == semi &&
        s_painter_cmd_blend[previous] == blend &&
        s_painter_cmd_first[previous] + s_painter_cmd_count[previous] == first) {
      s_painter_cmd_count[previous] += count;
      return true;
    }
  }
  if (s_painter_cmd_n >= 16384) {
    s_painter_overflow = 1;
    return false;
  }
  const int command = s_painter_cmd_n++;
  s_painter_cmd_material[command] = (uint8_t)material;
  s_painter_cmd_gouraud[command] = (uint8_t)s_painter_item_gouraud;
  s_painter_cmd_dither[command] = (uint8_t)s_painter_item_dither;
  s_painter_cmd_semi[command] = (uint8_t)semi;
  s_painter_cmd_blend[command] = (uint8_t)blend;
  s_painter_cmd_first[command] = first;
  s_painter_cmd_count[command] = count;
  return true;
}

void GpuVkState::painter_staging_stats(int *ordinary, int *painter, int *ranges) const {
  if (ordinary) {
    *ordinary = s_tex_n;
  }
  if (painter) {
    *painter = s_painter_tex_n + s_painter_tri_n;
  }
  if (ranges) {
    *ranges = s_painter_ranges;
  }
}
