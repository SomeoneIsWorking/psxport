// gpu_device.cpp — GpuDevice over the vendored Beetle gpu.c.
#include "gpu_device.h"

#include "gpu_display_mode.h"
#include "vram_pixel.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "beetle_psx_globals.h"
#include "mednafen/psx/gpu.h"
#include "mednafen/psx/psxport_gpu_census.h"
#include "mednafen/psx/timer.h"
#include "mednafen/settings.h"
#include "pgxp/pgxp_gpu.h"
#include "pgxp/pgxp_mem.h"

// gpu.c's link-time knobs and machine hooks. gMode, widescreen_hack and its aspect setting are owned
// by gte_beetle.cpp, EventCycles by mdec_beetle.c, IRQ_Assert by spu_beetle.cpp.
extern "C" {
extern PS_GPU GPU;

uint8_t psx_gpu_upscale_shift = 0;
uint8_t psx_gpu_upscale_shift_hw = 0;
enum dither_mode psx_gpu_dither_mode = DITHER_NATIVE;
bool psx_gpu_rasterize_both_fields = false;
int line_render_mode = 0;
int filter_mode = 0;
int crop_overscan = 0;
bool is_monkey_hero = false;
int psx_pgxp_2d_tol = -1;
unsigned psx_gpu_overclock_shift = 0;
int32_t psx_overclock_factor = 0;
bool fast_pal = false;
bool content_is_pal = false;
bool currently_interlaced = false;
bool aspect_ratio_dirty = false;
bool interlace_setting_dirty = false;
int aspect_ratio_setting = 0;
enum core_timing_fps_modes core_timing_fps_mode = FORCE_PROGRESSIVE_TIMING;
unsigned image_height = 240;
uint8_t startup_frame_count = 0;
void *video_cb = nullptr;
struct FrontIO *PSX_FIO = nullptr;

// Root counters and the frame loop belong to the framework (psx::frame::Timing), not to gpu.c.
void MDFN_FASTCALL TIMER_SetVBlank(bool) {}
void MDFN_FASTCALL TIMER_SetHRetrace(bool) {}
void MDFN_FASTCALL TIMER_AddDotClocks(uint32_t) {}
void TIMER_ClockHRetrace(void) {}
int32_t MDFN_FASTCALL TIMER_Update(const int32_t) {
  return 0x7FFFFFFF;
}
void PSX_RequestMLExit(void) {
  psx::gpu::GpuDevice::noteFieldEnd();
}
void PSX_SetEventNT(const int, const int32_t) {}
void FrontIO_GPULineHook(struct FrontIO *,
                         const int32_t,
                         const int32_t,
                         bool,
                         uint32_t *,
                         const unsigned,
                         const unsigned,
                         const unsigned,
                         const unsigned,
                         const unsigned,
                         const unsigned) {}
int64_t MDFN_GetSettingI(const char *) {
  return 0;
}

// PGXP is off: the device answers with the integer hardware result.
PGXP_value *ReadMem(uint32_t) {
  return nullptr;
}
void PGXP_WriteFIFO(PGXP_value *, uint32_t) {}
PGXP_value *PGXP_ReadFIFO(uint32_t) {
  return nullptr;
}
void PGXP_WriteCB(PGXP_value *, uint32_t) {}
int PGXP_GetVertex(const uint32_t, const uint32_t *, OGLVertex *, int, int) {
  return 0;
}
}

namespace psx::gpu {
namespace {

constexpr uint32_t kGp0Port = 0x1F801810u;
constexpr uint32_t kGp1Port = 0x1F801814u;
constexpr int kScanoutWidth = 1024;
constexpr int kScanoutHeight = 512;
// Less than one scanline of CPU clock, so a field end is always seen before the next line 0.
constexpr uint64_t kClockStepTicks = 2048;

std::vector<uint8_t> captureBeetleState() {
  StateMem stream{};
  if (GPU_StateAction(&stream, 0, 0) == 0 || stream.len == 0) {
    std::free(stream.data);
    lucent::error("gpu-device", "Beetle GPU_StateAction refused to save");
    std::abort();
  }
  std::vector<uint8_t> bytes(stream.data, stream.data + stream.len);
  std::free(stream.data);
  return bytes;
}

bool restoreBeetleState(std::span<const uint8_t> bytes) {
  // The load path only reads through the stream.
  StateMem stream{};
  stream.data = const_cast<uint8_t *>(bytes.data());
  stream.len = static_cast<uint32_t>(bytes.size());
  return GPU_StateAction(&stream, 1, 0) != 0;
}

} // namespace

// gpu.c's scanline walk writes every displayed line into an emulator surface; nothing reads it.
struct GpuDevice::Scanout {
  Scanout() : surface(MDFN_Surface_New(kScanoutWidth, kScanoutHeight, kScanoutWidth)) {
    if (surface == nullptr) {
      lucent::error("gpu-device", "scanout surface allocation failed");
      std::abort();
    }
    spec.surface = surface;
    spec.LineWidths = lineWidths;
    spec.DisplayRect = MDFN_Rect{0, 0, kScanoutWidth, kScanoutHeight};
  }
  ~Scanout() {
    MDFN_Surface_Delete(surface);
  }
  Scanout(const Scanout &) = delete;
  Scanout &operator=(const Scanout &) = delete;

  MDFN_Surface *surface;
  EmulateSpecStruct spec{};
  int32_t lineWidths[kScanoutHeight] = {};
};

GpuDevice::GpuDevice() : mScanout(std::make_unique<Scanout>()) {
  if (sLive == 0 && !GPU_Init(/*pal_clock_and_tv=*/false, /*sls=*/0, /*sle=*/239, /*upscale_shift=*/0)) {
    lucent::error("gpu-device", "Beetle GPU_Init failed");
    std::abort();
  }
  ++sLive;
}

GpuDevice::~GpuDevice() {
  if (sBound == this) {
    sBound = nullptr;
  }
  if (--sLive == 0) {
    GPU_Destroy();
  }
}

void GpuDevice::noteFieldEnd() {
  if (sBound != nullptr) {
    sBound->mFieldEnded = true;
  }
}

void GpuDevice::bind() {
  if (sBound == this) {
    return;
  }
  if (sBound != nullptr) {
    sBound->mParked = captureBeetleState();
  }
  if (!mPowered) {
    GPU_Power();
    mPowered = true;
  } else if (!restoreBeetleState(mParked)) {
    lucent::error("gpu-device", "Beetle GPU_StateAction could not restore a parked device");
    std::abort();
  }
  mParked.clear();
  mParked.shrink_to_fit();
  GPU_StartFrame(&mScanout->spec);
  mFieldEnded = false;
  sBound = this;
}

void GpuDevice::advanceTo(uint64_t cpuTicks) {
  bind();
  if (!mClockStarted || cpuTicks < mClockTicks) {
    mClockStarted = true;
    mClockTicks = cpuTicks;
  }
  while (mClockTicks < cpuTicks) {
    const uint64_t step = std::min(cpuTicks - mClockTicks, kClockStepTicks);
    GPU_ResetTS();
    GPU_Update(static_cast<int32_t>(step));
    mClockTicks += step;
    if (mFieldEnded) {
      GPU_StartFrame(&mScanout->spec);
      mFieldEnded = false;
    }
  }
  // Drawing is not timed: every command completes when its last word arrives.
  psxport_gpu_grant_drawtime();
}

DeviceProbe GpuDevice::probe(unsigned long dispatchedBefore, unsigned long droppedBefore) const {
  DeviceProbe probe;
  switch (GPU.InCmd) {
  case INCMD_PLINE:
    probe.command = DeviceCommandState::PolyLine;
    break;
  case INCMD_QUAD:
    probe.command = DeviceCommandState::Quad;
    break;
  case INCMD_FBWRITE:
    probe.command = DeviceCommandState::Upload;
    break;
  case INCMD_FBREAD:
    probe.command = DeviceCommandState::Read;
    break;
  default:
    probe.command = DeviceCommandState::None;
    break;
  }
  probe.fifoDepth = static_cast<unsigned>(psxport_gpu_fifo_depth());
  probe.dispatched = static_cast<unsigned>(psxport_gpu_census[PGC_CMDS_DISPATCHED] - dispatchedBefore);
  probe.wordDropped = psxport_gpu_census[PGC_WORDS_DROPPED] != droppedBefore;
  present::RecordDrawState &state = probe.state;
  state.clipX0 = GPU.ClipX0;
  state.clipY0 = GPU.ClipY0;
  state.clipX1 = GPU.ClipX1;
  state.clipY1 = GPU.ClipY1;
  state.offsetX = GPU.OffsX;
  state.offsetY = GPU.OffsY;
  state.texPageX = static_cast<int>(GPU.TexPageX);
  state.texPageY = static_cast<int>(GPU.TexPageY);
  state.texMode = std::min(static_cast<int>(GPU.TexMode), 2);
  state.blendMode = static_cast<int>(GPU.abr);
  state.windowMaskX = GPU.tww;
  state.windowMaskY = GPU.twh;
  state.windowOffsetX = GPU.twx;
  state.windowOffsetY = GPU.twy;
  state.dither = GPU.dtd && psx_gpu_dither_mode != DITHER_OFF;
  state.maskSet = GPU.MaskSetOR != 0;
  state.maskCheck = GPU.MaskEvalAND != 0;
  // gpu.c LineSkipTest.
  if ((GPU.DisplayMode & 0x24) == 0x24 && !GPU.dfe && !psx_gpu_rasterize_both_fields) {
    state.skipRowParity = static_cast<int>((GPU.DisplayFB_YStart + (GPU.field_ram_readout ? 1u : 0u)) & 1u);
  }
  probe.texDisable = GPU.TexDisable;
  probe.spriteFlipX = (GPU.SpriteFlip & 0x1000) != 0;
  probe.spriteFlipY = (GPU.SpriteFlip & 0x2000) != 0;
  probe.clutTag = GPU.CLUT_Cache_VB;
  probe.clut = std::span<const uint16_t>(GPU.CLUT_Cache, 256);
  probe.vram = std::span<const uint16_t>(GPU_get_vram(), static_cast<size_t>(kDeviceVramWidth) * kDeviceVramHeight);
  return probe;
}

void GpuDevice::beginPacket() {
  mTap.beginPacket();
}

void GpuDevice::enterSlot(const std::optional<present::OtSlot> &slot, bool descending) {
  mTap.enterSlot(slot, descending);
}

void GpuDevice::gp0(uint32_t word, uint32_t sourceAddress, const std::optional<present::RecordKey> &packetKey) {
  bind();
  psxport_gpu_grant_drawtime();
  const unsigned long dispatched = psxport_gpu_census[PGC_CMDS_DISPATCHED];
  const unsigned long dropped = psxport_gpu_census[PGC_WORDS_DROPPED];
  GPU_Write(0, kGp0Port, word);
  mTap.onGp0(word, sourceAddress, packetKey, probe(dispatched, dropped));
}

void GpuDevice::gp1(uint32_t word, uint64_t cpuTicks) {
  bind();
  const unsigned long dispatched = psxport_gpu_census[PGC_CMDS_DISPATCHED];
  const unsigned long dropped = psxport_gpu_census[PGC_WORDS_DROPPED];
  advanceTo(cpuTicks);
  GPU_Write(0, kGp1Port, word);
  const uint32_t command = word >> 24;
  if (command == 0x00) {
    mTap.onSoftReset(probe(dispatched, dropped));
  }
  if (command == 0x00 || command == 0x01) {
    mTap.onCommandReset(probe(dispatched, dropped));
  } else {
    mTap.onDeviceStep(probe(dispatched, dropped));
  }
}

uint32_t GpuDevice::read() {
  bind();
  const unsigned long dispatched = psxport_gpu_census[PGC_CMDS_DISPATCHED];
  const unsigned long dropped = psxport_gpu_census[PGC_WORDS_DROPPED];
  const uint32_t value = GPU_Read(0, kGp0Port);
  mTap.onDeviceStep(probe(dispatched, dropped));
  return value;
}

uint32_t GpuDevice::status(uint64_t cpuTicks) {
  bind();
  const unsigned long dispatched = psxport_gpu_census[PGC_CMDS_DISPATCHED];
  const unsigned long dropped = psxport_gpu_census[PGC_WORDS_DROPPED];
  advanceTo(cpuTicks);
  const uint32_t value = GPU_Read(0, kGp1Port);
  mTap.onDeviceStep(probe(dispatched, dropped));
  return value;
}

present::FrameRecord GpuDevice::sealRecord() {
  return mTap.seal();
}

bool GpuDevice::hasUnsealedWork() const {
  return mTap.hasPendingWork();
}

void GpuDevice::loadImage(int x, int y, int width, int height, std::span<const uint16_t> pixels) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (pixels.size() < count) {
    lucent::error("gpu-device", "loadImage {}x{} given {} pixels", width, height, pixels.size());
    std::abort();
  }
  gp0(0xA0000000u);
  gp0((static_cast<uint32_t>(y & 0x1FF) << 16) | static_cast<uint32_t>(x & 0x3FF));
  gp0((static_cast<uint32_t>(height) << 16) | static_cast<uint32_t>(width));
  for (size_t i = 0; i < count; i += 2) {
    const uint32_t low = pixels[i];
    const uint32_t high = i + 1 < count ? pixels[i + 1] : 0u; // an odd count pads the last word
    gp0(low | (high << 16));
  }
}

std::span<const uint16_t> GpuDevice::vram() {
  bind();
  return {GPU_get_vram(), static_cast<size_t>(kDeviceVramWidth) * kDeviceVramHeight};
}

DisplayArea GpuDevice::displayArea() {
  bind();
  DisplayArea area;
  area.x = static_cast<int>(GPU.DisplayFB_XStart);
  area.y = static_cast<int>(GPU.DisplayFB_YStart);
  area.width = gp1_display_width(GPU.DisplayMode);
  const int lines = static_cast<int>(GPU.VertEnd) - static_cast<int>(GPU.VertStart);
  const bool interlaced480 = (GPU.DisplayMode & (DISP_INTERLACED | DISP_VERT480)) == (DISP_INTERLACED | DISP_VERT480);
  area.height = std::max(lines, 0) * (interlaced480 ? 2 : 1);
  area.depth24 = (GPU.DisplayMode & DISP_RGB24) != 0;
  return area;
}

VramPoint GpuDevice::drawAreaOrigin() {
  bind();
  return {GPU.ClipX0, GPU.ClipY0};
}

Rgb24Picture GpuDevice::picture(int x, int y, int width, int height, bool depth24) {
  bind();
  const uint16_t *vram = GPU_get_vram();
  const auto at = [vram](int column, int row) {
    return vram[static_cast<size_t>(row & (kDeviceVramHeight - 1)) * kDeviceVramWidth +
                static_cast<size_t>(column & (kDeviceVramWidth - 1))];
  };
  Rgb24Picture out{
      width,
      height,
      std::vector<uint8_t>(static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)) * 3u)};
  uint8_t *dst = out.pixels.data();
  for (int row = 0; row < height; row++) {
    for (int column = 0; column < width; column++) {
      if (depth24) {
        // Three bytes per pixel, packed across the halfwords of the row.
        for (int channel = 0; channel < 3; channel++) {
          const int byteIndex = column * 3 + channel;
          const uint16_t half = at(x + byteIndex / 2, y + row);
          *dst++ = static_cast<uint8_t>((byteIndex & 1) ? (half >> 8) : (half & 0xFF));
        }
      } else {
        const VramPixel pixel = decodeVramPixel(at(x + column, y + row));
        *dst++ = static_cast<uint8_t>(scale5to8(pixel.red));
        *dst++ = static_cast<uint8_t>(scale5to8(pixel.green));
        *dst++ = static_cast<uint8_t>(scale5to8(pixel.blue));
      }
    }
  }
  return out;
}

std::vector<uint8_t> GpuDevice::saveState() {
  bind();
  return captureBeetleState();
}

bool GpuDevice::loadState(std::span<const uint8_t> bytes) {
  bind();
  if (!restoreBeetleState(bytes)) {
    return false;
  }
  GPU_StartFrame(&mScanout->spec);
  mFieldEnded = false;
  mClockStarted = false; // the restored machine's clock anchors at its next access
  mTap.onStateReplaced(probe(psxport_gpu_census[PGC_CMDS_DISPATCHED], psxport_gpu_census[PGC_WORDS_DROPPED]));
  return true;
}

} // namespace psx::gpu
