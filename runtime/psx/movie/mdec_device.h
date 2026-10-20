// class MdecDevice: one Game's Beetle MDEC state and the guest-facing MDEC port, DMA0 and DMA1 over it. Every
// member acts on the bound instance.
#pragma once
#include "beetle_device_instance.h"

#include <cstdint>

// Beetle mdec.c (compiled as C).
extern "C" {
void *MDEC_NewState(void);
void MDEC_FreeState(void *p);
void MDEC_BindState(void *p);
void MDEC_Power(void);
void MDEC_Write(const int32_t timestamp, uint32_t A, uint32_t V);
uint32_t MDEC_Read(const int32_t timestamp, uint32_t A);
void MDEC_DMAWrite(uint32_t V);
uint32_t MDEC_DMARead(uint32_t *offs);
bool MDEC_DMACanWrite(void);
bool MDEC_DMACanRead(void);
void MDEC_Run(int32_t clocks);
}

class MdecDevice
    : public psx::platform::BeetleDeviceInstance<MDEC_NewState, MDEC_FreeState, MDEC_BindState, MDEC_Power> {
public:
  // MDEC0 0x1F801820 (command/data) and MDEC1 0x1F801824 (control/status); Beetle decodes only bit 2.
  static constexpr uint32_t kDataPort = 0x1F801820u;
  static constexpr uint32_t kControlPort = 0x1F801824u;

  void write(uint32_t addr, uint32_t value) {
    MDEC_Write(0, addr, value);
  }
  uint32_t read(uint32_t addr) {
    return MDEC_Read(0, addr);
  }
  // Run the decoder until it parks at a FIFO gate.
  void step() {
    MDEC_Run(kRunToParkClocks);
  }
  bool canWrite() const {
    return MDEC_DMACanWrite();
  }
  bool canRead() const {
    return MDEC_DMACanRead();
  }
  // DMA0: feed up to `count` words; returns the words fed, short when the decoder cannot take more.
  int dmaIn(const uint32_t *words, int count);
  // DMA1: drain up to `count` words, each placed at its macroblock-interleave position in `buf` (dma.c CH_MDEC_OUT).
  int dmaOut(uint32_t *buf, int count);
  // The sub-burst tail DMA1 will not release, drained with the same placement; `buf` already advanced.
  int dmaOutRest(uint32_t *buf, int count);
  // One OutFIFO word for a caller that walks guest RAM itself; `*offs` is its signed word displacement.
  uint32_t dmaReadWord(uint32_t *offs) {
    return MDEC_DMARead(offs);
  }

private:
  // EventCycles is the only ceiling, so this always reaches the next block point.
  static constexpr int32_t kRunToParkClocks = 0x40000000;
};
