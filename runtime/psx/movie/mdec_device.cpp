#include "mdec_device.h"

// mdec.c clamps its clock to the scheduler's event horizon; with no scheduler here the FIFO gates bound the run.
extern "C" int32_t EventCycles = 0x7FFFFFFF;

// MDEC_DMAWrite drops a word on a full InFIFO, which DMA0 never lets happen. An output-blocked decoder
// cannot retire its macroblock until DMA1 drains, so the feed stops there.
int MdecDevice::dmaIn(const uint32_t *words, int count) {
  int i = 0;
  for (; i < count; i++) {
    if (!canWrite()) {
      if (!canRead()) {
        step();
      }
      if (!canWrite()) {
        return i;
      }
    }
    MDEC_DMAWrite(words[i]);
  }
  return i;
}

// Word i lands at buf[i + offs]; the lower half of each block row has a negative offs (MDEC_DMARead).
int MdecDevice::dmaOut(uint32_t *buf, int count) {
  constexpr int kStepClocks = 256;
  constexpr int kMaxSteps = 4096;
  for (int i = 0; i < count; i++) {
    int steps = 0;
    while (!canRead()) {
      if (++steps > kMaxSteps) {
        return i;
      }
      MDEC_Run(kStepClocks);
    }
    uint32_t offs = 0;
    const uint32_t v = MDEC_DMARead(&offs);
    buf[i + static_cast<int32_t>(offs)] = v;
  }
  return count;
}

int MdecDevice::dmaOutRest(uint32_t *buf, int count) {
  constexpr uint32_t kOutFifoEmpty = 0x80000000u;
  int i = 0;
  for (; i < count; i++) {
    if ((read(kControlPort) & kOutFifoEmpty) != 0) {
      break;
    }
    uint32_t offs = 0;
    const uint32_t v = MDEC_DMARead(&offs);
    buf[i + static_cast<int32_t>(offs)] = v;
  }
  return i;
}
