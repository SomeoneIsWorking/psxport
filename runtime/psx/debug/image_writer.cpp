#include "image_writer.h"

#include "fs_util.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <lucent/log.h>

// PNG is the default so captures are directly viewable; only an explicit .ppm path keeps the raw
// P6 representation. This function owns directory creation and error reporting because callers do
// not know which filesystem or image-codec operation failed.
bool image_write_rgb24(const char *path, const unsigned char *rgb, int width, int height) {
  if (!path || !rgb || width <= 0 || height <= 0) {
    lucent::error("image_write",
                  "refusing to write: path={} rgb={} {}x{}",
                  path ? path : "(null)",
                  static_cast<const void *>(rgb),
                  width,
                  height);
    return false;
  }
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("image_write", "cannot create the parent directory of {} — NOTHING written", path);
    return false;
  }

  const size_t length = std::strlen(path);
  const bool ppm = length > 4 && std::strcmp(path + length - 4, ".ppm") == 0;
  if (ppm) {
    FILE *file = std::fopen(path, "wb");
    if (!file) {
      lucent::error("image_write", "fopen({}) failed: {}", path, std::strerror(errno));
      return false;
    }
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0 &&
              std::fwrite(rgb, 3, static_cast<size_t>(width) * height, file) == static_cast<size_t>(width) * height;
    if (!ok) {
      lucent::error("image_write", "short write to {}: {}", path, std::strerror(errno));
    }
    if (std::fclose(file) != 0) {
      lucent::error("image_write", "fclose({}) failed: {}", path, std::strerror(errno));
      ok = false;
    }
    return ok;
  }

  SDL_Surface *surface =
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGB24, const_cast<unsigned char *>(rgb), width * 3);
  if (!surface) {
    lucent::error("image_write", "SDL_CreateSurfaceFrom failed for {}: {}", path, SDL_GetError());
    return false;
  }
  const bool ok = IMG_SavePNG(surface, path);
  if (!ok) {
    lucent::error("image_write", "IMG_SavePNG({}) failed: {}", path, SDL_GetError());
  }
  SDL_DestroySurface(surface);
  return ok;
}
