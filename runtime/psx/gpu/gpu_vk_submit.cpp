// gpu_vk_submit.cpp — GpuDevice's submits and its GPU-fault latch.
//
// A failed submit or a fence that does not signal latches the device off for the rest of the process:
// submitting into a card the kernel is resetting is what turns one lost frame into a desktop-killing
// ring reset. The fault is also persisted for the user, so the next process refuses to start until
// they clear it; it never clears itself.
#include "gpu_vk_device.h"

#include "config_vars.h"       // user_state_dir
#include "diagnostic_config.h" // gpu_fault_override
#include "fs_util.h"

#include <lucent/log.h>

#include <cstdio>
#include <string>

namespace {

// User-level, not per repo: the fault belongs to the card every title shares.
std::string faultMarkerPath() {
  const std::string stateDir = psx::config::user_state_dir();
  if (stateDir.empty()) {
    return {};
  }
  std::string path = stateDir + "/gpu_fault";
  Fs::ensureParentDirs(path.c_str());
  return path;
}

} // namespace

void GpuDevice::persistFault(const char *where, const char *detail) {
  const std::string path = faultMarkerPath();
  if (path.empty()) {
    lucent::warn("gpu_vk",
                 "cannot persist the GPU fault (no XDG_STATE_HOME or HOME): the next process will start clean. "
                 "Do not launch another GPU run until the machine is known good.");
    return;
  }
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    lucent::warn("gpu_vk", "could not write the GPU-fault marker {}: the next process will NOT refuse to start", path);
    return;
  }
  std::fprintf(file, "psxport GPU fault\nwhere: %s\ndetail: %s\n", where, detail);
  std::fclose(file);
  lucent::error(
      "gpu_vk", "GPU fault persisted to {}: every psxport process refuses the GPU until you rm {}", path, path);
}

bool GpuDevice::faultPreflight() {
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
        "gpu_vk", "a GPU fault is on record at {} and PSXPORT_GPU_FAULT_OVERRIDE is set: starting anyway", path);
    return true;
  }
  lucent::error("gpu_vk",
                "REFUSING TO START THE GPU: a previous run recorded a GPU fault.\n  marker : {}\n{}"
                "  clear it (your call, your machine): rm {}\n"
                "  or override for a single run:       PSXPORT_GPU_FAULT_OVERRIDE=1\n"
                "  check the kernel first:             journalctl -k -b | grep -iE 'gpu reset|ring|drm'",
                path,
                body,
                path);
  return false;
}

void GpuDevice::latchFault(const char *where, const char *detail) {
  if (faulted_) {
    return;
  }
  faulted_ = true;
  lucent::error("gpu_vk",
                "GPU FAULT at {}: {} — the renderer is latched off for the rest of this process; any capture "
                "taken from here on is not a picture of the game",
                where,
                detail);
  persistFault(where, detail);
}

bool GpuDevice::submit(SDL_GPUCommandBuffer *cmd, const char *where) {
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
bool GpuDevice::submitAndWait(SDL_GPUCommandBuffer *cmd, const char *where) {
  if (cmd == nullptr) {
    return false;
  }
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (fence == nullptr) {
    latchFault(where, SDL_GetError());
    return false;
  }
  constexpr Uint64 kBudgetMs = 5000;
  const Uint64 start = SDL_GetTicks();
  while (!SDL_QueryGPUFence(s_dev, fence)) {
    if (SDL_GetTicks() - start >= kBudgetMs) {
      SDL_ReleaseGPUFence(s_dev, fence);
      latchFault(where, "fence did not signal within the wall-clock budget");
      return false;
    }
    SDL_Delay(1);
  }
  SDL_ReleaseGPUFence(s_dev, fence);
  return true;
}
