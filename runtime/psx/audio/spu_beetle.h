// spu_beetle.h — the SPU register file, DMA port and mixer, served by Beetle's spu.c.
#pragma once

#include <cstdint>

class Core;

namespace psx::audio::spu {

// Create and power the process SPU state each SpuDevice binds over.
void init();
// The Core whose I_STAT the SPU interrupt line latches into; null drops the line.
void bindIrqCore(Core *core);
void write(std::uint32_t addr, std::uint32_t val);
std::uint32_t read(std::uint32_t addr);
void dmaWrite(const std::uint32_t *words, int count);
int dmaRead(std::uint32_t *words, int count);
// Advance the mixer by `clocks` CPU cycles; returns Beetle's clocks until the next SPU event.
std::int32_t update(std::int32_t clocks);
// Drain up to `maxFrames` mixed 44.1 kHz stereo frames; returns the frames written.
int render(std::int16_t *out, int maxFrames);
// The SPU interrupt line into `core` (spu_device.cpp).
void raiseIrq(Core &core, bool asserted);

} // namespace psx::audio::spu
