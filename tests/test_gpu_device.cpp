// test_gpu_device — the GPU device the guest observes: VRAM written by GP0 fill and copy, read back
// through GP0(C0) + GPUREAD, and GPUSTAT after a GP1 reset.

#include "gpu_device.h"
#include "testutil.h"

#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t xy(int x, int y) {
  return static_cast<uint32_t>(x & 0x3FF) | (static_cast<uint32_t>(y & 0x1FF) << 16);
}

uint16_t pixel(psx::gpu::GpuDevice &device, int x, int y) {
  return device.vram()[static_cast<size_t>(y) * psx::gpu::kDeviceVramWidth + static_cast<size_t>(x)];
}

// R=0xF8 G=0x80 B=0x08 as 1555: red 31, green 16, blue 1.
constexpr uint32_t kFillCommand = 0x020880F8u;
constexpr uint16_t kFillPixel = 31u | (16u << 5) | (1u << 10);

} // namespace

// Fill a rect, copy it elsewhere with GP0(80), and read the copy back through GP0(C0).
static void test_fill_copy_readback_roundtrip(void) {
  psx::gpu::GpuDevice device;
  device.gp1(0x00000000u, 0);
  const int fillX = 64, fillY = 16, width = 16, height = 4;
  device.gp0(kFillCommand);
  device.gp0(xy(fillX, fillY));
  device.gp0(xy(width, height));

  int filled = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      filled += pixel(device, fillX + x, fillY + y) == kFillPixel ? 1 : 0;
    }
  }
  CHECK_EQ(filled, width * height);
  CHECK_EQ(pixel(device, fillX + width, fillY), 0); // the fill stops at its width

  const int copyX = 300, copyY = 200;
  device.gp0(0x80000000u);
  device.gp0(xy(fillX, fillY));
  device.gp0(xy(copyX, copyY));
  device.gp0(xy(width, height));
  int copied = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      copied += pixel(device, copyX + x, copyY + y) == kFillPixel ? 1 : 0;
    }
  }
  CHECK_EQ(copied, width * height);

  device.gp0(0xC0000000u);
  device.gp0(xy(copyX, copyY));
  device.gp0(xy(width, height));
  const uint32_t expected = static_cast<uint32_t>(kFillPixel) | (static_cast<uint32_t>(kFillPixel) << 16);
  int words = 0;
  for (int i = 0; i < width * height / 2; i++) {
    words += device.read() == expected ? 1 : 0;
  }
  CHECK_EQ(words, width * height / 2);
}

// An HLE'd LoadImage lands where GP0(A0) would put it.
static void test_load_image(void) {
  psx::gpu::GpuDevice device;
  device.gp1(0x00000000u, 0);
  const std::vector<uint16_t> pixels = {0x1111u, 0x2222u, 0x3333u, 0x4444u, 0x5555u, 0x6666u};
  device.loadImage(900, 500, 3, 2, pixels);
  int matched = 0;
  for (int i = 0; i < 6; i++) {
    matched += pixel(device, 900 + i % 3, 500 + i / 3) == pixels[static_cast<size_t>(i)] ? 1 : 0;
  }
  CHECK_EQ(matched, 6);
}

// After GP1(00): display off, field bit set, idle, ready for commands and DMA blocks; bit 31 is the
// scanline's odd/even readout, which depends on where the beam is.
static void test_status_after_reset(void) {
  psx::gpu::GpuDevice device;
  device.gp1(0x00000000u, 0);
  CHECK_EQ(device.status(0) & 0x7FFFFFFFu, 0x14802000u);
  device.gp1(0x03000000u, 0); // display on
  CHECK_EQ(device.status(0) & (1u << 23), 0u);
}

// Two devices keep separate VRAM while sharing gpu.c's globals.
static void test_two_devices_are_independent(void) {
  psx::gpu::GpuDevice first;
  psx::gpu::GpuDevice second;
  first.gp1(0x00000000u, 0);
  second.gp1(0x00000000u, 0);
  first.gp0(kFillCommand);
  first.gp0(xy(0, 0));
  first.gp0(xy(16, 1));
  CHECK_EQ(pixel(second, 0, 0), 0);
  CHECK_EQ(pixel(first, 0, 0), kFillPixel);
}

int main(void) {
  RUN(fill_copy_readback_roundtrip);
  RUN(load_image);
  RUN(status_after_reset);
  RUN(two_devices_are_independent);
  return pt_summary();
}
