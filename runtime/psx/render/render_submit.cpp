// render_submit.cpp — RenderDevice's submits and its GPU-fault latch.
//
// A failed submit or a fence that does not signal latches the device off: submitting into a card the kernel
// is resetting turns one lost frame into a ring reset. The fault persists until the user clears it.
#include "render_device.h"

#include "diagnostic_config.h"

#include <lucent/log.h>
#include <lucent/platform.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace psx::render {
namespace {

constexpr Uint64 kFenceBudgetMs = 5000;

// User-level, not per repo: the fault belongs to the card every title shares.
std::string faultMarkerPath() {
  const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory("psxport");
  if (!directory) {
    return {};
  }
  std::string error;
  if (!lucent::platform::ensure_user_data_directory(*directory, error)) {
    lucent::warn("render", "cannot create {}: {}", directory->string(), error);
    return {};
  }
  return (*directory / "gpu_fault").string();
}

} // namespace

void RenderDevice::persistFault(const char *where, const char *detail) {
  const std::string path = faultMarkerPath();
  if (path.empty()) {
    lucent::warn("render", "cannot persist the GPU fault: the next process will start clean");
    return;
  }
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    lucent::warn("render", "could not write the GPU-fault marker {}: the next process will not refuse to start", path);
    return;
  }
  std::fprintf(file, "psxport GPU fault\nwhere: %s\ndetail: %s\n", where, detail);
  std::fclose(file);
  lucent::error(
      "render", "GPU fault persisted to {}: every psxport process refuses the GPU until you rm {}", path, path);
}

bool RenderDevice::faultPreflight() {
  const std::string path = faultMarkerPath();
  if (path.empty()) {
    return true;
  }
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return true;
  }
  char body[512] = {};
  const std::size_t n = std::fread(body, 1, sizeof body - 1, file);
  body[n] = 0;
  std::fclose(file);
  if (psx::config::gpu_fault_override()) {
    lucent::warn(
        "render", "a GPU fault is on record at {} and PSXPORT_GPU_FAULT_OVERRIDE is set: starting anyway", path);
    return true;
  }
  lucent::error("render",
                "refusing to start the GPU: a previous run recorded a GPU fault.\n  marker : {}\n{}"
                "  clear it (your call, your machine): rm {}\n"
                "  or override for a single run:       PSXPORT_GPU_FAULT_OVERRIDE=1\n"
                "  check the kernel first:             journalctl -k -b | grep -iE 'gpu reset|ring|drm'",
                path,
                body,
                path);
  return false;
}

void RenderDevice::latchFault(const char *where, const char *detail) {
  if (faulted_) {
    return;
  }
  faulted_ = true;
  lucent::error("render",
                "GPU fault at {}: {}; the renderer is latched off for the rest of this process and no later capture "
                "is a picture of the game",
                where,
                detail);
  persistFault(where, detail);
}

bool RenderDevice::submit(SDL_GPUCommandBuffer *cmd, const char *where) {
  if (cmd == nullptr) {
    return false;
  }
  if (SDL_SubmitGPUCommandBuffer(cmd)) {
    return true;
  }
  latchFault(where, SDL_GetError());
  return false;
}

// SDL_WaitForGPUFences has no timeout, so the bound is built from the non-blocking query.
bool RenderDevice::submitAndWait(SDL_GPUCommandBuffer *cmd, const char *where) {
  if (cmd == nullptr) {
    return false;
  }
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (fence == nullptr) {
    latchFault(where, SDL_GetError());
    return false;
  }
  const Uint64 start = SDL_GetTicks();
  while (!SDL_QueryGPUFence(device_, fence)) {
    if (SDL_GetTicks() - start >= kFenceBudgetMs) {
      SDL_ReleaseGPUFence(device_, fence);
      latchFault(where, "fence did not signal within the wall-clock budget");
      return false;
    }
    SDL_Delay(1);
  }
  SDL_ReleaseGPUFence(device_, fence);
  return true;
}

} // namespace psx::render
