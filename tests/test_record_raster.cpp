// test_record_raster — GP0 streams executed by the GPU device, then replayed from its FrameRecord by
// RecordRasterizer on a headless Vulkan device; all of VRAM must match pixel for pixel (at S > 1, block for pixel).
//
// Bounds the streams respect, each a gpu.c behaviour the rasterizer does not reproduce:
//   - gpu.c's texture cache is invalidated only by copy, upload, read and texpage changes, so a draw
//     or fill into a texture page can be sampled stale. Draws and fills stay at x < 512; textures and
//     CLUTs sampled by draws come from x >= 512.
//   - A draw area below VRAM row 511 is clipped at 511; gpu.c wraps it. Draw areas stay within 511.
//   - Texel rows are fetched & 511; gpu.c reads past VRAM. Texture pages stay within VRAM.

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

namespace {

using psx::gpu::GpuDevice;
using psx::gpu::RecordRasterizer;

SDL_GPUDevice *gDevice = nullptr;
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

bool submitAndWait(SDL_GPUCommandBuffer *cmd) {
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
Outcome replay(const Stream &stream, const std::vector<std::uint32_t> &gp1 = {}, int scale = 1) {
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

Stream baseState() {
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
CanvasOutcome present(const std::vector<Stream> &records, const psx::gpu::RecordView &view) {
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
void crossingPrimitives(Stream &s) {
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
Stream bufferFrame(const psx::gpu::RecordRect &buffer, int x) {
  Stream s = baseState();
  s.area(buffer.x0, buffer.y0, buffer.x1 - 1, buffer.y1 - 1);
  s.offset(buffer.x0, buffer.y0);
  s.add({rgb(0x02, 0, 0, 64), xy(buffer.x0, buffer.y0), xy(320, 240)});
  s.add({rgb(0x20, 255, 255, 0), xy(x, 40), xy(x + 60, 60), xy(x + 10, 180)});
  return s;
}

// The device's VRAM after only `stream`.
std::vector<std::uint16_t> deviceDrawing(const Stream &stream) {
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

int triangleX(int frame) {
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

psx::gpu::RecordRect alternating(int frame) {
  return frame % 2 == 0 ? kBufferA : kBufferB;
}
psx::gpu::RecordRect previousAlternating(int frame) {
  return alternating(frame + 1);
}
psx::gpu::RecordRect single(int) {
  return kBufferA;
}

long differsFromBuffer(const std::vector<std::uint16_t> &picture,
                       const std::vector<std::uint16_t> &vram,
                       const psx::gpu::RecordRect &buffer) {
  return differsFromVram(picture, buffer.x1 - buffer.x0, buffer.y1 - buffer.y0, vram, buffer.x0, buffer.y0);
}

long differing(const std::vector<std::uint16_t> &a, const std::vector<std::uint16_t> &b) {
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

} // namespace

// Frame k draws one buffer while the other, drawn by k-1, is displayed.
static void test_a_double_buffer_in_between_sits_between_the_shown_pictures(void) {
  const std::vector<Present> presents = presentFrames(6, alternating, previousAlternating);
  CHECK_EQ(presents.size(), 2u + 2u * 4u);
  checkPresents(presents, 1, alternating, previousAlternating);
}

// Spyro 2's cadence: each walk is followed by a record that drew nothing, which changes no pixel.
static void test_an_empty_record_after_the_shown_one_keeps_its_in_between(void) {
  const std::vector<Present> presents = presentFrames(6, alternating, previousAlternating, true);
  CHECK_EQ(presents.size(), 4u + 3u * 4u);
  checkPresents(presents, 1, alternating, previousAlternating);
}

// Every present is the render at its t: at t = 1 exactly the device's buffer.
static void test_a_rendered_object_draws_every_present_from_its_state(void) {
  psx::present::StateProducers renders;
  renders.install(kTriangleProducer, std::make_unique<TriangleRender>());
  const std::vector<Present> doubled = presentFrames(6, alternating, previousAlternating, false, &renders);
  CHECK_EQ(doubled.size(), 2u + 2u * 4u);
  for (std::size_t i = 1; i < doubled.size(); i++) {
    CHECK(doubled[i].composed);
  }
  checkPresents(doubled, 1, alternating, previousAlternating);
  const std::vector<Present> single = presentFrames(6, ::single, ::single, false, &renders);
  checkPresents(single, 0, ::single, ::single);
}

static void test_a_single_buffer_in_between_blends_n_minus_one_and_n(void) {
  const std::vector<Present> presents = presentFrames(6, single, single);
  CHECK_EQ(presents.size(), 1u + 2u * 5u);
  checkPresents(presents, 0, single, single);
}

static void test_four_three_presents_the_device_display(void) {
  Stream s = baseState();
  s.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  crossingPrimitives(s);
  const CanvasOutcome out = present({s}, {kBuffer, 0});
  CHECK(out.ran);
  CHECK(out.picture.texture == out.vramImage);
  CHECK(out.picture.rect == kBuffer);
  CHECK_EQ(differsFromVram(out.canvas, 320, 240, out.device, kBuffer.x0, kBuffer.y0), 0);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);
}

// A device that rasterizes one field per frame (480-line interlace, E1 bit 10 clear) shows that field with each
// row doubled over its pair; the other field's rows are the previous frame's and never reach the picture.
static void test_an_interlaced_one_field_frame_presents_that_field(void) {
  Stream s = baseState();
  s.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  s.add({0xE1000000u | page(8, 0, 0, 0)});
  s.add({rgb(0x28, 200, 30, 30),
         xy(kBuffer.x0, kBuffer.y0),
         xy(kBuffer.x1, kBuffer.y0),
         xy(kBuffer.x0, kBuffer.y1),
         xy(kBuffer.x1, kBuffer.y1)});
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  device.gp1(0x08000000u | 0x24u | 0x01u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  RecordRasterizer rasterizer(gDevice);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, {kBuffer, 0});
  for (std::uint32_t word : s.words) {
    device.gp0(word, 0x80100000u);
  }
  const psx::present::FrameRecord record = device.sealRecord();
  rasterizer.update(cmd, record, false, device.vram(), 1, {kBuffer, 0});
  const psx::gpu::RecordPicture picture = rasterizer.presented();
  rasterizer.download(cmd, picture);
  CHECK(submitAndWait(cmd));
  const std::vector<std::uint16_t> shown = rasterizer.downloaded();
  const std::optional<int> undrawn = record.undrawnRowParity();
  CHECK(undrawn.has_value());
  CHECK(picture.texture != rasterizer.image());
  CHECK(rasterizer.woven().texture == rasterizer.image());
  const std::span<const std::uint16_t> vram = device.vram();
  const int width = kBuffer.x1 - kBuffer.x0;
  long wrong = 0;
  long stale = 0;
  for (int row = 0; row < kBuffer.y1 - kBuffer.y0; row++) {
    const int vramRow = kBuffer.y0 + row;
    const int drawnRow = (vramRow & 1) == *undrawn ? vramRow ^ 1 : vramRow;
    for (int column = 0; column < width; column++) {
      const std::size_t at =
          static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column);
      const std::uint16_t want = vram[static_cast<std::size_t>(drawnRow) * psx::gpu::kRecordVramWidth +
                                      static_cast<std::size_t>(kBuffer.x0 + column)];
      wrong += shown[at] != want ? 1 : 0;
      stale += vram[static_cast<std::size_t>(vramRow) * psx::gpu::kRecordVramWidth +
                    static_cast<std::size_t>(kBuffer.x0 + column)] != want
                   ? 1
                   : 0;
    }
  }
  CHECK_EQ(wrong, 0);
  CHECK(stale > 0);
}

// The canvas equals the device drawing the same frame with its draw area widened by the margin.
static void test_a_display_draw_reaches_the_margins(void) {
  Stream wide = baseState();
  wide.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  wide.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0, kBuffer.y0), xy(320, 240)});
  crossingPrimitives(wide);
  const CanvasOutcome out = present({wide}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK(out.picture.texture != out.vramImage);
  CHECK(out.picture.rect == (psx::gpu::RecordRect{0, 0, 416, 240}));
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);

  Stream reference = baseState();
  reference.area(kBuffer.x0 - kMargin, kBuffer.y0, kBuffer.x1 - 1 + kMargin, kBuffer.y1 - 1);
  reference.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0 - kMargin, kBuffer.y0), xy(416, 240)});
  crossingPrimitives(reference);
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  for (std::uint32_t word : reference.words) {
    device.gp0(word);
  }
  const std::vector<std::uint16_t> wideDevice(device.vram().begin(), device.vram().end());
  CHECK_EQ(differsFromVram(out.canvas, 416, 240, wideDevice, kBuffer.x0 - kMargin, kBuffer.y0), 0);
  // Something was drawn in each margin.
  CHECK(differsFromVram(out.canvas, 416, 240, out.device, kBuffer.x0 - kMargin, kBuffer.y0) > 0);
}

// A buffer at VRAM x = 0 puts its left margin left of VRAM; a flat quad drawn there lands in the canvas.
static void test_a_draw_left_of_vram_reaches_the_left_margin(void) {
  constexpr psx::gpu::RecordRect kFlush{0, 0, 320, 240};
  Stream s = baseState();
  s.area(kFlush.x0, kFlush.y0, kFlush.x1 - 1, kFlush.y1 - 1);
  s.add({rgb(0x28, 0, 248, 0), xy(-40, 20), xy(-5, 20), xy(-40, 60), xy(-5, 60)});
  const CanvasOutcome out = present({s}, {kFlush, kMargin});
  CHECK(out.ran);
  constexpr std::size_t width = 320 + 2 * kMargin;
  const std::uint16_t green = static_cast<std::uint16_t>(31u << 5);
  long drawn = 0;
  for (int row = 20; row < 60; row++) {
    for (int column = kMargin - 40; column < kMargin - 5; column++) {
      drawn += out.canvas[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)] == green ? 1 : 0;
    }
  }
  CHECK_EQ(drawn, 35 * 40);
}

// Mega Man X4 shows (0,0,320,240) and (0,240,320,479) alternately: neither canvas may retire the other.
static void test_a_double_buffer_of_differing_heights_keeps_both_canvases(void) {
  const psx::gpu::RecordCanvas top{{0, 0, 320, 240}, kMargin};
  const psx::gpu::RecordCanvas bottom{{0, 240, 320, 479}, kMargin};
  CHECK(psx::gpu::canvasSurvives(top, 320, kMargin));
  CHECK(psx::gpu::canvasSurvives(bottom, 320, kMargin));
  CHECK(!psx::gpu::canvasSurvives(top, 320, kMargin + 1));
  CHECK(!psx::gpu::canvasSurvives(top, 256, kMargin));
  CHECK(!psx::gpu::canvasSurvives(top, 320, 0));
}

// A draw area inset in rows (Tekken 3 draws rows 20..467 of 480) still reaches the margins.
static void test_a_row_inset_display_draw_reaches_the_margins(void) {
  constexpr int kInset = 20;
  Stream inset = baseState();
  inset.area(kBuffer.x0, kBuffer.y0 + kInset, kBuffer.x1 - 1, kBuffer.y1 - 1 - kInset);
  inset.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0, kBuffer.y0 + kInset), xy(320, 240 - 2 * kInset)});
  crossingPrimitives(inset);
  const CanvasOutcome out = present({inset}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);

  Stream reference = baseState();
  reference.area(kBuffer.x0 - kMargin, kBuffer.y0 + kInset, kBuffer.x1 - 1 + kMargin, kBuffer.y1 - 1 - kInset);
  reference.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0 - kMargin, kBuffer.y0 + kInset), xy(416, 240 - 2 * kInset)});
  crossingPrimitives(reference);
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  for (std::uint32_t word : reference.words) {
    device.gp0(word);
  }
  const std::vector<std::uint16_t> wideDevice(device.vram().begin(), device.vram().end());
  // Rows outside the inset keep the seed, so compare the inset rows only.
  long wrong = 0;
  for (int row = kInset; row < 240 - kInset; row++) {
    for (int column = 0; column < 416; column++) {
      const std::uint16_t pixel = out.canvas[(static_cast<std::size_t>(row) * 416) + static_cast<std::size_t>(column)];
      const std::uint16_t want = wideDevice[static_cast<std::size_t>(kBuffer.y0 + row) * 1024 +
                                            static_cast<std::size_t>(kBuffer.x0 - kMargin + column)];
      wrong += pixel != want ? 1 : 0;
    }
  }
  CHECK_EQ(wrong, 0);
  CHECK(differsFromVram(out.canvas, 416, 240, out.device, kBuffer.x0 - kMargin, kBuffer.y0) > 0);
}

static void test_a_fill_of_the_display_clears_the_margins(void) {
  Stream first = baseState();
  first.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  first.add({rgb(0x20, 255, 0, 0), xy(-200, -10), xy(700, -10), xy(250, 600)});
  Stream second;
  second.add({rgb(0x02, 0, 0, 248), xy(kBuffer.x0, kBuffer.y0), xy(320, 240)});
  const CanvasOutcome out = present({first, second}, {kBuffer, kMargin});
  CHECK(out.ran);
  long notBlue = 0;
  for (std::uint16_t pixel : out.canvas) {
    notBlue += pixel != 0x7C00u ? 1 : 0;
  }
  CHECK_EQ(notBlue, 0);
}

static void test_an_offscreen_draw_leaves_the_canvas(void) {
  Stream s = baseState();
  s.area(512, 256, 767, 511);
  s.add({rgb(0x20, 255, 0, 0), xy(400, 200), xy(900, 260), xy(600, 600)});
  const CanvasOutcome out = present({s}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);
  // The seed: the buffer's VRAM in the middle, empty margins.
  long wrong = 0;
  for (int row = 0; row < 240; row++) {
    for (int column = 0; column < 416; column++) {
      const std::uint16_t pixel = out.canvas[(static_cast<std::size_t>(row) * 416) + static_cast<std::size_t>(column)];
      const int x = column - kMargin;
      const std::uint16_t want =
          x < 0 || x >= 320 ? 0 : out.device[static_cast<std::size_t>(row) * 1024 + static_cast<std::size_t>(64 + x)];
      wrong += pixel != want ? 1 : 0;
    }
  }
  CHECK_EQ(wrong, 0);
}

static void test_fills(void) {
  Stream s = baseState();
  s.add({rgb(0x02, 0xF8, 0x80, 0x08), xy(16, 16), xy(64, 32)});
  s.add({rgb(0x02, 0x13, 0x57, 0x9B), xy(37, 70), xy(33, 9)});     // x and width round to 16
  s.add({rgb(0x02, 0x40, 0x40, 0x40), xy(1008, 500), xy(48, 24)}); // wraps both axes
  CHECK_REPLAY(s);
}

static void test_copies(void) {
  Stream s = baseState();
  s.add({0x80000000u, xy(600, 10), xy(20, 300), xy(40, 30)});
  s.add({0x80000000u, xy(100, 100), xy(103, 102), xy(150, 40)}); // overlapping, moving right/down
  s.add({0x80000000u, xy(203, 202), xy(200, 200), xy(130, 20)}); // overlapping, moving left/up
  s.add({0x80000000u, xy(1000, 500), xy(300, 300), xy(50, 30)}); // source wraps
  s.mask(true, false);
  s.add({0x80000000u, xy(0, 0), xy(400, 0), xy(32, 32)});
  s.mask(false, true);
  s.add({0x80000000u, xy(700, 300), xy(400, 0), xy(64, 64)});
  CHECK_REPLAY(s);
}

static void test_uploads(void) {
  Stream s = baseState();
  s.upload(10, 10, 33, 7, 1);
  s.upload(1010, 505, 30, 20, 2); // wraps
  s.mask(true, true);
  s.upload(20, 12, 16, 16, 3);
  CHECK_REPLAY(s);
}

// At a scale that is not a power of two, an upload and a sprite sampling it fill every pixel of each block.
static void test_a_scaled_upload_and_a_read_of_it_fill_whole_blocks(void) {
  Stream s = baseState();
  s.upload(600, 300, 40, 24, 11);
  s.add({drawMode(page(9, 1, 0, 2), false, false, false)});
  s.add({rgb(0x65, 0, 0, 0), xy(10, 10), uv(24, 44, 0), xy(40, 24)});
  s.upload(1010, 505, 30, 20, 2);
  for (int scale : {3, 5}) {
    const Outcome outcome = replay(s, {}, scale);
    CHECK(outcome.replayed);
    CHECK_EQ(outcome.mismatched, 0);
  }
}

// A present before the first record shows the presenter's empty record; the device's first record still lands.
static void test_the_first_record_follows_the_empty_one(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  RecordRasterizer rasterizer(gDevice);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  const psx::gpu::RecordView view{kBuffer, 0};
  rasterizer.update(cmd, psx::present::FrameRecord(), false, device.vram(), 1, view);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, view);
  rasterizer.download(cmd, {rasterizer.image(), kWholeVram});
  CHECK(submitAndWait(cmd));
  const std::vector<std::uint16_t> vram(device.vram().begin(), device.vram().end());
  CHECK_EQ(differsFromVram(rasterizer.downloaded(), 1024, 512, vram, 0, 0), 0);
}

static void test_flat_and_gouraud_polygons(void) {
  for (int semi = 0; semi < 4; semi++) {
    for (int dither = 0; dither < 2; dither++) {
      Stream s = baseState();
      s.add({drawMode(page(8, 0, semi, 0), dither != 0, false, false)});
      s.add({rgb(0x20, 200, 100, 50), xy(10, 10), xy(200, 40), xy(60, 180)});
      s.add({rgb(0x22, 90, 180, 250), xy(30, 30), xy(230, 60), xy(80, 200)});
      s.add({rgb(0x28, 30, 60, 90), xy(250, 10), xy(400, 20), xy(240, 120), xy(420, 150)});
      s.add({rgb(0x2A, 255, 255, 255), xy(260, 30), xy(410, 40), xy(250, 140), xy(430, 170)});
      s.add({rgb(0x30, 255, 0, 0), xy(20, 220), rgb(0, 0, 255, 0), xy(220, 260), rgb(0, 0, 0, 255), xy(90, 400)});
      s.add({rgb(0x32, 10, 200, 30), xy(40, 240), rgb(0, 250, 5, 128), xy(240, 280), rgb(0, 7, 9, 250), xy(110, 420)});
      s.add({rgb(0x38, 0, 0, 0),
             xy(280, 220),
             rgb(0, 255, 128, 0),
             xy(500, 230),
             rgb(0, 0, 128, 255),
             xy(270, 380),
             rgb(0, 255, 255, 255),
             xy(505, 400)});
      s.add({rgb(0x3A, 64, 32, 16),
             xy(300, 240),
             rgb(0, 16, 32, 64),
             xy(480, 250),
             rgb(0, 100, 0, 100),
             xy(290, 360),
             rgb(0, 1, 2, 3),
             xy(470, 390)});
      CHECK_REPLAY(s);
    }
  }
}

static void test_textured_polygons(void) {
  for (int mode = 0; mode < 3; mode++) {
    for (int semi = 0; semi < 4; semi++) {
      Stream s = baseState();
      const std::uint32_t texture = page(9, 0, semi, mode);
      const std::uint32_t clut = clutAt(512, 480 + semi);
      s.add({drawMode(texture, true, false, false)});
      s.add({rgb(0x24, 128, 128, 128),
             xy(10, 10),
             uv(0, 0, clut),
             xy(200, 30),
             uv(200, 10, texture),
             xy(40, 200),
             uv(20, 250, 0)});
      s.add({rgb(0x25, 0, 0, 0),
             xy(220, 10),
             uv(5, 5, clut),
             xy(420, 20),
             uv(80, 5, texture),
             xy(230, 90),
             uv(5, 80, 0)});
      s.add({rgb(0x26, 200, 60, 90),
             xy(260, 100),
             uv(0, 0, clut),
             xy(500, 140),
             uv(255, 0, texture),
             xy(280, 260),
             uv(0, 255, 0)});
      s.add({rgb(0x2C, 90, 100, 200),
             xy(10, 260),
             uv(0, 0, clut),
             xy(130, 260),
             uv(64, 0, texture),
             xy(10, 380),
             uv(0, 64, 0),
             xy(130, 380),
             uv(64, 64, 0)});
      s.add({rgb(0x2F, 0, 0, 0),
             xy(150, 270),
             uv(64, 0, clut),
             xy(250, 290),
             uv(0, 0, texture),
             xy(140, 400),
             uv(64, 64, 0),
             xy(260, 410),
             uv(0, 64, 0)});
      s.add({rgb(0x34, 255, 0, 0),
             xy(280, 300),
             uv(10, 10, clut),
             rgb(0, 0, 255, 0),
             xy(500, 320),
             uv(100, 20, texture),
             rgb(0, 0, 0, 255),
             xy(300, 500),
             uv(30, 120, 0)});
      s.add({rgb(0x3E, 40, 80, 160),
             xy(20, 420),
             uv(0, 0, clut),
             rgb(0, 160, 80, 40),
             xy(200, 410),
             uv(127, 0, texture),
             rgb(0, 255, 255, 255),
             xy(30, 505),
             uv(0, 63, 0),
             rgb(0, 0, 0, 0),
             xy(210, 500),
             uv(127, 63, 0)});
      CHECK_REPLAY(s);
    }
  }
}

// CTR's additive gouraud textured fade triangles: one vertex bright, two black, drawn under a row offset.
static void test_steep_gouraud_textured_semi_triangles(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(14, 0, 1, 0);
  const std::uint32_t clut = clutAt(912, 254);
  s.area(0, 296, 511, 511);
  s.offset(0, 296);
  s.add({drawMode(texture, true, false, false)});
  s.add({rgb(0x36, 0, 0, 0),
         xy(-20, 82),
         uv(191, 48, clut),
         rgb(0, 253, 255, 255),
         xy(42, 90),
         uv(191, 95, texture),
         rgb(0, 0, 0, 0),
         xy(9, 68),
         uv(144, 48, 0)});
  s.add({rgb(0x36, 0, 0, 0),
         xy(2, 97),
         uv(144, 48, clut),
         rgb(0, 0, 0, 0),
         xy(-20, 82),
         uv(144, 95, texture),
         rgb(0, 253, 255, 255),
         xy(42, 90),
         uv(191, 95, 0)});
  CHECK_REPLAY(s);
}

// gpu.c fetches texels through a cache that only an upload, copy or read invalidates, and a draw reads the
// pixels it wrote earlier; the rasterizer's snapshot read must still end on the device's picture.
static void test_a_draw_that_samples_its_own_pixels(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x25, 128, 128, 128), xy(4, 4), uv(12, 9, 0), xy(70, 6), uv(90, 7, texture), xy(30, 80), uv(30, 75, 0)});
  s.add({rgb(0x2D, 128, 128, 128),
         xy(10, 100),
         uv(0, 98, 0),
         xy(90, 100),
         uv(80, 98, texture),
         xy(10, 150),
         uv(5, 120, 0),
         xy(90, 150),
         uv(85, 130, 0)});
  CHECK_REPLAY(s);
}

static void test_a_draw_over_a_cached_texture_is_sampled_stale(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  const auto sample = [&](int x, int y) {
    s.add({rgb(0x25, 128, 128, 128),
           xy(x, y),
           uv(0, 0, 0),
           xy(x + 60, y),
           uv(60, 0, texture),
           xy(x, y + 60),
           uv(0, 60, 0)});
  };
  sample(300, 300);
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  sample(300, 400);
  CHECK_REPLAY(s);
}

// gpu.c SetTPage invalidates its texture cache when the page changes, from E1 or from a textured polygon's own
// texpage word; a texture read after that sees current VRAM and needs no device-resolved pixels.
static void test_a_texpage_change_invalidates_the_cache_through_e1(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  const std::uint32_t other = page(2, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  s.add({drawMode(other, false, false, false)});
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 300),
         uv(0, 0, 0),
         xy(360, 300),
         uv(60, 0, texture),
         xy(300, 360),
         uv(0, 60, 0)});
  const Outcome outcome = replay(s);
  CHECK(outcome.replayed);
  CHECK_EQ(outcome.mismatched, 0);
  CHECK_EQ(outcome.uploads, 0u);
}

static void test_a_texpage_change_invalidates_the_cache_through_a_polygon(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  const std::uint32_t other = page(2, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 300),
         uv(0, 0, 0),
         xy(360, 300),
         uv(60, 0, other),
         xy(300, 360),
         uv(0, 60, 0)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 400),
         uv(0, 0, 0),
         xy(360, 400),
         uv(60, 0, texture),
         xy(300, 460),
         uv(0, 60, 0)});
  const Outcome outcome = replay(s);
  CHECK(outcome.replayed);
  CHECK_EQ(outcome.mismatched, 0);
  CHECK_EQ(outcome.uploads, 0u);
}

static void test_texture_window(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(10, 0, 0, 1);
  s.add({drawMode(texture, false, false, false)});
  // Mask 3 (24 texels), offset 2 on x; mask 1, offset 1 on y.
  s.add({0xE2000000u | 3u | (1u << 5) | (2u << 10) | (1u << 15)});
  s.add({rgb(0x2C, 128, 128, 128),
         xy(10, 10),
         uv(0, 0, clutAt(512, 400)),
         xy(300, 10),
         uv(255, 0, texture),
         xy(10, 300),
         uv(0, 255, 0),
         xy(300, 300),
         uv(255, 255, 0)});
  s.add({rgb(0x64, 128, 128, 128), xy(320, 10), uv(3, 7, clutAt(512, 401)), xy(150, 120)});
  CHECK_REPLAY(s);
}

static void test_sprites(void) {
  for (int flip = 0; flip < 4; flip++) {
    Stream s = baseState();
    const std::uint32_t texture = page(11, 1, 1, flip % 3);
    s.add({drawMode(texture, true, (flip & 1) != 0, (flip & 2) != 0)});
    s.add({rgb(0x60, 50, 100, 150), xy(5, 5), xy(37, 19)});
    s.add({rgb(0x62, 250, 10, 100), xy(20, 10), xy(40, 30)});
    s.add({rgb(0x68, 255, 255, 255), xy(100, 3)});
    s.add({rgb(0x70, 0, 255, 0), xy(110, 3)});
    s.add({rgb(0x78, 0, 0, 255), xy(120, 3)});
    s.add({rgb(0x64, 128, 128, 128), xy(10, 60), uv(13, 200, clutAt(528, 300)), xy(120, 90)});
    s.add({rgb(0x65, 0, 0, 0), xy(140, 60), uv(250, 250, clutAt(528, 301)), xy(80, 70)}); // u, v wrap
    s.add({rgb(0x66, 60, 200, 128), xy(240, 60), uv(0, 0, clutAt(528, 302)), xy(100, 100)});
    s.add({rgb(0x74, 200, 100, 50), xy(10, 200), uv(9, 9, clutAt(528, 303))});
    s.add({rgb(0x7C, 128, 128, 128), xy(30, 200), uv(31, 17, clutAt(528, 304))});
    s.add({rgb(0x6C, 255, 128, 64), xy(60, 200), uv(1, 2, clutAt(528, 305))});
    s.offset(-20, -15); // clipped at the draw area's top-left
    s.add({rgb(0x64, 128, 128, 128), xy(0, 0), uv(100, 100, clutAt(528, 306)), xy(64, 64)});
    CHECK_REPLAY(s);
  }
}

static void test_lines(void) {
  for (int semi = 0; semi < 4; semi++) {
    Stream s = baseState();
    s.add({drawMode(page(8, 0, semi, 0), semi % 2 == 0, false, false)});
    s.add({rgb(0x40, 255, 128, 0), xy(10, 10), xy(300, 100)});
    s.add({rgb(0x42, 0, 128, 255), xy(300, 10), xy(10, 120)});
    s.add({rgb(0x40, 9, 99, 199), xy(50, 300), xy(50, 300)});
    s.add({rgb(0x50, 255, 0, 0), xy(20, 200), rgb(0, 0, 0, 255), xy(480, 260)});
    s.add({rgb(0x52, 0, 255, 0), xy(200, 500), rgb(0, 255, 0, 255), xy(210, 150)});
    s.add({rgb(0x48, 200, 200, 200), xy(100, 400), xy(150, 350), xy(250, 450), xy(400, 380), 0x55555555u});
    s.add({rgb(0x5A, 255, 255, 0),
           xy(300, 300),
           rgb(0, 0, 255, 255),
           xy(350, 500),
           rgb(0, 255, 0, 255),
           xy(500, 320),
           0x50005000u});
    s.offset(-30, 600); // vertices wrap at 11 bits
    s.add({rgb(0x40, 77, 177, 77), xy(40, -580), xy(470, -500)});
    CHECK_REPLAY(s);
  }
}

static void test_clip_offset_and_mask(void) {
  Stream s = baseState();
  s.area(40, 30, 300, 200);
  s.offset(25, -10);
  s.add({rgb(0x28, 120, 50, 200), xy(-100, -100), xy(500, -80), xy(-90, 400), xy(480, 420)});
  s.mask(true, false);
  s.add({rgb(0x20, 255, 255, 0), xy(0, 0), xy(200, 20), xy(30, 180)});
  s.mask(false, true);
  s.add({rgb(0x30, 0, 255, 255), xy(10, 10), rgb(0, 255, 0, 0), xy(260, 40), rgb(0, 0, 0, 255), xy(40, 200)});
  s.add({rgb(0x60, 9, 9, 9), xy(100, 50), xy(100, 100)});
  CHECK_REPLAY(s);
}

static void test_interlaced_row_skip(void) {
  Stream s;
  s.area(0, 0, 511, 511);
  s.offset(0, 0);
  // Drawing to the display area disallowed (E1 bit 10 clear) in 480-line interlace: one field only.
  s.add({0xE1000000u | page(8, 0, 0, 0)});
  s.add({rgb(0x28, 200, 30, 30), xy(0, 0), xy(320, 0), xy(0, 480), xy(320, 480)});
  s.add({rgb(0x60, 30, 200, 30), xy(330, 10), xy(100, 100)});
  s.add({rgb(0x40, 30, 30, 200), xy(330, 200), xy(500, 300)});
  CHECK_REPLAY(s, {0x08000000u | 0x24u | 0x01u});
}

// Seeded random primitives under random state; the bounds in the file comment are kept.
static void test_random_streams(void) {
  std::mt19937 random(1234);
  const auto pick = [&](int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(random);
  };
  for (int round = 0; round < 6; round++) {
    Stream s = baseState();
    for (int n = 0; n < 400; n++) {
      const int kind = pick(0, 9);
      const std::uint32_t texture = page(pick(8, 15), pick(0, 1), pick(0, 3), pick(0, 2));
      const std::uint32_t clut = clutAt(pick(32, 63) * 16, pick(0, 511));
      const auto vertex = [&] {
        return xy(pick(-80, 560), pick(-60, 560));
      };
      const auto colour = [&](std::uint32_t command) {
        return rgb(command, pick(0, 255), pick(0, 255), pick(0, 255));
      };
      if (kind == 0) {
        s.add({drawMode(texture, pick(0, 1) != 0, pick(0, 1) != 0, pick(0, 1) != 0)});
        s.mask(pick(0, 3) == 0, pick(0, 3) == 0);
        const int x0 = pick(0, 200);
        const int y0 = pick(0, 200);
        s.area(x0, y0, pick(x0, 511), pick(y0, 511));
        s.offset(pick(-64, 64), pick(-64, 64));
        s.add({0xE2000000u | (pick(0, 3) == 0 ? static_cast<std::uint32_t>(random() & 0xFFFFFu) : 0u)});
      } else if (kind <= 3) {
        const std::uint32_t command = 0x20u | static_cast<std::uint32_t>(pick(0, 31) & 0x1F);
        const bool gouraud = (command & 0x10u) != 0;
        const bool textured = (command & 0x04u) != 0;
        const int count = (command & 0x08u) != 0 ? 4 : 3;
        for (int v = 0; v < count; v++) {
          if (v == 0 || gouraud) {
            s.add({colour(v == 0 ? command : 0)});
          }
          s.add({vertex()});
          if (textured) {
            s.add({uv(pick(0, 255), pick(0, 255), v == 0 ? clut : v == 1 ? texture : 0)});
          }
        }
      } else if (kind <= 5) {
        const std::uint32_t command = 0x60u | static_cast<std::uint32_t>(pick(0, 31) & 0x1F);
        s.add({colour(command), vertex()});
        if ((command & 0x04u) != 0) {
          s.add({uv(pick(0, 255), pick(0, 255), clut)});
        }
        if ((command & 0x18u) == 0) {
          s.add({xy(pick(0, 300), pick(0, 300))});
        }
      } else if (kind == 6) {
        const std::uint32_t command = 0x40u | static_cast<std::uint32_t>(pick(0, 31) & 0x1A);
        const bool gouraud = (command & 0x10u) != 0;
        const int points = (command & 0x08u) != 0 ? pick(2, 6) : 2;
        for (int p = 0; p < points; p++) {
          if (p == 0 || gouraud) {
            s.add({colour(p == 0 ? command : 0)});
          }
          s.add({vertex()});
        }
        if ((command & 0x08u) != 0) {
          s.add({0x55555555u});
        }
      } else if (kind == 7) {
        s.add({colour(0x02), xy(pick(0, 400), pick(0, 500)), xy(pick(0, 96), pick(0, 100))});
      } else if (kind == 8) {
        s.add({0x80000000u,
               xy(pick(0, 1023), pick(0, 511)),
               xy(pick(0, 1023), pick(0, 511)),
               xy(pick(1, 96), pick(1, 64))});
      } else {
        s.upload(pick(0, 1023), pick(0, 511), pick(1, 40), pick(1, 20), static_cast<std::uint32_t>(random()));
      }
    }
    CHECK_REPLAY(s);
  }
}

int main(void) {
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
  if (!SDL_Init(SDL_INIT_VIDEO) ||
      (gDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr)) == nullptr) {
    fprintf(stderr, "    FAIL no headless Vulkan device: %s\n", SDL_GetError());
    return 1;
  }
  RUN(fills);
  RUN(copies);
  RUN(uploads);
  RUN(the_first_record_follows_the_empty_one);
  RUN(a_scaled_upload_and_a_read_of_it_fill_whole_blocks);
  RUN(flat_and_gouraud_polygons);
  RUN(textured_polygons);
  RUN(steep_gouraud_textured_semi_triangles);
  RUN(a_draw_that_samples_its_own_pixels);
  RUN(a_draw_over_a_cached_texture_is_sampled_stale);
  RUN(a_texpage_change_invalidates_the_cache_through_e1);
  RUN(a_texpage_change_invalidates_the_cache_through_a_polygon);
  RUN(texture_window);
  RUN(sprites);
  RUN(lines);
  RUN(clip_offset_and_mask);
  RUN(interlaced_row_skip);
  RUN(an_interlaced_one_field_frame_presents_that_field);
  RUN(random_streams);
  RUN(four_three_presents_the_device_display);
  RUN(a_display_draw_reaches_the_margins);
  RUN(a_draw_left_of_vram_reaches_the_left_margin);
  RUN(a_row_inset_display_draw_reaches_the_margins);
  RUN(a_double_buffer_of_differing_heights_keeps_both_canvases);
  RUN(a_fill_of_the_display_clears_the_margins);
  RUN(an_offscreen_draw_leaves_the_canvas);
  RUN(a_double_buffer_in_between_sits_between_the_shown_pictures);
  RUN(an_empty_record_after_the_shown_one_keeps_its_in_between);
  RUN(a_single_buffer_in_between_blends_n_minus_one_and_n);
  RUN(a_rendered_object_draws_every_present_from_its_state);
  SDL_DestroyGPUDevice(gDevice);
  SDL_Quit();
  return pt_summary();
}
