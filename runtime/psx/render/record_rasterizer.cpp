// record_rasterizer.cpp — the record rasterizer's GPU half: buffers, passes and the snapshot.
#include "record_rasterizer.h"

#include "psxport_generated/render_shaders.h"
#include "render_check.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace psx::render {
namespace {

using gpu::RecordCanvas;
using gpu::RecordRect;

constexpr SDL_GPUTextureFormat kVramFormat = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
constexpr ShaderCode kRecordVertex{spv_g_record_vert, spv_g_record_vert_len, {0, 1, 1}};
constexpr ShaderCode kRecordFragment{spv_g_record_frag, spv_g_record_frag_len, {3, 1, 3}};
constexpr ShaderCode kRecordDepthFragment{spv_g_record_depth_frag, spv_g_record_depth_frag_len, {3, 1, 3}};
constexpr ShaderCode kRecordDepthBackFragment{
    spv_g_record_depth_back_frag, spv_g_record_depth_back_frag_len, {3, 1, 3}};
constexpr std::uint32_t kDepthRunFlags =
    gpu::kOpDepthTest | gpu::kOpDepthObject | gpu::kOpDepthModel | gpu::kOpDepthGroup;
// Stencil: the high bit marks where a run's test draw passed, until its write draw clears it; the low bits name the
// Model whose depth a pixel holds (gpu::kModelIds), 0 for any other.
constexpr std::uint8_t kPassed = 0x80;
constexpr std::uint8_t kModelBits = 0x7F;

struct TargetUniform {
  float width = 0;
  float height = 0;
  float unused[2] = {};
};

struct ParamsUniform {
  std::int32_t scale = 1;
  std::int32_t originX = 0;
  std::int32_t originY = 0;
  std::int32_t unused = 0;
};

std::uint64_t coveredPixels(std::span<const gpu::RecordQuad> quads) {
  std::uint64_t pixels = 0;
  for (const gpu::RecordQuad &quad : quads) {
    pixels += static_cast<std::uint64_t>(std::max(0, quad.x1 - quad.x0)) *
              static_cast<std::uint64_t>(std::max(0, quad.y1 - quad.y0));
  }
  return pixels;
}

// record.frag writes log2(z) / 16; 24 bits resolve z far below a bin.
SDL_GPUTextureFormat depthFormat(SDL_GPUDevice *device) {
  return SDL_GPUTextureSupportsFormat(device,
                                      SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
             ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
             : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

RenderDevice::DepthTarget depthTarget(SDL_GPUTextureFormat format, SDL_GPUCompareOp compare, bool writes) {
  RenderDevice::DepthTarget target;
  target.format = format;
  target.state.enable_depth_test = true;
  target.state.enable_depth_write = writes;
  target.state.compare_op = compare;
  return target;
}

// The stencil bits a draw compares and the bits it writes.
struct StencilBits {
  std::uint8_t compare = 0xFF;
  std::uint8_t write = 0xFF;
};

// Every face runs `compare` of the reference against the stencil's `bits.compare`, and on passing both tests applies
// `passed` to its `bits.write`.
RenderDevice::DepthTarget withStencil(RenderDevice::DepthTarget target,
                                      SDL_GPUCompareOp compare,
                                      SDL_GPUStencilOp passed,
                                      StencilBits bits = {}) {
  SDL_GPUStencilOpState face = {};
  face.fail_op = SDL_GPU_STENCILOP_KEEP;
  face.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
  face.pass_op = passed;
  face.compare_op = compare;
  target.state.enable_stencil_test = true;
  target.state.front_stencil_state = face;
  target.state.back_stencil_state = face;
  target.state.compare_mask = bits.compare;
  target.state.write_mask = bits.write;
  return target;
}

// A quad's depth flags and, for a Model part, its model's stencil id: consecutive quads with the same draw as one run.
struct DepthRun {
  std::uint32_t flags = 0;
  std::uint32_t model = 0;
  bool operator==(const DepthRun &) const = default;
};

DepthRun depthRun(const gpu::RecordRasterPlan &plan, const gpu::RecordQuad &quad) {
  const std::uint32_t flags = plan.ops[quad.op][gpu::kOpFlags] & kDepthRunFlags;
  return {flags, (flags & gpu::kOpDepthModel) != 0u ? plan.ops[quad.op + 1][gpu::kDepthModel] : 0u};
}

// Storage buffers are read as whole words, and never sized zero.
std::uint32_t paddedBytes(std::size_t bytes) {
  return static_cast<std::uint32_t>(std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 16));
}

} // namespace

RecordRasterizer::RecordRasterizer(RenderDevice &device)
    : device_(device), pipeline_(device.makePipeline(kRecordVertex, kRecordFragment, kVramFormat)),
      depthFormat_(depthFormat(device.handle())) {
  const RenderDevice::DepthTarget slot = depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, false);
  const RenderDevice::DepthTarget tests = withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_LESS_OR_EQUAL, false),
                                                      SDL_GPU_COMPAREOP_ALWAYS,
                                                      SDL_GPU_STENCILOP_REPLACE);
  const RenderDevice::DepthTarget marks = withStencil(
      depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, true), SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_STENCILOP_ZERO);
  // An Object's depth is no Model's.
  const RenderDevice::DepthTarget covers = withStencil(
      depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, true), SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_STENCILOP_ZERO);
  // A Model part tests where another's depth lies and paints over its own model's, marking where it drew.
  const RenderDevice::DepthTarget modelTests =
      withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_LESS_OR_EQUAL, false),
                  SDL_GPU_COMPAREOP_NOT_EQUAL,
                  SDL_GPU_STENCILOP_REPLACE,
                  {.compare = kModelBits, .write = kPassed});
  const RenderDevice::DepthTarget modelPaints = withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, false),
                                                            SDL_GPU_COMPAREOP_EQUAL,
                                                            SDL_GPU_STENCILOP_REPLACE,
                                                            {.compare = kModelBits, .write = kPassed});
  // Where it drew it leaves the nearer of its moved-back depth and the depth there, and names its model where its own
  // is kept.
  const RenderDevice::DepthTarget leaves = withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_LESS_OR_EQUAL, true),
                                                       SDL_GPU_COMPAREOP_EQUAL,
                                                       SDL_GPU_STENCILOP_REPLACE,
                                                       {.compare = kPassed, .write = kModelBits});
  const RenderDevice::DepthTarget clears = withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, false),
                                                       SDL_GPU_COMPAREOP_EQUAL,
                                                       SDL_GPU_STENCILOP_ZERO,
                                                       {.compare = kPassed, .write = kPassed});
  SDL_GPUColorTargetBlendState noColour = {};
  noColour.enable_color_write_mask = true;
  noColour.color_write_mask = 0;
  slotPipeline_ = device.makePipeline(kRecordVertex, kRecordFragment, kVramFormat, nullptr, nullptr, &slot);
  groupTestPipeline_ = device.makePipeline(kRecordVertex, kRecordDepthFragment, kVramFormat, nullptr, nullptr, &tests);
  groupMarkPipeline_ =
      device.makePipeline(kRecordVertex, kRecordDepthBackFragment, kVramFormat, &noColour, nullptr, &marks);
  objectPipeline_ = device.makePipeline(kRecordVertex, kRecordDepthFragment, kVramFormat, nullptr, nullptr, &covers);
  modelTestPipeline_ =
      device.makePipeline(kRecordVertex, kRecordDepthFragment, kVramFormat, nullptr, nullptr, &modelTests);
  modelPaintPipeline_ =
      device.makePipeline(kRecordVertex, kRecordDepthFragment, kVramFormat, nullptr, nullptr, &modelPaints);
  modelMarkPipeline_ =
      device.makePipeline(kRecordVertex, kRecordDepthBackFragment, kVramFormat, &noColour, nullptr, &leaves);
  modelClearPipeline_ =
      device.makePipeline(kRecordVertex, kRecordDepthFragment, kVramFormat, &noColour, nullptr, &clears);
  canvases_.reserve(gpu::kRecordMaxCanvases);
}

RecordRasterizer::~RecordRasterizer() {
  releaseImage();
  for (Buffer *buffer : {&quads_, &ops_, &cluts_, &pixels_}) {
    device_.release(buffer->buffer);
  }
  device_.release(upload_);
  device_.release(readback_);
  device_.release(pipeline_);
  device_.release(slotPipeline_);
  device_.release(groupTestPipeline_);
  device_.release(groupMarkPipeline_);
  device_.release(objectPipeline_);
  device_.release(modelTestPipeline_);
  device_.release(modelPaintPipeline_);
  device_.release(modelMarkPipeline_);
  device_.release(modelClearPipeline_);
}

RecordRasterizer::Plane RecordRasterizer::makePlane(const RecordCanvas &shape) {
  const PixelSize scaled{shape.width() * scale_, shape.height() * scale_};
  constexpr SDL_GPUTextureUsageFlags kTarget = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  Plane plane;
  plane.image = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.snapshot = device_.makeTexture(scaled, kVramFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  plane.inBetween = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.before = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.width = shape.width();
  plane.height = shape.height();
  plane.originX = shape.originX();
  plane.originY = shape.originY();
  return plane;
}

void RecordRasterizer::releasePlane(Plane &plane) {
  for (SDL_GPUTexture **texture : {&plane.image, &plane.snapshot, &plane.inBetween, &plane.before, &plane.depth}) {
    device_.release(*texture);
  }
  plane = {};
}

void RecordRasterizer::releaseImage() {
  releasePlane(vram_);
  for (Canvas &canvas : canvases_) {
    releasePlane(canvas.plane);
  }
  canvases_.clear();
  presented_ = {};
  woven_ = {};
  device_.release(field_);
  device_.release(capture_);
  captureWidth_ = 0;
  captureHeight_ = 0;
  readBack_.reset();
}

void RecordRasterizer::ensureImage(int scale) {
  if (scale == scale_ && vram_.image != nullptr) {
    return;
  }
  releaseImage();
  scale_ = scale;
  vram_ = makePlane(RecordCanvas{.buffer = {0, 0, gpu::kVramWidth, gpu::kVramHeight}});
  chained_ = false;
  beforeHeld_ = false;
}

RecordRasterizer::Canvas *RecordRasterizer::canvasFor(const RecordView &view) {
  for (Canvas &canvas : canvases_) {
    if (canvas.shape.buffer == view.display && canvas.shape.reach == view.reach) {
      return &canvas;
    }
  }
  return nullptr;
}

namespace {

void setWindow(RecordCanvas &shape, present::HorizontalMargins margins) {
  shape.left = std::clamp(margins.left, 0, shape.reach);
  shape.right = std::clamp(margins.right, 0, shape.reach);
}

RecordRect windowOf(RecordCanvas shape, present::HorizontalMargins margins) {
  setWindow(shape, margins);
  return shape.window();
}

} // namespace

std::span<const RecordCanvas> RecordRasterizer::canvasShapes(present::HorizontalMargins margins) {
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    shapes_[i] = canvases_[i].shape;
    setWindow(shapes_[i], margins);
  }
  return {shapes_.data(), canvases_.size()};
}

void RecordRasterizer::adoptWindows(const gpu::RecordRasterPlan &plan, const present::RecordWindows &windows) {
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    if (!plan.quads[1 + i].empty()) {
      setWindow(canvases_[i].shape, windows.drawn);
      canvases_[i].view = windows.view;
    }
  }
}

RecordRect RecordRasterizer::copiedRect(int index, present::HorizontalMargins margins) {
  const Plane &target = plane(index);
  if (index == 0) {
    return {0, 0, target.width, target.height};
  }
  const RecordCanvas &shape = canvases_[static_cast<std::size_t>(index - 1)].shape;
  const RecordRect now = shape.window();
  const RecordRect then = windowOf(shape, margins);
  return {std::min(now.x0, then.x0), 0, std::max(now.x1, then.x1), target.height};
}

RecordRasterizer::Plane &RecordRasterizer::plane(int index) {
  return index == 0 ? vram_ : canvases_[static_cast<std::size_t>(index - 1)].plane;
}

void RecordRasterizer::showCanvas(SDL_GPUCommandBuffer *cmd,
                                  const RecordView &view,
                                  const present::RecordWindows &windows) {
  const int width = view.display.x1 - view.display.x0;
  const int height = view.display.y1 - view.display.y0;
  // A widening or display width change makes every canvas another shape; a split change keeps them.
  std::erase_if(canvases_, [&](Canvas &canvas) {
    const bool stale = !gpu::canvasSurvives(canvas.shape, width, view.reach);
    if (stale) {
      releasePlane(canvas.plane);
    }
    return stale;
  });
  if (view.reach <= 0 || width <= 0 || height <= 0) {
    return;
  }
  Canvas *canvas = canvasFor(view);
  if (canvas == nullptr) {
    if (canvases_.size() == static_cast<std::size_t>(gpu::kRecordMaxCanvases)) {
      auto oldest = std::min_element(canvases_.begin(), canvases_.end(), [](const Canvas &a, const Canvas &b) {
        return a.shown < b.shown;
      });
      releasePlane(oldest->plane);
      canvases_.erase(oldest);
    }
    Canvas added;
    added.shape = RecordCanvas{.buffer = view.display, .reach = view.reach};
    setWindow(added.shape, windows.drawn);
    added.view = windows.view;
    added.plane = makePlane(added.shape);
    canvases_.push_back(added);
    canvas = &canvases_.back();
    seedCanvas(cmd, *canvas, true);
  }
  canvas->shown = ++shows_;
}

void RecordRasterizer::seedCanvas(SDL_GPUCommandBuffer *cmd, Canvas &canvas, bool fresh) {
  for (auto [from, to] : {std::pair{vram_.image, canvas.plane.image}, std::pair{vram_.before, canvas.plane.before}}) {
    if (fresh) {
      SDL_GPUColorTargetInfo clear = {};
      clear.texture = to;
      clear.load_op = SDL_GPU_LOADOP_CLEAR;
      clear.store_op = SDL_GPU_STOREOP_STORE;
      SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &clear, 1, nullptr));
      ++work_.renderPasses;
    }
    copyTexture(cmd, from, canvas.shape.buffer, to, canvas.shape.reach, 0);
  }
  const RecordRect seeded = fresh ? RecordRect{0, 0, canvas.plane.width, canvas.plane.height}
                                  : RecordRect{canvas.shape.reach,
                                               0,
                                               canvas.shape.reach + canvas.shape.buffer.x1 - canvas.shape.buffer.x0,
                                               canvas.plane.height};
  copyTexture(cmd, canvas.plane.image, seeded, canvas.plane.snapshot, seeded.x0, 0);
}

void RecordRasterizer::present(const RecordView &view, bool inBetween, const present::RecordWindows *windows) {
  if (Canvas *canvas = view.reach > 0 ? canvasFor(view) : nullptr) {
    const present::RecordWindows shown =
        windows != nullptr ? *windows : present::RecordWindows{canvas->view, {canvas->shape.left, canvas->shape.right}};
    presented_ = {inBetween ? canvas->plane.inBetween : canvas->plane.image,
                  windowOf(canvas->shape, shown.view),
                  windowOf(canvas->shape, shown.drawn)};
    wovenOriginY_ = canvas->plane.originY;
  } else {
    presented_ = {inBetween ? vram_.inBetween : vram_.image, view.display, view.display};
    wovenOriginY_ = view.display.y0;
  }
  woven_ = presented_;
}

void RecordRasterizer::selectField(SDL_GPUCommandBuffer *cmd, const present::FrameRecord &record) {
  const std::optional<int> undrawn = record.undrawnRowParity();
  const RecordRect source = woven_.rect;
  const int width = source.x1 - source.x0;
  const int height = source.y1 - source.y0;
  if (!undrawn || woven_.texture == nullptr || width <= 0 || height <= 0) {
    return;
  }
  if (field_ == nullptr || fieldWidth_ != width || fieldHeight_ != height) {
    device_.release(field_);
    field_ = device_.makeTexture({width * scale_, height * scale_}, kVramFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    fieldWidth_ = width;
    fieldHeight_ = height;
  }
  copyTexture(cmd, woven_.texture, source, field_, 0, 0);
  // Each undrawn row repeats the drawn row of its pair, so the picture is one field and never two moments.
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  for (int row = 0; row < height; row++) {
    const int vramRow = wovenOriginY_ + row;
    const int partner = row + ((vramRow & 1) == 0 ? 1 : -1);
    if ((vramRow & 1) != *undrawn || partner < 0 || partner >= height) {
      continue;
    }
    SDL_GPUTextureLocation from = {};
    from.texture = woven_.texture;
    from.x = static_cast<Uint32>(source.x0 * scale_);
    from.y = static_cast<Uint32>((source.y0 + partner) * scale_);
    SDL_GPUTextureLocation to = {};
    to.texture = field_;
    to.y = static_cast<Uint32>(row * scale_);
    SDL_CopyGPUTextureToTexture(
        copy, &from, &to, static_cast<Uint32>(width * scale_), static_cast<Uint32>(scale_), 1, false);
    ++work_.textureCopies;
    work_.copyPixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(scale_ * scale_);
  }
  SDL_EndGPUCopyPass(copy);
  presented_ = {field_, {0, 0, width, height}, {woven_.drawn.x0 - source.x0, 0, woven_.drawn.x1 - source.x0, height}};
}

void RecordRasterizer::update(SDL_GPUCommandBuffer *cmd,
                              const present::FrameRecord &record,
                              bool deviceAhead,
                              std::span<const std::uint16_t> vram,
                              int scale,
                              const RecordView &view) {
  ensureImage(std::max(scale, 1));
  showCanvas(cmd, view, record.windows());
  advance(cmd, record, deviceAhead, vram);
  present(view, false, nullptr);
  selectField(cmd, record);
}

void RecordRasterizer::advance(SDL_GPUCommandBuffer *cmd,
                               const present::FrameRecord &record,
                               bool deviceAhead,
                               std::span<const std::uint16_t> vram) {
  if (chained_ && record.sequence() == applied_) {
    return;
  }
  if (chained_ && record.complete() && record.sequence() == applied_ + 1) {
    applied_ = record.sequence();
    replayed_++;
    if (record.entries().empty()) {
      return;
    }
    for (int index = 0; index <= static_cast<int>(canvases_.size()); index++) {
      const Plane &target = plane(index);
      const RecordRect copied = copiedRect(index, record.windows().drawn);
      copyTexture(cmd, target.image, copied, target.before, copied.x0, 0);
    }
    const gpu::RecordRasterPlan &plan =
        planner_.plan(record, scale_, canvasShapes(record.windows().drawn), readBack_, true, depthTest_);
    execute(cmd, plan, false);
    adoptWindows(plan, record.windows());
    readBack_ = plan.readBack;
    beforeOf_ = applied_;
    beforeHeld_ = true;
    return;
  }
  execute(cmd, planner_.planVramUpload(vram, scale_), false);
  readBack_.reset();
  for (Canvas &canvas : canvases_) {
    seedCanvas(cmd, canvas, false);
  }
  applied_ = record.sequence();
  beforeHeld_ = false;
  // Work after `record` is in the image now, so the next record cannot be applied on top of it.
  chained_ = !deviceAhead;
  resynced_++;
}

bool RecordRasterizer::drawInBetween(SDL_GPUCommandBuffer *cmd,
                                     const present::FrameRecord &record,
                                     int scale,
                                     const RecordView &view) {
  if (vram_.image == nullptr || std::max(scale, 1) != scale_ || !chained_ || !record.complete() ||
      (view.reach > 0 && canvasFor(view) == nullptr)) {
    return false;
  }
  const bool onImage = record.sequence() == applied_ + 1;
  if (!onImage && !(beforeHeld_ && record.sequence() == beforeOf_)) {
    return false;
  }
  const present::RecordWindows windows = record.windows();
  const present::HorizontalMargins margins = windows.drawn;
  for (int index = 0; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    const RecordRect copied = copiedRect(index, margins);
    copyTexture(cmd, onImage ? target.image : target.before, copied, target.inBetween, copied.x0, 0);
    if (!onImage) {
      copyTexture(cmd, target.before, copied, target.snapshot, copied.x0, 0);
    }
  }
  const gpu::RecordRasterPlan &plan =
      planner_.plan(record, scale_, canvasShapes(margins), readBack_, false, depthTest_);
  execute(cmd, plan, true);
  for (int index = 0; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    const RecordRect copied = copiedRect(index, margins);
    copyTexture(cmd, target.image, copied, target.snapshot, copied.x0, 0);
  }
  const Canvas *shown = view.reach > 0 ? canvasFor(view) : nullptr;
  const bool drewShown =
      shown != nullptr && !plan.quads[1 + static_cast<std::size_t>(shown - canvases_.data())].empty();
  present(view, true, drewShown ? &windows : nullptr);
  selectField(cmd, record);
  return true;
}

bool RecordRasterizer::show(SDL_GPUCommandBuffer *cmd,
                            const present::FrameRecord &record,
                            const present::FrameRecord *composed,
                            bool advanceToRecord,
                            bool deviceAhead,
                            std::span<const std::uint16_t> vram,
                            int scale,
                            const RecordView &view,
                            const present::DepthTest *depthTest) {
  work_ = {};
  depthTest_ = depthTest;
  if (composed != nullptr && drawInBetween(cmd, *composed, scale, view)) {
    if (advanceToRecord) {
      advance(cmd, record, deviceAhead, vram);
    }
    return true;
  }
  update(cmd, record, deviceAhead, vram, scale, view);
  return false;
}

void RecordRasterizer::copyTexture(
    SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *from, const RecordRect &source, SDL_GPUTexture *to, int toX, int toY) {
  const int s = scale_;
  if (source.x1 <= source.x0 || source.y1 <= source.y0) {
    return;
  }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureLocation origin = {};
  origin.texture = from;
  origin.x = static_cast<Uint32>(source.x0 * s);
  origin.y = static_cast<Uint32>(source.y0 * s);
  SDL_GPUTextureLocation destination = {};
  destination.texture = to;
  destination.x = static_cast<Uint32>(toX * s);
  destination.y = static_cast<Uint32>(toY * s);
  SDL_CopyGPUTextureToTexture(copy,
                              &origin,
                              &destination,
                              static_cast<Uint32>((source.x1 - source.x0) * s),
                              static_cast<Uint32>((source.y1 - source.y0) * s),
                              1,
                              false);
  SDL_EndGPUCopyPass(copy);
  ++work_.textureCopies;
  work_.copyPixels += static_cast<std::uint64_t>(source.x1 - source.x0) *
                      static_cast<std::uint64_t>(source.y1 - source.y0) * static_cast<std::uint64_t>(s * s);
}

void RecordRasterizer::ensureBuffer(Buffer &buffer, std::uint32_t bytes) {
  if (buffer.buffer != nullptr && buffer.size >= bytes) {
    return;
  }
  device_.release(buffer.buffer);
  SDL_GPUBufferCreateInfo info = {};
  info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
  info.size = std::max(bytes, buffer.size * 2);
  buffer.buffer = SDL_CreateGPUBuffer(device_.handle(), &info);
  GPUCHK(buffer.buffer, "record storage buffer");
  buffer.size = info.size;
}

void RecordRasterizer::ensureTransfer(SDL_GPUTransferBuffer *&transfer,
                                      std::uint32_t &size,
                                      SDL_GPUTransferBufferUsage usage,
                                      std::uint32_t bytes) {
  if (transfer != nullptr && size >= bytes) {
    return;
  }
  device_.release(transfer);
  size = std::max(bytes, size * 2);
  transfer = device_.makeTransferBuffer(usage, size);
}

void RecordRasterizer::capture(SDL_GPUCommandBuffer *cmd, const gpu::RecordCapture &capture) {
  const gpu::RecordRect &rect = capture.rect;
  const int width = rect.x1 - rect.x0;
  const int height = rect.y1 - rect.y0;
  if (capture_ == nullptr || captureWidth_ != width || captureHeight_ != height) {
    device_.release(capture_);
    capture_ = device_.makeTexture({width * scale_, height * scale_}, kVramFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    captureWidth_ = width;
    captureHeight_ = height;
  }
  const Plane &source = plane(capture.plane);
  copyTexture(cmd,
              source.image,
              {rect.x0 - source.originX, rect.y0 - source.originY, rect.x1 - source.originX, rect.y1 - source.originY},
              capture_,
              0,
              0);
}

void RecordRasterizer::execute(SDL_GPUCommandBuffer *cmd, const gpu::RecordRasterPlan &plan, bool inBetween) {
  // Only a plan that advances the image declares, so a capture never sees an in-between.
  const auto captureBefore = [&](std::size_t batch) {
    if (plan.capture && plan.capture->beforeBatch == batch) {
      capture(cmd, *plan.capture);
    }
  };
  if (plan.batches.empty()) {
    captureBefore(0);
    return;
  }
  std::array<std::uint32_t, gpu::kRecordPlanes> firstQuad{};
  std::size_t quadCount = 0;
  for (std::size_t p = 0; p < gpu::kRecordPlanes; p++) {
    firstQuad[p] = static_cast<std::uint32_t>(quadCount);
    quadCount += plan.quads[p].size();
  }
  const std::uint32_t quadBytes = paddedBytes(quadCount * sizeof(gpu::RecordQuad));
  const std::uint32_t opBytes = paddedBytes(plan.ops.size() * sizeof(gpu::RecordOp));
  const std::uint32_t clutBytes = paddedBytes(plan.clutPool.size() * sizeof(std::uint16_t));
  const std::uint32_t pixelBytes = paddedBytes(plan.uploadPool.size() * sizeof(std::uint16_t));
  ensureBuffer(quads_, quadBytes);
  ensureBuffer(ops_, opBytes);
  ensureBuffer(cluts_, clutBytes);
  ensureBuffer(pixels_, pixelBytes);
  ensureTransfer(
      upload_, uploadSize_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, quadBytes + opBytes + clutBytes + pixelBytes);

  auto *staging = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(device_.handle(), upload_, true));
  GPUCHK(staging, "record upload map");
  // The padding past each array is never read.
  std::uint8_t *quads = staging;
  for (std::size_t p = 0; p < gpu::kRecordPlanes; p++) {
    std::memcpy(quads, plan.quads[p].data(), plan.quads[p].size() * sizeof(gpu::RecordQuad));
    quads += plan.quads[p].size() * sizeof(gpu::RecordQuad);
  }
  std::memcpy(staging + quadBytes, plan.ops.data(), plan.ops.size() * sizeof(gpu::RecordOp));
  std::memcpy(staging + quadBytes + opBytes, plan.clutPool.data(), plan.clutPool.size() * sizeof(std::uint16_t));
  std::memcpy(staging + quadBytes + opBytes + clutBytes,
              plan.uploadPool.data(),
              plan.uploadPool.size() * sizeof(std::uint16_t));
  SDL_UnmapGPUTransferBuffer(device_.handle(), upload_);

  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  std::uint32_t offset = 0;
  for (auto [buffer, bytes] : {std::pair{quads_.buffer, quadBytes},
                               std::pair{ops_.buffer, opBytes},
                               std::pair{cluts_.buffer, clutBytes},
                               std::pair{pixels_.buffer, pixelBytes}}) {
    SDL_GPUTransferBufferLocation source = {upload_, offset};
    SDL_GPUBufferRegion destination = {buffer, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    offset += bytes;
  }
  SDL_EndGPUCopyPass(copy);
  work_.uploadBytes += quadBytes + opBytes + clutBytes + pixelBytes;

  const int s = plan.scale;
  const int planes = 1 + static_cast<int>(canvases_.size());
  // A plane any depth-tested quad lands on draws every pass of this record against its depth, cleared first.
  std::array<bool, gpu::kRecordPlanes> depthCleared{};
  std::array<bool, gpu::kRecordPlanes> usesDepth{};
  for (int p = 0; p < planes; p++) {
    const auto &quads = plan.quads[static_cast<std::size_t>(p)];
    usesDepth[static_cast<std::size_t>(p)] = std::ranges::any_of(quads, [&](const gpu::RecordQuad &quad) {
      return depthRun(plan, quad).flags != 0u;
    });
    Plane &target = plane(p);
    if (usesDepth[static_cast<std::size_t>(p)] && target.depth == nullptr) {
      target.depth = device_.makeTexture(
          {target.width * s, target.height * s}, depthFormat_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    }
  }
  for (std::size_t b = 0; b < plan.batches.size(); b++) {
    const gpu::RecordBatch &batch = plan.batches[b];
    captureBefore(b);
    for (int p = 0; p < planes; p++) {
      const gpu::RecordPlaneBatch &range = batch.planes[static_cast<std::size_t>(p)];
      if (range.endQuad == range.firstQuad) {
        continue;
      }
      const Plane &target = plane(p);
      const TargetUniform extent{static_cast<float>(target.width * s), static_cast<float>(target.height * s), {}};
      const ParamsUniform params{s, target.originX, target.originY, 0};
      SDL_PushGPUVertexUniformData(cmd, 0, &extent, sizeof(extent));
      SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));
      SDL_GPUColorTargetInfo colour = {};
      colour.texture = inBetween ? target.inBetween : target.image;
      colour.load_op = SDL_GPU_LOADOP_LOAD;
      colour.store_op = SDL_GPU_STOREOP_STORE;
      const bool depth = usesDepth[static_cast<std::size_t>(p)];
      SDL_GPUDepthStencilTargetInfo depthInfo = {};
      if (depth) {
        bool &cleared = depthCleared[static_cast<std::size_t>(p)];
        depthInfo.texture = target.depth;
        depthInfo.clear_depth = 1.0f;
        depthInfo.clear_stencil = 0;
        depthInfo.load_op = cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        depthInfo.store_op = SDL_GPU_STOREOP_STORE;
        depthInfo.stencil_load_op = depthInfo.load_op;
        depthInfo.stencil_store_op = SDL_GPU_STOREOP_STORE;
        cleared = true;
      }
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &colour, 1, depth ? &depthInfo : nullptr);
      SDL_BindGPUGraphicsPipeline(pass, depth ? slotPipeline_ : pipeline_);
      ++work_.renderPasses;
      ++work_.pipelineBinds;
      SDL_BindGPUVertexStorageBuffers(pass, 0, &quads_.buffer, 1);
      SDL_GPUSampler *nearest = device_.nearestSampler();
      // With no capture the binding is never sampled: only a held read-back plans read-back ops.
      SDL_GPUTexture *readBack = capture_ != nullptr ? capture_ : vram_.snapshot;
      const SDL_GPUTextureSamplerBinding snapshots[3] = {
          {target.snapshot, nearest}, {vram_.snapshot, nearest}, {readBack, nearest}};
      SDL_BindGPUFragmentSamplers(pass, 0, snapshots, 3);
      SDL_GPUBuffer *fragmentBuffers[3] = {ops_.buffer, cluts_.buffer, pixels_.buffer};
      SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 3);
      const auto &quads = plan.quads[static_cast<std::size_t>(p)];
      drawQuads(pass,
                plan,
                std::span(quads).subspan(range.firstQuad, range.endQuad - range.firstQuad),
                firstQuad[static_cast<std::size_t>(p)] + range.firstQuad,
                depth);
      SDL_EndGPURenderPass(pass);
    }
    // Every pass of the batch read the snapshots as they were; refresh them only now.
    for (int p = 0; p < planes; p++) {
      const gpu::RecordPlaneBatch &range = batch.planes[static_cast<std::size_t>(p)];
      const Plane &target = plane(p);
      copyTexture(cmd,
                  inBetween ? target.inBetween : target.image,
                  range.dirty,
                  target.snapshot,
                  range.dirty.x0,
                  range.dirty.y0);
    }
  }
  captureBefore(plan.batches.size());
}

void RecordRasterizer::drawQuads(SDL_GPURenderPass *pass,
                                 const gpu::RecordRasterPlan &plan,
                                 std::span<const gpu::RecordQuad> quads,
                                 std::uint32_t firstVertexQuad,
                                 bool depthTarget) {
  // Each draw is two triangles a quad, its fragments the quads' covered pixels.
  const auto tally = [&](std::span<const gpu::RecordQuad> drawn) {
    ++work_.draws;
    ++work_.pipelineBinds;
    work_.triangles += drawn.size() * 2;
    work_.pixels += countPixels_ ? coveredPixels(drawn) : 0;
  };
  if (!depthTarget) {
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(quads.size()) * 6, 1, firstVertexQuad * 6, 0);
    tally(quads);
    return;
  }
  std::span<const gpu::RecordQuad> runQuads;
  const auto draw = [&](SDL_GPUGraphicsPipeline *pipeline, Uint32 first, Uint32 count) {
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_DrawGPUPrimitives(pass, count, 1, first, 0);
    tally(runQuads);
  };
  std::size_t run = 0;
  while (run < quads.size()) {
    const DepthRun key = depthRun(plan, quads[run]);
    std::size_t end = run + 1;
    while (end < quads.size() && depthRun(plan, quads[end]) == key) {
      end++;
    }
    const auto count = static_cast<Uint32>(end - run) * 6;
    runQuads = quads.subspan(run, end - run);
    const Uint32 first = (firstVertexQuad + static_cast<std::uint32_t>(run)) * 6;
    if ((key.flags & gpu::kOpDepthModel) != 0u) {
      // Its parts' colour where they pass or cover their own model, then the depth they leave.
      SDL_SetGPUStencilReference(pass, static_cast<std::uint8_t>(kPassed | key.model));
      for (SDL_GPUGraphicsPipeline *pipeline :
           {modelTestPipeline_, modelPaintPipeline_, modelMarkPipeline_, modelClearPipeline_}) {
        draw(pipeline, first, count);
      }
    } else if ((key.flags & gpu::kOpDepthObject) != 0u) {
      draw(objectPipeline_, first, count);
    } else if ((key.flags & gpu::kOpDepthTest) != 0u) {
      // Its polygons cover no pixel twice, so each wrote its colour exactly where it passed; now its depth there.
      SDL_SetGPUStencilReference(pass, kPassed);
      draw(groupTestPipeline_, first, count);
      draw(groupMarkPipeline_, first, count);
    } else {
      draw(slotPipeline_, first, count);
    }
    run = end;
  }
}

MappedTransfer RecordRasterizer::download(const RecordPicture &picture) {
  const RecordRect &rect = picture.rect;
  const int s = std::max(scale_, 1);
  const SDL_Rect region{rect.x0 * s, rect.y0 * s, (rect.x1 - rect.x0) * s, (rect.y1 - rect.y0) * s};
  if (picture.texture == nullptr || region.w <= 0 || region.h <= 0) {
    return {};
  }
  ensureTransfer(readback_,
                 readbackSize_,
                 SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                 static_cast<std::uint32_t>(region.w) * static_cast<std::uint32_t>(region.h) * 2);
  return device_.download(picture.texture, region, readback_, "record download");
}

} // namespace psx::render
