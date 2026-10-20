// gpu_ports.h — the guest's GP1 writes and DMA channel 2 into the Core's GpuDevice.
#pragma once

#include "guest_address_range.h"

#include <cstdint>

class Core;

namespace psx::gpu {

// A GP1 write at the device's current scanline time.
void gp1Write(Core &core, std::uint32_t word);
// DMA2 sync mode 2: walk the ordering table at `madr` into GP0.
void dma2LinkedList(Core &core, std::uint32_t madr);
// DMA2 sync modes 0/1: the words of `block` between RAM and GP0 (toGpu) or GPUREAD.
void dma2Block(Core &core, GuestSpan block, bool toGpu);

} // namespace psx::gpu
