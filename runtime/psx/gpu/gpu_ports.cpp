#include "gpu_ports.h"

#include "core.h"
#include "diagnostic_config.h"
#include "game.h"
#include "gpu_debug.h"
#include "gpu_device.h"
#include "ordering_table.h"

namespace psx::gpu {

void gp1Write(Core &core, std::uint32_t word) {
  core.gpuDevice().gp1(word, frame::CpuTicks{core.game->timing.emulatedCpuTicks()});
}

void dma2LinkedList(Core &core, std::uint32_t madr) {
  core.rsub.lastOrderingTable = mainRamOffsetOf(madr);
  if (core.game->display.frame() == psx::config::gpu_scene_dump_frame()) {
    psx::debug::logScene(core, madr);
  }
  submitOrderingTable(core, core.gpuDevice(), madr);
}

void dma2Block(Core &core, GuestSpan block, std::uint32_t blockWords, bool toGpu) {
  const std::uint32_t base = mainRamOffsetOf(block.address);
  const present::OwnerHandle owner = toGpu ? core.owners.port(present::WriterKind::BlockDma) : present::kNoOwner;
  if (toGpu) {
    core.gpuDevice().noteDrawEntry(static_cast<std::uint32_t>(core.game->timing.displayField()));
  }
  for (std::uint32_t i = 0; i < block.bytes / 4u; i++) {
    // A long upload is milliseconds of host work inside one executor segment; a present due meanwhile lands
    // between DMA blocks.
    if (blockWords != 0 && i % blockWords == 0) {
      core.game->display.showIfDue();
    }
    const std::uint32_t address = base + i * 4u;
    if (toGpu) {
      core.gpuDevice().gp0(core.mem_r32(address), {.sourceAddress = guestAddressOf(address), .owner = owner});
    } else {
      core.mem_w32(address, core.gpuDevice().read());
    }
  }
}

} // namespace psx::gpu
