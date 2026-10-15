// gpu_ports.h — the GPU's guest-visible ports: GP0/GP1 writes, GPUREAD, and DMA channel 2. Every
// access goes to the Core's GpuDevice.
#pragma once

#include "frame_record.h"
#include "guest_address_range.h"
#include "vram_rect.h"

#include <cstdint>
#include <span>

class Core;

namespace psx::gpu {

void gp0Write(Core &core, std::uint32_t word);
void gp1Write(Core &core, std::uint32_t word);
std::uint32_t gpuRead(Core &core);
// DMA2 sync mode 2: walk the ordering table at `madr` into GP0.
void dma2LinkedList(Core &core, std::uint32_t madr);
// DMA2 sync modes 0/1: the words of `block` between RAM and GP0 (toGpu) or GPUREAD.
void dma2Block(Core &core, GuestSpan block, bool toGpu);
// `pixels` (width x height halfwords) into VRAM at `rect`, as the guest's LoadImage does.
void loadImage(Core &core, VramRect rect, std::span<const std::uint16_t> pixels);
// The device's whole VRAM, row-major 1024x512 halfwords.
std::span<const std::uint16_t> vram(Core &core);
// The frame record the device accumulated since the last seal.
present::FrameRecord sealRecord(Core &core);

} // namespace psx::gpu
