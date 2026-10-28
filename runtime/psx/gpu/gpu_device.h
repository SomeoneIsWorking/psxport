// gpu_device.h — the PSX GPU device: Beetle's gpu.c, the only VRAM and register file the guest sees.
#pragma once

#include "emulated_time.h"
#include "gp0_record_tap.h"
#include "rgb24_picture.h"
#include "vram_rect.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace psx::gpu {

// The VRAM rectangle the display scans out, as GP1(05/07/08) programmed it.
struct DisplayArea {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool depth24 = false;
  bool pal = false;     // GP1(08) video mode: 50 Hz fields
  bool enabled = false; // GP1(03) display enable: off scans out black
};

// One machine's GPU. gpu.c keeps its state in globals, so one device is bound to them at a time:
// every entry point binds its own device first, parking the previously bound one's state with it.
class GpuDevice {
public:
  GpuDevice();
  ~GpuDevice();
  GpuDevice(const GpuDevice &) = delete;
  GpuDevice &operator=(const GpuDevice &) = delete;

  void gp0(uint32_t word, const Gp0Origin &origin = {});
  // The next GP0 words come from a new OT node.
  void beginPacket();
  // Draw work enters the GPU in display field `field` (an OT walk, a block DMA, a GP0 port write).
  void noteDrawEntry(std::uint32_t field);
  // The guest filled a page from display pixels it read back; only the record sees this (record_raster_setup.h).
  void declareReadBack(const present::ReadBackTexture &texture) {
    mTap.declareReadBack(texture);
  }
  // The OT walk reached a named bucket, or left the named tables (nullopt).
  void enterSlot(const std::optional<present::OtSlot> &slot, bool descending);
  // The device's scanline clock runs from `now`.
  void gp1(uint32_t word, frame::CpuTicks now);
  uint32_t read();
  uint32_t status(frame::CpuTicks now);

  // A CPU->VRAM upload the guest's libgpu would have sent as GP0(A0) (an HLE'd LoadImage).
  // `owner` is the OwnerLedger handle of the code uploading (a LoadImage port writer).
  void loadImage(VramRect rect, std::span<const uint16_t> pixels, std::uint32_t owner);

  // 1024x512 1555 halfwords, row-major.
  std::span<const uint16_t> vram();
  DisplayArea displayArea();
  // Top-left of the GP0(E3) drawing area: where the frame being drawn lands.
  VramPoint drawAreaOrigin();
  // A VRAM rectangle as 8-bit RGB, read as 15-bit pixels or as 24-bit display data.
  Rgb24Picture picture(VramRect rect, bool depth24);

  // Beetle's own state stream for this device.
  std::vector<uint8_t> saveState();
  bool loadState(std::span<const uint8_t> bytes);

  // The GP0 work executed since the previous call, in execution order, into `into`, whose storage the next
  // record reuses.
  void sealRecord(present::FrameRecord &into);
  // True when GP0 work executed since the last sealRecord().
  bool hasUnsealedWork() const;

  // gpu.c's frame-end callback (PSX_RequestMLExit) for the bound device.
  static void noteFieldEnd();

private:
  struct Scanout;

  void bind();
  // gpu.c's census counters before a step; probe() reports the step's delta from them.
  struct CensusMark {
    unsigned long dispatched = 0;
    unsigned long dropped = 0;
  };
  static CensusMark censusMark();
  void advanceTo(frame::CpuTicks now);
  DeviceProbe probe(CensusMark before) const;
  // The draw environment a primitive submitted now is drawn under.
  present::RecordDrawState drawState() const;

  std::unique_ptr<Scanout> mScanout;
  std::vector<uint8_t> mParked; // this device's state while another device is bound
  bool mPowered = false;
  bool mClockStarted = false;
  uint64_t mClockTicks = 0; // CPU tick the scanline clock has reached
  bool mFieldEnded = false;
  Gp0RecordTap mTap;

  static inline GpuDevice *sBound = nullptr;
  static inline int sLive = 0;
};

} // namespace psx::gpu
