// record_raster_harness.h - GP0 stream building, device replay and canvas presentation for the record rasterizer tests.
#pragma once

#include "frame_presenter.h"
#include "frame_state.h"
#include "gpu_device.h"
#include "gpu_vk_record_raster.h"
#include "testutil.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <variant>
#include <vector>

namespace record_raster_harness {

using psx::gpu::GpuDevice;
using psx::gpu::RecordRasterizer;

inline SDL_GPUDevice *gDevice = nullptr;
constexpr psx::gpu::RecordRect kWholeVram{0, 0, psx::gpu::kRecordVramWidth, psx::gpu::kRecordVramHeight};

constexpr std::uint32_t xy(int x, int y) {
  return (static_cast<std::uint32_t>(x) & 0x7FFu) | ((static_cast<std::uint32_t>(y) & 0x7FFu) << 16);
}
constexpr std::uint32_t rgb(std::uint32_t command, int r, int g, int b) {
  return (command << 24) | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(g) << 8) |
         static_cast<std::uint32_t>(r);
}
constexpr std::uint32_t uv(int u, int v, std::uint32_t high) {
  return (high << 16) | (static_cast<std::uint32_t>(v) << 8) | static_cast<std::uint32_t>(u);
}
constexpr std::uint32_t clutAt(int x, int y) {
  return static_cast<std::uint32_t>(x / 16) | (static_cast<std::uint32_t>(y) << 6);
}
// Texpage word: page x (64-pixel units), page y (0/1), semi mode, texture mode.
constexpr std::uint32_t page(int pageX, int pageY, int semi, int mode) {
  return static_cast<std::uint32_t>(pageX) | (static_cast<std::uint32_t>(pageY) << 4) |
         (static_cast<std::uint32_t>(semi) << 5) | (static_cast<std::uint32_t>(mode) << 7);
}
constexpr std::uint32_t drawMode(std::uint32_t pageBits, bool dither, bool flipX, bool flipY) {
  return 0xE1000000u | pageBits | (dither ? 1u << 9 : 0u) | (1u << 10) | (flipX ? 1u << 12 : 0u) |
         (flipY ? 1u << 13 : 0u);
}
constexpr std::uint32_t drawArea(int x0, int y0, int x1, int y1, std::uint32_t command) {
  return (command << 24) | static_cast<std::uint32_t>(x0) | (static_cast<std::uint32_t>(y0) << 10);
}

struct Stream {
  std::vector<std::uint32_t> words;
  void add(std::initializer_list<std::uint32_t> list) {
    words.insert(words.end(), list);
  }
  void area(int x0, int y0, int x1, int y1) {
    add({drawArea(x0, y0, 0, 0, 0xE3), drawArea(x1, y1, 0, 0, 0xE4)});
  }
  void offset(int x, int y) {
    add({0xE5000000u | (static_cast<std::uint32_t>(x) & 0x7FFu) | ((static_cast<std::uint32_t>(y) & 0x7FFu) << 11)});
  }
  void mask(bool set, bool check) {
    add({0xE6000000u | (set ? 1u : 0u) | (check ? 2u : 0u)});
  }
  void upload(int x, int y, int w, int h, std::uint32_t seed) {
    add({0xA0000000u, xy(x, y), static_cast<std::uint32_t>(w) | (static_cast<std::uint32_t>(h) << 16)});
    std::mt19937 random(seed);
    const int count = w * h;
    for (int i = 0; i < count; i += 2) {
      words.push_back(random());
    }
  }
};

inline bool submitAndWait(SDL_GPUCommandBuffer *cmd) {
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (fence == nullptr) {
    return false;
  }
  const bool ok = SDL_WaitForGPUFences(gDevice, true, &fence, 1);
  SDL_ReleaseGPUFence(gDevice, fence);
  return ok;
}

struct Outcome {
  long mismatched = -1;
  int firstX = -1;
  int firstY = -1;
  std::uint16_t image = 0;
  std::uint16_t device = 0;
  bool replayed = false;
  long changed = 0; // pixels the stream changed on the device: the comparison's denominator
  std::size_t entries = 0;
  std::size_t uploads = 0; // upload entries after the pattern: the tap's device-resolved primitives
};

// VRAM starts as a seeded pattern (applied by resync), then `stream` runs and is replayed at `scale`,
// where every pixel of a block must be its native pixel, so only streams without polygons or lines.
inline Outcome replay(const Stream &stream, const std::vector<std::uint32_t> &gp1 = {}, int scale = 1) {
  Outcome out;
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  for (std::uint32_t word : gp1) {
    device.gp1(word, 0);
  }
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  RecordRasterizer rasterizer(gDevice);
  const psx::present::FrameRecord before = device.sealRecord();
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.update(cmd, before, false, device.vram(), scale, {kWholeVram, 0});
  if (!submitAndWait(cmd)) {
    return out;
  }
  const std::vector<std::uint16_t> pristine(device.vram().begin(), device.vram().end());
  std::uint32_t address = 0x80100000u;
  for (std::uint32_t word : stream.words) {
    device.gp0(word, address);
    address += 4;
  }
  const psx::present::FrameRecord record = device.sealRecord();
  cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.update(cmd, record, false, device.vram(), scale, {kWholeVram, 0});
  rasterizer.download(cmd, {rasterizer.image(), kWholeVram});
  if (!submitAndWait(cmd)) {
    return out;
  }
  out.replayed = record.complete() && rasterizer.replayed() == 1 && rasterizer.resynced() == 1;
  const std::vector<std::uint16_t> image = rasterizer.downloaded();
  const std::span<const std::uint16_t> vram = device.vram();
  out.mismatched = 0;
  out.entries = record.entries().size();
  for (const psx::present::RecordEntry &entry : record.entries()) {
    out.uploads += std::holds_alternative<psx::present::VramUpload>(entry) ? 1 : 0;
  }
  for (std::size_t i = 0; i < vram.size(); i++) {
    out.changed += vram[i] != pristine[i] ? 1 : 0;
  }
  const int width = psx::gpu::kRecordVramWidth * scale;
  for (std::size_t i = 0; i < image.size(); i++) {
    const int x = static_cast<int>(i % static_cast<std::size_t>(width));
    const int y = static_cast<int>(i / static_cast<std::size_t>(width));
    const std::uint16_t want =
        vram[static_cast<std::size_t>(y / scale) * psx::gpu::kRecordVramWidth + static_cast<std::size_t>(x / scale)];
    if (image[i] != want && out.mismatched++ == 0) {
      out.firstX = x;
      out.firstY = y;
      out.image = image[i];
      out.device = want;
    }
  }
  fprintf(stderr, "    %zu entries, %ld pixels changed\n", out.entries, out.changed);
  if (out.mismatched != 0) {
    fprintf(stderr,
            "    %ld mismatched, first (%d,%d) record=%04x device=%04x\n",
            out.mismatched,
            out.firstX,
            out.firstY,
            out.image,
            out.device);
  }
  return out;
}

#define CHECK_REPLAY(stream, ...)                                                                                      \
  do {                                                                                                                 \
    const Outcome outcome = replay(stream __VA_OPT__(, ) __VA_ARGS__);                                                 \
    CHECK(outcome.replayed);                                                                                           \
    CHECK(outcome.changed > 0);                                                                                        \
    CHECK_EQ(outcome.mismatched, 0);                                                                                   \
  } while (0)

inline Stream baseState() {
  Stream s;
  s.area(0, 0, 511, 511);
  s.offset(0, 0);
  s.mask(false, false);
  s.add({drawMode(page(8, 0, 0, 2), false, false, false), 0xE2000000u});
  return s;
}

// ---- Display canvases ------------------------------------------------------------------------------

// The display buffer the canvas tests draw into and the margin they widen it by. x and the widened
// extent stay 16-aligned so a reference fill can cover exactly the canvas.
constexpr psx::gpu::RecordRect kBuffer{64, 0, 384, 240};
constexpr int kMargin = 48;

struct CanvasOutcome {
  bool ran = false;
  std::vector<std::uint16_t> canvas; // the presented picture
  std::vector<std::uint16_t> image;  // the VRAM image
  std::vector<std::uint16_t> device; // the device's VRAM after every record
  psx::gpu::RecordPicture picture;
  SDL_GPUTexture *vramImage = nullptr;
};

// The pattern, then each of `records` as its own frame, presenting `view` after each.
inline CanvasOutcome present(const std::vector<Stream> &records, const psx::gpu::RecordView &view) {
  CanvasOutcome out;
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  RecordRasterizer rasterizer(gDevice);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, view);
  for (const Stream &stream : records) {
    for (std::uint32_t word : stream.words) {
      device.gp0(word, 0x80100000u);
    }
    rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, view);
  }
  out.picture = rasterizer.presented();
  out.vramImage = rasterizer.image();
  rasterizer.download(cmd, out.picture);
  if (!submitAndWait(cmd)) {
    return out;
  }
  out.canvas = rasterizer.downloaded();
  cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.download(cmd, {rasterizer.image(), kWholeVram});
  if (!submitAndWait(cmd)) {
    return out;
  }
  out.image = rasterizer.downloaded();
  out.device.assign(device.vram().begin(), device.vram().end());
  out.ran = rasterizer.replayed() == records.size() && rasterizer.resynced() == 1;
  return out;
}

// Pixels of `picture` (width x height) that differ from VRAM rect at (x, y).
long differsFromVram(const std::vector<std::uint16_t> &picture,
                     int width,
                     int height,
                     const std::vector<std::uint16_t> &vram,
                     int x,
                     int y) {
  long differing = 0;
  for (int row = 0; row < height; row++) {
    for (int column = 0; column < width; column++) {
      const std::uint16_t want =
          vram[static_cast<std::size_t>(y + row) * psx::gpu::kRecordVramWidth + static_cast<std::size_t>(x + column)];
      differing += picture[(static_cast<std::size_t>(row) * static_cast<std::size_t>(width)) +
                           static_cast<std::size_t>(column)] != want
                       ? 1
                       : 0;
    }
  }
  return differing;
}

// Primitives crossing both edges of the buffer: flat, gouraud, textured semi, a sprite and a line.
inline void crossingPrimitives(Stream &s) {
  const std::uint32_t texture = page(8, 0, 1, 2);
  s.add({drawMode(texture, true, false, false)});
  s.add({rgb(0x20, 200, 40, 40), xy(10, 20), xy(300, 60), xy(60, 200)});
  s.add({rgb(0x30, 255, 0, 0), xy(200, 30), rgb(0, 0, 255, 0), xy(430, 90), rgb(0, 0, 0, 255), xy(260, 230)});
  s.add({rgb(0x2E, 128, 128, 128),
         xy(30, 100),
         uv(0, 0, 0),
         xy(420, 110),
         uv(255, 0, texture),
         xy(40, 180),
         uv(0, 80, 0),
         xy(410, 200),
         uv(255, 80, 0)});
  s.add({rgb(0x64, 128, 128, 128), xy(20, 140), uv(3, 7, 0), xy(90, 40)});
  s.add({rgb(0x40, 255, 255, 0), xy(300, 10), xy(430, 150)});
}

// ---- In-betweens through FramePresenter -------------------------------------------------------------

constexpr psx::gpu::RecordRect kBufferA{0, 0, 320, 240};
constexpr psx::gpu::RecordRect kBufferB{0, 256, 320, 496};

// One frame into `buffer`: a fill, then a flat triangle `x` pixels into it.
inline Stream bufferFrame(const psx::gpu::RecordRect &buffer, int x) {
  Stream s = baseState();
  s.area(buffer.x0, buffer.y0, buffer.x1 - 1, buffer.y1 - 1);
  s.offset(buffer.x0, buffer.y0);
  s.add({rgb(0x02, 0, 0, 64), xy(buffer.x0, buffer.y0), xy(320, 240)});
  s.add({rgb(0x20, 255, 255, 0), xy(x, 40), xy(x + 60, 60), xy(x + 10, 180)});
  return s;
}

// The device's VRAM after only `stream`.
inline std::vector<std::uint16_t> deviceDrawing(const Stream &stream) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  for (std::uint32_t word : stream.words) {
    device.gp0(word);
  }
  return {device.vram().begin(), device.vram().end()};
}

struct Present {
  int frame = -1; // the frame just committed
  bool inBetween = false;
  bool composed = false; // a composed frame was drawn, at t = 0.5 or t = 1
  std::vector<std::uint16_t> picture;
  std::vector<std::uint16_t> device;
};

// Every present goes through RecordRasterizer::show, as RenderPath::Record does.
class RasterBackend final : public psx::frame::FramePresentationBackend {
public:
  RasterBackend(const psx::frame::FramePresenter &presenter, RecordRasterizer &rasterizer, GpuDevice &device)
      : presenter_(presenter), rasterizer_(rasterizer), device_(device) {}

  void emit(std::span<const RqItem>) override {}
  void presentReal() override {
    show();
  }
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return true;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  psx::gpu::RecordRect displayedBuffer() override {
    return display;
  }
  void presentInBetween() override {
    show();
  }
  const psx::present::StateProducers *stateProducers() override {
    return producers;
  }

  psx::gpu::RecordRect display;
  const psx::present::StateProducers *producers = nullptr;
  int frame = -1;
  std::vector<Present> presents;

private:
  void show() {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
    Present present;
    present.frame = frame;
    present.composed = rasterizer_.show(cmd,
                                        presenter_.currentRecord(),
                                        presenter_.composedRecord(),
                                        presenter_.composedAdvances(),
                                        false,
                                        device_.vram(),
                                        1,
                                        {display, 0});
    present.inBetween = present.composed && !presenter_.composedAdvances();
    rasterizer_.download(cmd, rasterizer_.presented());
    if (!submitAndWait(cmd)) {
      return;
    }
    present.picture = rasterizer_.downloaded();
    present.device.assign(device_.vram().begin(), device_.vram().end());
    presents.push_back(std::move(present));
  }

  const psx::frame::FramePresenter &presenter_;
  RecordRasterizer &rasterizer_;
  GpuDevice &device_;
};

constexpr int kTriangleStep = 40;
constexpr std::uint32_t kTriangleProducer = 0x8001F798u;
constexpr std::uint32_t kTriangleObject = 0x80150000u;

inline int triangleX(int frame) {
  return 20 + kTriangleStep * frame;
}

// bufferFrame's moving triangle drawn from its saved x.
class TriangleRender final : public psx::present::StateProducer {
public:
  void render(std::span<const std::byte> from,
              std::span<const std::byte> to,
              float t,
              psx::present::PrimitiveSink &sink) const override {
    const auto a = psx::present::stateAs<float>(from);
    const auto b = psx::present::stateAs<float>(to);
    const float x = a + (b - a) * t;
    psx::present::DrawPrimitive primitive;
    primitive.vertexCount = 3;
    const float corners[3][2] = {{x, 40.0f}, {x + 60.0f, 60.0f}, {x + 10.0f, 180.0f}};
    for (std::size_t v = 0; v < 3; v++) {
      psx::present::RecordVertex &vertex = primitive.vertices[v];
      psx::present::placeVertex(vertex, corners[v][0], corners[v][1], primitive.kind);
      vertex.r = 255;
      vertex.g = 255;
    }
    sink.emit(psx::present::OtSlot{0, 1}, primitive);
  }
};

// Frames 0..count-1, frame k drawn into draw(k) while display(k) is scanned out at its present.
std::vector<Present> presentFrames(int count,
                                   psx::gpu::RecordRect (*draw)(int),
                                   psx::gpu::RecordRect (*display)(int),
                                   bool emptyAfterEach = false,
                                   const psx::present::StateProducers *renders = nullptr) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  RecordRasterizer rasterizer(gDevice);
  psx::frame::FramePresenter presenter;
  RasterBackend backend(presenter, rasterizer, device);
  backend.producers = renders;
  backend.display = display(0);
  presenter.commit(backend, 2, device.sealRecord());
  backend.presents.clear();
  for (int frame = 0; frame < count; frame++) {
    for (std::uint32_t word : bufferFrame(draw(frame), triangleX(frame)).words) {
      device.gp0(word, 0x80100000u);
    }
    psx::present::FrameRecord record = device.sealRecord();
    const auto serial = static_cast<std::uint32_t>(frame + 1);
    const psx::present::RecordKey key{kTriangleProducer, kTriangleObject, 0, 0, serial};
    for (psx::present::RecordEntry &entry : record.entries()) {
      auto *primitive = std::get_if<psx::present::DrawPrimitive>(&entry);
      // A render owns only the triangle; the background stays as the guest drew it.
      if (primitive != nullptr && (renders == nullptr || primitive->vertexCount == 3)) {
        primitive->key = key;
        primitive->slot = psx::present::OtSlot{0, 1};
      }
    }
    psx::present::FrameStates states;
    states.save(key, static_cast<float>(triangleX(frame)));
    psx::present::FrameState state = states.collect(record);
    backend.display = display(frame);
    backend.frame = frame;
    presenter.commit(backend, 2, std::move(record), std::move(state));
    if (emptyAfterEach) {
      presenter.commit(backend, 2, device.sealRecord());
    }
  }
  return std::move(backend.presents);
}

inline psx::gpu::RecordRect alternating(int frame) {
  return frame % 2 == 0 ? kBufferA : kBufferB;
}
inline psx::gpu::RecordRect previousAlternating(int frame) {
  return alternating(frame + 1);
}
inline psx::gpu::RecordRect single(int) {
  return kBufferA;
}

long differsFromBuffer(const std::vector<std::uint16_t> &picture,
                       const std::vector<std::uint16_t> &vram,
                       const psx::gpu::RecordRect &buffer) {
  return differsFromVram(picture, buffer.x1 - buffer.x0, buffer.y1 - buffer.y0, vram, buffer.x0, buffer.y0);
}

inline long differing(const std::vector<std::uint16_t> &a, const std::vector<std::uint16_t> &b) {
  long count = 0;
  for (std::size_t i = 0; i < a.size() && i < b.size(); i++) {
    count += a[i] != b[i] ? 1 : 0;
  }
  return count;
}

// Every in-between differs from the reals either side, and is exactly the device drawing the shown
// record with the triangle halfway from the record shown before it; every real is the device's buffer.
void checkPresents(const std::vector<Present> &presents,
                   int shownLag,
                   psx::gpu::RecordRect (*draw)(int),
                   psx::gpu::RecordRect (*display)(int)) {
  int inBetweens = 0;
  for (std::size_t i = 0; i < presents.size(); i++) {
    const Present &present = presents[i];
    const psx::gpu::RecordRect shown = display(present.frame);
    if (!present.inBetween) {
      CHECK_EQ(differsFromBuffer(present.picture, present.device, shown), 0);
      continue;
    }
    CHECK(i > 0 && i + 1 < presents.size() && !presents[i - 1].inBetween && !presents[i + 1].inBetween);
    if (i == 0 || i + 1 >= presents.size()) {
      continue;
    }
    const long fromBefore = differing(present.picture, presents[i - 1].picture);
    const long fromAfter = differing(present.picture, presents[i + 1].picture);
    fprintf(stderr,
            "    frame %d in-between: %ld px from the real before, %ld from the real after\n",
            present.frame,
            fromBefore,
            fromAfter);
    CHECK(fromBefore > 0);
    CHECK(fromAfter > 0);
    const int frame = present.frame - shownLag;
    CHECK(draw(frame) == shown);
    const int halfway = (triangleX(frame - 1) + triangleX(frame)) / 2;
    CHECK_EQ(differsFromBuffer(present.picture, deviceDrawing(bufferFrame(shown, halfway)), shown), 0);
    inBetweens++;
  }
  CHECK(inBetweens >= 3);
}

} // namespace record_raster_harness
