// device_vram.h — class DeviceVram: the GPU device's VRAM, settled when read.
#pragma once

#include "vram_rect.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::gpu {

// The device draws a command's pixels only once something reads them (gpu.c's deferred pixels), so a reader
// names what it is about to read and gets those pixels as the commands so far left them. Holding one costs
// nothing: a present that never reads VRAM never makes the device draw.
class DeviceVram {
public:
  using Settle = void (*)(VramRect rect);

  DeviceVram() = default;
  DeviceVram(std::span<const std::uint16_t> words, Settle settle) : words_(words), settle_(settle) {}

  // All of VRAM, 1024x512.
  [[nodiscard]] std::span<const std::uint16_t> all() const {
    return settled({0, 0, kVramWidth, kVramHeight});
  }
  // VRAM with `rect` (wrapping at VRAM's edges) settled; pixels outside it may lag.
  [[nodiscard]] std::span<const std::uint16_t> settled(VramRect rect) const {
    if (settle_ != nullptr) {
      settle_(rect);
    }
    return words_;
  }

private:
  std::span<const std::uint16_t> words_;
  Settle settle_ = nullptr;
};

} // namespace psx::gpu
