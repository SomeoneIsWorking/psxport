// render_submit.cpp — RenderDevice's submits and its GPU-fault latch.
//
// A failed submit or a fence that does not signal latches the device off: submitting into a card the kernel
// is resetting turns one lost frame into a ring reset. The marker records the last fault; it never blocks a launch.
#include "render_device.h"

#include "diagnostic_config.h"

#include <lucent/log.h>
#include <lucent/platform.h>

#include <algorithm>
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
    lucent::warn("render", "cannot record the GPU fault: no user data directory");
    return;
  }
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    lucent::warn("render", "could not write the GPU-fault marker {}", path);
    return;
  }
  std::fprintf(file, "psxport GPU fault\nwhere: %s\ndetail: %s\n", where, detail);
  std::fclose(file);
  lucent::error("render", "GPU fault recorded in {}", path);
}

// A device lost in one process says nothing about the next one's context: the kernel resets the ring.
void RenderDevice::reportRecordedFault() {
  const std::string path = faultMarkerPath();
  if (path.empty()) {
    return;
  }
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return;
  }
  char body[512] = {};
  const std::size_t n = std::fread(body, 1, sizeof body - 1, file);
  std::fclose(file);
  std::string record(body, n);
  std::replace(record.begin(), record.end(), '\n', ' ');
  lucent::warn(
      "render", "an earlier run recorded a GPU fault in {}: {}; the kernel log names the process it hit", path, record);
}

namespace {

const char *causeName(FaultCause cause) {
  switch (cause) {
  case FaultCause::Device:
    return "GPU fault";
  case FaultCause::DrawListCycle:
    return "draw-list cycle";
  }
  return "?";
}

const char *playerNotice(FaultCause cause) {
  switch (cause) {
  case FaultCause::Device:
    return "The graphics device failed and the game cannot draw any more, so it will close.";
  case FaultCause::DrawListCycle:
    return "The game asked to draw a list that never ends, which hangs a PlayStation, so it will close.";
  }
  return "?";
}

} // namespace

void RenderDevice::latchFault(FaultCause cause, std::string_view where, std::string_view detail) {
  if (faulted_) {
    return;
  }
  faulted_ = true;
  cause_ = cause;
  fault_ = lucent::format("{} at {}: {}", causeName(cause), where, detail);
  // Work submitted after a device loss never retires, so every later present would only pile up memory.
  disable();
  lucent::error("render",
                "{}; the renderer is latched off for the rest of this process and no later capture is a picture of "
                "the game",
                fault_);
}

void RenderDevice::failDevice(const char *where, const char *detail) {
  latchFault(FaultCause::Device, where, detail);
  persistFault(where, detail);
}

void RenderDevice::reportFaultToPlayer() const {
  window_.showError(windowTitle_ != nullptr ? windowTitle_ : "psxport",
                    lucent::format("{}\n{}", playerNotice(cause_), fault_));
}

bool RenderDevice::submit(SDL_GPUCommandBuffer *cmd, const char *where) {
  if (cmd == nullptr) {
    return false;
  }
  if (SDL_SubmitGPUCommandBuffer(cmd)) {
    return true;
  }
  failDevice(where, SDL_GetError());
  return false;
}

// SDL_WaitForGPUFences has no timeout, so the bound is built from the non-blocking query.
bool RenderDevice::submitAndWait(SDL_GPUCommandBuffer *cmd, const char *where) {
  if (cmd == nullptr) {
    return false;
  }
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (fence == nullptr) {
    failDevice(where, SDL_GetError());
    return false;
  }
  const Uint64 start = SDL_GetTicks();
  while (!SDL_QueryGPUFence(device_, fence)) {
    if (SDL_GetTicks() - start >= kFenceBudgetMs) {
      SDL_ReleaseGPUFence(device_, fence);
      failDevice(where, "fence did not signal within the wall-clock budget");
      return false;
    }
    SDL_Delay(1);
  }
  SDL_ReleaseGPUFence(device_, fence);
  return true;
}

} // namespace psx::render
