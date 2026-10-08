// gpu_device.h — the PSX GPU device: Beetle's gpu.c, the only VRAM and register file the guest sees.
#pragma once

#include "gp0_record_tap.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace psx::gpu {

inline constexpr int kDeviceVramWidth = 1024;
inline constexpr int kDeviceVramHeight = 512;

// The VRAM rectangle the display scans out, as GP1(05/07/08) programmed it.
struct DisplayArea {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool depth24 = false;
};

struct VramPoint {
  int x = 0;
  int y = 0;
};

struct Rgb24Picture {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels; // row-major RGB
};

// One machine's GPU. gpu.c keeps its state in globals, so one device is bound to them at a time:
// every entry point binds its own device first, parking the previously bound one's state with it.
class GpuDevice {
public:
  GpuDevice();
  ~GpuDevice();
  GpuDevice(const GpuDevice &) = delete;
  GpuDevice &operator=(const GpuDevice &) = delete;

  // `sourceAddress` is the guest address the word was read from, 0 when it came from no packet;
  // `packetKey` the emission key of the OT node it belongs to.
  void gp0(uint32_t word, uint32_t sourceAddress = 0, const std::optional<present::RecordKey> &packetKey = {});
  // The next GP0 words come from a new OT node.
  void beginPacket();
  // The OT walk reached a named bucket, or left the named tables (nullopt).
  void enterSlot(const std::optional<present::OtSlot> &slot, bool descending);
  // `cpuTicks` is the machine's emulated CPU clock; the device's scanline clock runs from it.
  void gp1(uint32_t word, uint64_t cpuTicks);
  uint32_t read();
  uint32_t status(uint64_t cpuTicks);

  // A CPU->VRAM upload the guest's libgpu would have sent as GP0(A0) (an HLE'd LoadImage).
  void loadImage(int x, int y, int width, int height, std::span<const uint16_t> pixels);

  // 1024x512 1555 halfwords, row-major.
  std::span<const uint16_t> vram();
  DisplayArea displayArea();
  // Top-left of the GP0(E3) drawing area: where the frame being drawn lands.
  VramPoint drawAreaOrigin();
  // A VRAM rectangle as 8-bit RGB, read as 15-bit pixels or as 24-bit display data.
  Rgb24Picture picture(int x, int y, int width, int height, bool depth24);

  // Beetle's own state stream for this device.
  std::vector<uint8_t> saveState();
  bool loadState(std::span<const uint8_t> bytes);

  // The GP0 work executed since the previous call, in execution order.
  present::FrameRecord sealRecord();
  // True when GP0 work executed since the last sealRecord().
  bool hasUnsealedWork() const;

  // gpu.c's frame-end callback (PSX_RequestMLExit) for the bound device.
  static void noteFieldEnd();

private:
  struct Scanout;

  void bind();
  void advanceTo(uint64_t cpuTicks);
  DeviceProbe probe(unsigned long dispatchedBefore, unsigned long droppedBefore) const;

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
