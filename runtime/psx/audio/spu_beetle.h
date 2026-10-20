// The SPU register file, DMA port and mixer, served by Beetle's spu.c on the bound SpuDevice.
#pragma once

#include <cstdint>

class Core;

namespace psx::audio::spu {

// `debug spu` print counters; log numbering only, never machine state.
struct DebugCounters {
  long writes = 0;
  long dataPortWrites = 0;
  long voiceWrites = 0;
  long keyOffWrites = 0;
  long mainVolumeWrites = 0;
  long cdVolumeWrites = 0;
  std::uint32_t transferAddress = 0;
  long dmaCalls = 0;
  long dmaWords = 0;
};

// Create and power the process SPU state each SpuDevice binds over.
void init();
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
