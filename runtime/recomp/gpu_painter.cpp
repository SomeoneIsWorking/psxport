#include "gpu_vk_internal.h"

#include "core.h"
#include "game.h"
#include "gpu_painter.h"
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

bool GpuVkState::painter_begin(uint32_t range_id) {
  if (!range_id || s_painter_active || s_painter_ranges >= 256) {
    return false;
  }
  s_painter_active = 1;
  s_painter_current_object = 0;
  s_painter_range_id[s_painter_ranges] = range_id;
  s_painter_first[s_painter_ranges] = s_painter_cmd_n;
  return true;
}

void GpuVkState::painter_set_item_object(uint32_t object) {
  s_painter_current_object = object;
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
        s_painter_cmd_blend[previous] == blend && s_painter_cmd_object[previous] == s_painter_current_object &&
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
  s_painter_cmd_object[command] = s_painter_current_object;
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

bool gpu_vk_painter_begin(Core *core, uint32_t range_id) {
  return core->game->gpu_vk.painter_begin(range_id);
}

void gpu_vk_painter_set_item_object(Core *core, uint32_t object) {
  core->game->gpu_vk.painter_set_item_object(object);
}

bool gpu_vk_painter_end(Core *core) {
  return core->game->gpu_vk.painter_end();
}

void gpu_vk_painter_stage_draw_area_selftest(GpuVkState &gpu, uint16_t *vram, int vramWidth) {
  float depth[3] = {.40f, .40f, .40f};
  gpu.set_order(7);
  gpu.set_vd(depth);
  gpu.s_painter_item_gouraud = 0;
  gpu.s_painter_item_dither = 0;
  // The triangle covers both readback probes, but only the inner draw area is eligible to paint.
  gpu.draw_tri(200, 190, 255, 0, 0, 280, 190, 255, 0, 0, 240, 230, 255, 0, 0, 220, 200, 260, 220);
  for (int y = 190; y <= 230; ++y) {
    for (int x = 200; x <= 280; ++x) {
      vram[y * vramWidth + x] = 0;
    }
  }
}

bool gpu_vk_painter_check_draw_area_selftest(const uint16_t *localColor, int scaledWidth, int scale) {
  const uint16_t outside = localColor[(205 * scale) * scaledWidth + 216 * scale];
  const uint16_t inside = localColor[(205 * scale) * scaledWidth + 240 * scale];
  const bool ok = outside == 0 && inside == 0x001F;
  lucent::info("gpu_selftest",
               "painter untextured draw-area outside={:04X} expect 0000 inside={:04X} expect 001F => {}",
               outside,
               inside,
               ok ? "PASS" : "FAIL");
  return ok;
}
