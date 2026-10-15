// gpu_ports.cpp — see gpu_ports.h.
#include "gpu_ports.h"

#include "core.h"
#include "diagnostic_config.h"
#include "game.h"
#include "gpu_debug.h"
#include "gpu_device.h"
#include "ordering_table.h"

namespace psx::gpu {

void gp0Write(Core &core, std::uint32_t word) {
  core.gpuDevice().gp0(word);
}

void gp1Write(Core &core, std::uint32_t word) {
  core.gpuDevice().gp1(word, frame::CpuTicks{core.game->timing.emulatedCpuTicks()});
}

std::uint32_t gpuRead(Core &core) {
  return core.gpuDevice().read();
}

void dma2LinkedList(Core &core, std::uint32_t madr) {
  core.rsub.lastOrderingTable = mainRamOffsetOf(madr);
  if (core.game->display.frame() == psx::config::gpu_scene_dump_frame()) {
    psx::debug::logScene(core, madr);
  }
  submitOrderingTable(core, core.gpuDevice(), madr);
}

void dma2Block(Core &core, GuestSpan block, bool toGpu) {
  const std::uint32_t base = mainRamOffsetOf(block.address);
  for (std::uint32_t i = 0; i < block.bytes / 4u; i++) {
    const std::uint32_t address = base + i * 4u;
    if (toGpu) {
      core.gpuDevice().gp0(core.mem_r32(address), {.sourceAddress = guestAddressOf(address)});
    } else {
      core.mem_w32(address, core.gpuDevice().read());
    }
  }
}

void loadImage(Core &core, VramRect rect, std::span<const std::uint16_t> pixels) {
  core.gpuDevice().loadImage(rect, pixels);
}

std::span<const std::uint16_t> vram(Core &core) {
  return core.gpuDevice().vram();
}

present::FrameRecord sealRecord(Core &core) {
  return core.gpuDevice().sealRecord();
}

} // namespace psx::gpu
