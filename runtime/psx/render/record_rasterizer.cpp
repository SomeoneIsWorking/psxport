// record_rasterizer.cpp — the record rasterizer's GPU half: buffers, passes and the snapshot.
#include "record_rasterizer.h"

#include "psxport_generated/render_shaders.h"
#include "render_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace psx::render {
namespace {

using gpu::RecordCanvas;
using gpu::RecordRect;

constexpr SDL_GPUTextureFormat kVramFormat = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
constexpr ShaderCode kRecordVertex{spv_g_record_vert, spv_g_record_vert_len, {0, 1, 2}, PSXPORT_MSL(record_vert)};
constexpr ShaderCode kRecordFragment{spv_g_record_frag, spv_g_record_frag_len, {5, 1, 3}, PSXPORT_MSL(record_frag)};
constexpr ShaderCode kRecordLeanFragment{
    spv_g_record_lean_frag, spv_g_record_lean_frag_len, {5, 1, 3}, PSXPORT_MSL(record_lean_frag)};
// A mark: coverage and the moved-back depth alone, for a depth group or Model draw that writes no colour where the
// test draw already passed (stencil), so it never samples a texel or reads the target; both pipeline sets share it.
constexpr ShaderCode kRecordDepthMarkFragment{
    spv_g_record_depth_mark_frag, spv_g_record_depth_mark_frag_len, {5, 1, 3}, PSXPORT_MSL(record_depth_mark_frag)};
// The same reading the target in-pass (RECORD_FETCH): Metal only, so it exists only where Metal was built.
#if defined(PSXPORT_GPU_SHADERS_MSL)
constexpr ShaderCode kRecordFetchFragment{nullptr, 0, {5, 1, 3}, PSXPORT_MSL(record_fetch_frag)};
constexpr ShaderCode kRecordFetchLeanFragment{nullptr, 0, {5, 1, 3}, PSXPORT_MSL(record_fetch_lean_frag)};
#endif

// The record fragment shader `device` draws with: the in-pass reading one where it fetches its target. Depth-tested
// draws use it too: the hardware interpolates their depth (record.vert), so only a mark writes it.
const ShaderCode &recordFragment(bool fetch, bool lean) {
#if defined(PSXPORT_GPU_SHADERS_MSL)
  if (fetch) {
    return lean ? kRecordFetchLeanFragment : kRecordFetchFragment;
  }
#endif
  (void)fetch;
  return lean ? kRecordLeanFragment : kRecordFragment;
}
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

// record.frag Params.
struct ParamsUniform {
  std::int32_t scale = 1;
  std::int32_t originX = 0;
  std::int32_t originY = 0;
  std::int32_t imageScale = 1;
  std::int32_t uiScale = 0;
  std::int32_t uiOriginX = 0;
  std::int32_t uiOriginY = 0;
  std::int32_t unused = 0;
  std::int32_t outlineScale = 0;
  std::array<std::int32_t, 3> unusedOutline{};
  std::array<std::int32_t, 4> outlineFaces{};
  // Each outline font's first texel across VRAM and its atlas row, then its size (present::OutlineAtlasFont).
  std::array<std::array<std::int32_t, 4>, present::kMaxOutlineFonts> outlineFontPlaces{};
  std::array<std::array<std::int32_t, 4>, present::kMaxOutlineFonts> outlineFontSizes{};
};

// A triangle's pixels are its area; a rect's every pixel.
std::uint64_t coveredPixels(std::span<const gpu::RecordShape> shapes) {
  double pixels = 0.0;
  for (const gpu::RecordShape &shape : shapes) {
    if (shape.kind == gpu::kShapeTriangle) {
      const gpu::RecordCorner &a = shape.corners[0];
      const gpu::RecordCorner &b = shape.corners[1];
      const gpu::RecordCorner &c = shape.corners[2];
      pixels += std::abs(static_cast<double>((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y))) / 2.0;
    } else {
      pixels +=
          static_cast<double>(std::max(0, shape.x1 - shape.x0)) * static_cast<double>(std::max(0, shape.y1 - shape.y0));
    }
  }
  return static_cast<std::uint64_t>(pixels);
}

// Hardware triangles a shape draws.
std::uint64_t shapeTriangles(std::span<const gpu::RecordShape> shapes) {
  std::uint64_t triangles = 0;
  for (const gpu::RecordShape &shape : shapes) {
    triangles += shape.kind == gpu::kShapeTriangle ? 1 : 2;
  }
  return triangles;
}

// Depth is 1/z (record.vert), down to 1/65536: a float keeps its relative precision there, where 24 fixed bits resolve
// z only to about 0.4% at SZ's far end.
SDL_GPUTextureFormat depthFormat(SDL_GPUDevice *device) {
  return SDL_GPUTextureSupportsFormat(device,
                                      SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
             ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
             : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
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

// A shape's depth flags and, for a Model part, its model's stencil id: consecutive shapes with the same draw as one
// run. Whether the op reads the target pixel: record.frag fetches it only for these.
bool readsTarget(const gpu::RecordRasterPlan &plan, const gpu::RecordShape &shape) {
  return (plan.ops[shape.op][gpu::kOpFlags] & (gpu::kOpSemi | gpu::kOpMaskCheck)) != 0u;
}

// Whether the op needs the full record.frag: a copy, an upload, or texels from the read-back or the outline atlas.
bool needsFull(const gpu::RecordRasterPlan &plan, const gpu::RecordShape &shape) {
  const std::uint32_t flags = plan.ops[shape.op][gpu::kOpFlags];
  const auto kind = static_cast<gpu::RecordOpKind>(flags & gpu::kOpKindMask);
  return kind == gpu::RecordOpKind::Copy || kind == gpu::RecordOpKind::Upload ||
         (flags & (gpu::kOpReadBack | gpu::kOpOutline)) != 0u;
}

struct DepthRun {
  std::uint32_t flags = 0;
  std::uint32_t model = 0;
  bool reads = false;
  bool full = false;
  bool operator==(const DepthRun &) const = default;
};

// With a depth target, a run also shares its depth flags and Model.
DepthRun depthRun(const gpu::RecordRasterPlan &plan, const gpu::RecordShape &shape, bool depthTarget) {
  const std::uint32_t flags = depthTarget ? plan.ops[shape.op][gpu::kOpFlags] & kDepthRunFlags : 0u;
  return {flags,
          (flags & gpu::kOpDepthModel) != 0u ? plan.ops[shape.op + 1][gpu::kDepthModel] : 0u,
          readsTarget(plan, shape),
          needsFull(plan, shape)};
}

// Storage buffers are read as whole words, and never sized zero.
std::uint32_t paddedBytes(std::size_t bytes) {
  return static_cast<std::uint32_t>(std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 16));
}

} // namespace

RecordRasterizer::RecordRasterizer(RenderDevice &device) : device_(device), depthFormat_(depthFormat(device.handle())) {
  planner_.setDestinationReadsInPass(device.fetchesTarget());
  fetches_ = device.fetchesTarget();
  for (int set = 0; set < (fetches_ ? 4 : 2); set++) {
    sets_.at(static_cast<std::size_t>(set)) = makePipelines(set >= 2, (set & 1) == 0);
  }
  canvases_.reserve(gpu::kRecordMaxCanvases);
}

RecordRasterizer::Pipelines RecordRasterizer::makePipelines(bool fetch, bool lean) {
  const ShaderCode &fragment = recordFragment(fetch, lean);
  Pipelines pipelines;
  pipelines.plain = device_.makePipeline(kRecordVertex, fragment, kVramFormat);
  const RenderDevice::DepthTarget slot = depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, false);
  // Depth is 1/z, nearer larger (record.vert).
  const RenderDevice::DepthTarget tests =
      withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, false),
                  SDL_GPU_COMPAREOP_ALWAYS,
                  SDL_GPU_STENCILOP_REPLACE);
  const RenderDevice::DepthTarget marks = withStencil(
      depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, true), SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_STENCILOP_ZERO);
  // An Object's depth is no Model's.
  const RenderDevice::DepthTarget covers = withStencil(
      depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, true), SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_STENCILOP_ZERO);
  // A Model part tests where another's depth lies and paints over its own model's, marking where it drew.
  const RenderDevice::DepthTarget modelTests =
      withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, false),
                  SDL_GPU_COMPAREOP_NOT_EQUAL,
                  SDL_GPU_STENCILOP_REPLACE,
                  {.compare = kModelBits, .write = kPassed});
  const RenderDevice::DepthTarget modelPaints = withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_ALWAYS, false),
                                                            SDL_GPU_COMPAREOP_EQUAL,
                                                            SDL_GPU_STENCILOP_REPLACE,
                                                            {.compare = kModelBits, .write = kPassed});
  // Where it drew it leaves the nearer of its moved-back depth and the depth there, and names its model where its own
  // is kept.
  const RenderDevice::DepthTarget leaves =
      withStencil(depthTarget(depthFormat_, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, true),
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
  pipelines.slot = device_.makePipeline(kRecordVertex, fragment, kVramFormat, nullptr, nullptr, &slot);
  pipelines.groupTest = device_.makePipeline(kRecordVertex, fragment, kVramFormat, nullptr, nullptr, &tests);
  pipelines.groupMark =
      device_.makePipeline(kRecordVertex, kRecordDepthMarkFragment, kVramFormat, &noColour, nullptr, &marks);
  pipelines.object = device_.makePipeline(kRecordVertex, fragment, kVramFormat, nullptr, nullptr, &covers);
  pipelines.modelTest = device_.makePipeline(kRecordVertex, fragment, kVramFormat, nullptr, nullptr, &modelTests);
  pipelines.modelPaint = device_.makePipeline(kRecordVertex, fragment, kVramFormat, nullptr, nullptr, &modelPaints);
  pipelines.modelMark =
      device_.makePipeline(kRecordVertex, kRecordDepthMarkFragment, kVramFormat, &noColour, nullptr, &leaves);
  pipelines.modelClear =
      device_.makePipeline(kRecordVertex, kRecordDepthMarkFragment, kVramFormat, &noColour, nullptr, &clears);
  return pipelines;
}

void RecordRasterizer::release(Pipelines &pipelines) {
  for (SDL_GPUGraphicsPipeline **pipeline : {&pipelines.plain,
                                             &pipelines.slot,
                                             &pipelines.groupTest,
                                             &pipelines.groupMark,
                                             &pipelines.object,
                                             &pipelines.modelTest,
                                             &pipelines.modelPaint,
                                             &pipelines.modelMark,
                                             &pipelines.modelClear}) {
    device_.release(*pipeline);
  }
}

RecordRasterizer::~RecordRasterizer() {
  releaseImage();
  device_.release(outlineFields_);
  device_.release(outlineIndices_);
  for (Buffer *buffer : {&shapeBuffer_, &ops_, &cluts_, &pixels_}) {
    device_.release(buffer->buffer);
  }
  device_.release(upload_);
  device_.release(readback_);
  for (Pipelines &set : sets_) {
    release(set);
  }
}

RecordRasterizer::Plane RecordRasterizer::makePlane(const RecordCanvas &shape, int scale) {
  const PixelSize scaled{shape.width() * scale, shape.height() * scale};
  constexpr SDL_GPUTextureUsageFlags kTarget = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  Plane plane;
  plane.image = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.snapshot = device_.makeTexture(scaled, kVramFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  plane.inBetween = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.before = device_.makeTexture(scaled, kVramFormat, kTarget);
  plane.width = shape.width();
  plane.height = shape.height();
  plane.scale = scale;
  plane.originX = shape.originX();
  plane.originY = shape.originY();
  const RecordRect whole{0, 0, plane.width, plane.height};
  for (Region *region : {&plane.beforeDiffers, &plane.inBetweenDiffers, &plane.snapshotDiffers}) {
    region->add(whole);
  }
  return plane;
}

namespace {

std::int64_t area(const RecordRect &rect) {
  return static_cast<std::int64_t>(rect.x1 - rect.x0) * static_cast<std::int64_t>(rect.y1 - rect.y0);
}

RecordRect bound(const RecordRect &a, const RecordRect &b) {
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

bool touches(const RecordRect &a, const RecordRect &b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

} // namespace

void RecordRasterizer::Region::add(RecordRect rect) {
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return;
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (std::size_t i = 0; i < rects_.size(); i++) {
      if (touches(rects_[i], rect)) {
        rect = bound(rects_[i], rect);
        rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(i));
        grew = true;
        break;
      }
    }
  }
  rects_.push_back(rect);
  while (rects_.size() > kMaxRects) {
    std::size_t first = 0;
    std::size_t second = 1;
    std::int64_t least = INT64_MAX;
    for (std::size_t i = 0; i < rects_.size(); i++) {
      for (std::size_t j = i + 1; j < rects_.size(); j++) {
        const std::int64_t added = area(bound(rects_[i], rects_[j])) - area(rects_[i]) - area(rects_[j]);
        if (added < least) {
          least = added;
          first = i;
          second = j;
        }
      }
    }
    const RecordRect merged = bound(rects_[first], rects_[second]);
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(second));
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(first));
    add(merged);
  }
}

void RecordRasterizer::Region::add(const Region &other) {
  for (const RecordRect &rect : other.rects_) {
    add(rect);
  }
}

void RecordRasterizer::releasePlane(Plane &plane) {
  for (SDL_GPUTexture **texture : {&plane.image, &plane.snapshot, &plane.inBetween, &plane.before, &plane.depth}) {
    device_.release(*texture);
  }
  plane = {};
}

void RecordRasterizer::useOutline(const present::OutlineAtlas *atlas, const std::array<std::uint32_t, 4> &faces) {
  outlineFaces_ = faces;
  if (atlas == nullptr || atlas->generation == outlineGeneration_) {
    return;
  }
  device_.release(outlineFields_);
  device_.release(outlineIndices_);
  outlineFields_ = device_.makeFilledTexture({atlas->fieldWidth, atlas->fieldHeight}, atlas->fields);
  outlineIndices_ = device_.makeFilledTexture({atlas->width * 2, atlas->height}, atlas->indices);
  outlineGeneration_ = atlas->generation;
  outlineScale_ = atlas->scale;
  outlineFonts_ = atlas->fonts;
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
  vram_ = makePlane(RecordCanvas{.buffer = {0, 0, gpu::kVramWidth, gpu::kVramHeight}}, scale_);
  chained_ = false;
  beforeHeld_ = false;
}

RecordCanvas RecordRasterizer::viewShape(const RecordView &view) const {
  if (view.crop.active()) {
    return gpu::cropCanvas(view.display, view.crop.p, view.crop.q, present::cropUnit(view.crop, scale_));
  }
  return RecordCanvas{.buffer = view.display, .reach = view.reach};
}

RecordRasterizer::Canvas *RecordRasterizer::canvasFor(const RecordView &view) {
  if (view.reach <= 0 && !view.crop.active()) {
    return nullptr;
  }
  const RecordCanvas shape = viewShape(view);
  for (Canvas &canvas : canvases_) {
    // A margin canvas's window changes with the records drawing it; its allocation does not.
    if (canvas.shape.crop() ? canvas.shape == shape
                            : canvas.shape.buffer == shape.buffer && canvas.shape.reach == shape.reach) {
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

bool RecordRasterizer::drewCanvas(const gpu::RecordRasterPlan &plan, std::size_t index) const {
  if (!plan.shapes[1 + index].empty()) {
    return true;
  }
  const Canvas &canvas = canvases_[index];
  return !canvas.shape.crop() && std::ranges::any_of(plan.batches, [&](const gpu::RecordBatch &batch) {
    const RecordRect &dirty = batch.planes[0].dirty;
    const RecordRect &buffer = canvas.shape.buffer;
    return dirty.x0 < buffer.x1 && buffer.x0 < dirty.x1 && dirty.y0 < buffer.y1 && buffer.y0 < dirty.y1;
  });
}

void RecordRasterizer::copyInteriors(const RecordRect &written, bool inBetween) {
  for (Canvas &canvas : canvases_) {
    if (canvas.shape.crop()) {
      continue;
    }
    const RecordRect &buffer = canvas.shape.buffer;
    const RecordRect inside{std::max(written.x0, buffer.x0),
                            std::max(written.y0, buffer.y0),
                            std::min(written.x1, buffer.x1),
                            std::min(written.y1, buffer.y1)};
    copyTexture({.from = inBetween ? vram_.inBetween : vram_.image,
                 .source = inside,
                 .to = inBetween ? canvas.plane.inBetween : canvas.plane.image,
                 .toX = inside.x0 - canvas.plane.originX,
                 .toY = inside.y0 - canvas.plane.originY,
                 .scale = scale_});
  }
}

void RecordRasterizer::adoptWindows(const gpu::RecordRasterPlan &plan, const present::RecordWindows &windows) {
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    if (drewCanvas(plan, i)) {
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

std::array<RecordRect, 2> RecordRasterizer::copiedStrips(int index, present::HorizontalMargins margins) {
  const RecordRect copied = copiedRect(index, margins);
  const Canvas &canvas = canvases_[static_cast<std::size_t>(index - 1)];
  if (canvas.shape.crop()) {
    return {copied, RecordRect{}};
  }
  const int x0 = canvas.shape.buffer.x0 - canvas.plane.originX;
  const int x1 = canvas.shape.buffer.x1 - canvas.plane.originX;
  return {RecordRect{copied.x0, copied.y0, std::min(copied.x1, x0), copied.y1},
          RecordRect{std::max(copied.x0, x1), copied.y0, copied.x1, copied.y1}};
}

RecordRasterizer::Plane &RecordRasterizer::plane(int index) {
  return index == 0 ? vram_ : canvases_[static_cast<std::size_t>(index - 1)].plane;
}

void RecordRasterizer::showCanvas(const RecordView &view, const present::RecordWindows &windows) {
  const int width = view.display.x1 - view.display.x0;
  const int height = view.display.y1 - view.display.y0;
  const bool crop = view.crop.active();
  const RecordCanvas shape = viewShape(view);
  // A widening, crop or display width change makes every canvas another shape; a split change keeps them.
  std::erase_if(canvases_, [&](Canvas &canvas) {
    const bool stale =
        crop ? !(canvas.shape.crop() && canvas.shape.worldScale == shape.worldScale &&
                 canvas.shape.uiScale == shape.uiScale && canvas.shape.buffer.x1 - canvas.shape.buffer.x0 == width)
             : !gpu::canvasSurvives(canvas.shape, width, view.reach);
    if (stale) {
      releasePlane(canvas.plane);
    }
    return stale;
  });
  if ((view.reach <= 0 && !crop) || width <= 0 || height <= 0 || shape.height() <= 0) {
    return;
  }
  Canvas *canvas = canvasFor(view);
  if (canvas == nullptr) {
    canvas = &addCanvas(shape, windows);
  }
  canvas->shown = ++shows_;
}

RecordRasterizer::Canvas &RecordRasterizer::addCanvas(const RecordCanvas &shape,
                                                      const present::RecordWindows &windows) {
  if (canvases_.size() == static_cast<std::size_t>(gpu::kRecordMaxCanvases)) {
    auto oldest = std::min_element(canvases_.begin(), canvases_.end(), [](const Canvas &a, const Canvas &b) {
      return a.shown < b.shown;
    });
    releasePlane(oldest->plane);
    canvases_.erase(oldest);
  }
  const bool crop = shape.crop();
  Canvas added;
  added.shape = shape;
  if (!crop) {
    setWindow(added.shape, windows.drawn);
    added.view = windows.view;
  }
  added.plane = makePlane(added.shape, crop ? shape.worldScale : scale_);
  canvases_.push_back(added);
  Canvas &canvas = canvases_.back();
  if (crop) {
    seedCrop(canvas);
  } else {
    seedCanvas(canvas, true);
  }
  return canvas;
}

void RecordRasterizer::seedCanvas(Canvas &canvas, bool fresh) {
  for (auto [from, to] : {std::pair{vram_.image, canvas.plane.image}, std::pair{vram_.before, canvas.plane.before}}) {
    if (fresh) {
      SDL_GPUColorTargetInfo clear = {};
      clear.texture = to;
      clear.load_op = SDL_GPU_LOADOP_CLEAR;
      clear.store_op = SDL_GPU_STOREOP_STORE;
      SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd_, &clear, 1, nullptr));
      ++work_.renderPasses;
    }
    copyTexture({.from = from, .source = canvas.shape.buffer, .to = to, .toX = canvas.shape.reach, .scale = scale_});
  }
  const RecordRect seeded = fresh ? RecordRect{0, 0, canvas.plane.width, canvas.plane.height}
                                  : RecordRect{canvas.shape.reach,
                                               0,
                                               canvas.shape.reach + canvas.shape.buffer.x1 - canvas.shape.buffer.x0,
                                               canvas.plane.height};
  copyTexture(
      {.from = canvas.plane.image, .source = seeded, .to = canvas.plane.snapshot, .toX = seeded.x0, .scale = scale_});
}

void RecordRasterizer::seedCrop(Canvas &canvas) {
  const auto index = static_cast<std::size_t>(&canvas - canvases_.data());
  std::array<RecordCanvas, gpu::kRecordMaxCanvases> shapes{};
  shapes[index] = canvas.shape;
  execute(planner_.planCropSeed(scale_, std::span(shapes.data(), canvases_.size())), false);
  const RecordRect whole{0, 0, canvas.plane.width, canvas.plane.height};
  copyTexture({.from = canvas.plane.image, .source = whole, .to = canvas.plane.before, .scale = canvas.plane.scale});
}

void RecordRasterizer::present(const RecordView &view, bool inBetween, const present::RecordWindows *windows) {
  Canvas *canvas = canvasFor(view);
  if (canvas != nullptr && canvas->shape.crop()) {
    const RecordRect whole{0, 0, canvas->plane.width, canvas->plane.height};
    presented_ = {inBetween ? canvas->plane.inBetween : canvas->plane.image, whole, whole, canvas->plane.scale};
    wovenOriginY_ = canvas->plane.originY;
  } else if (canvas != nullptr) {
    const present::RecordWindows shown =
        windows != nullptr ? *windows : present::RecordWindows{canvas->view, {canvas->shape.left, canvas->shape.right}};
    presented_ = {inBetween ? canvas->plane.inBetween : canvas->plane.image,
                  windowOf(canvas->shape, shown.view),
                  windowOf(canvas->shape, shown.drawn),
                  scale_};
    wovenOriginY_ = canvas->plane.originY;
  } else {
    presented_ = {inBetween ? vram_.inBetween : vram_.image, view.display, view.display, scale_};
    wovenOriginY_ = view.display.y0;
  }
  woven_ = presented_;
}

void RecordRasterizer::selectField(const present::FrameRecord &record) {
  const std::optional<int> undrawn = record.undrawnRowParity();
  const RecordRect source = woven_.rect;
  const int width = source.x1 - source.x0;
  const int height = source.y1 - source.y0;
  // A crop canvas draws every row of a record, so it holds one field already.
  if (!undrawn || woven_.texture == nullptr || woven_.scale != scale_ || width <= 0 || height <= 0) {
    return;
  }
  if (field_ == nullptr || fieldWidth_ != width || fieldHeight_ != height) {
    device_.release(field_);
    field_ = device_.makeTexture({width * scale_, height * scale_}, kVramFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    fieldWidth_ = width;
    fieldHeight_ = height;
  }
  copyTexture({.from = woven_.texture, .source = source, .to = field_, .scale = scale_});
  // Each undrawn row repeats the drawn row of its pair, so the picture is one field and never two moments.
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_);
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
  presented_ = {
      field_, {0, 0, width, height}, {woven_.drawn.x0 - source.x0, 0, woven_.drawn.x1 - source.x0, height}, scale_};
}

void RecordRasterizer::update(const present::FrameRecord &record,
                              bool deviceAhead,
                              std::span<const std::uint16_t> vram,
                              int scale,
                              const RecordView &view) {
  ensureImage(std::max(scale, 1));
  showCanvas(view, record.windows());
  advance(record, deviceAhead, vram);
  present(view, false, nullptr);
  selectField(record);
}

void RecordRasterizer::advance(const present::FrameRecord &record,
                               bool deviceAhead,
                               std::span<const std::uint16_t> vram) {
  if (chained_ && record.sequence() == applied_) {
    return;
  }
  if (chained_ && record.complete() && record.sequence() == applied_ + 1) {
    applied_ = record.sequence();
    replayed_++;
    if (!record.entries().empty()) {
      apply(record);
    }
    return;
  }
  // Every canvas is a crop canvas or none is: a crop canvas takes the upload, a margin canvas the image's columns.
  const bool crops = !canvases_.empty() && canvases_.front().shape.crop();
  execute(planner_.planVramUpload(vram, scale_, crops ? canvasShapes({}) : std::span<const RecordCanvas>{}), false);
  readBack_.reset();
  for (Canvas &canvas : canvases_) {
    if (crops) {
      const RecordRect whole{0, 0, canvas.plane.width, canvas.plane.height};
      copyTexture(
          {.from = canvas.plane.image, .source = whole, .to = canvas.plane.before, .scale = canvas.plane.scale});
    } else {
      seedCanvas(canvas, false);
    }
  }
  applied_ = record.sequence();
  beforeHeld_ = false;
  // Work after `record` is in the image now, so the next record cannot be applied on top of it.
  chained_ = !deviceAhead;
  resynced_++;
}

void RecordRasterizer::apply(const present::FrameRecord &record) {
  copyRegion(vram_.image, vram_.before, vram_.beforeDiffers, vram_.scale);
  vram_.beforeDiffers.clear();
  for (int index = 1; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    for (const RecordRect &copied : copiedStrips(index, record.windows().drawn)) {
      copyTexture(
          {.from = target.image, .source = copied, .to = target.before, .toX = copied.x0, .scale = target.scale});
    }
  }
  stage("record keep before");
  const gpu::RecordRasterPlan &plan =
      planner_.plan(record, scale_, canvasShapes(record.windows().drawn), readBack_, true, depthTest_);
  execute(plan, false);
  adoptWindows(plan, record.windows());
  readBack_ = plan.readBack;
  beforeOf_ = record.sequence();
  beforeHeld_ = true;
}

void RecordRasterizer::redrawRestored(std::span<const std::uint16_t> vram, const RecordView &view) {
  restorePending_ = false;
  const std::vector<present::FrameRecord> records = std::move(restoredRecords_);
  const std::vector<CanvasWindows> canvases = std::move(restoredCanvases_);
  restoredRecords_.clear();
  restoredCanvases_.clear();
  // The records drawn again over the VRAM they left, at scale: what each covers shows as the saving run drew it.
  execute(planner_.planVramUpload(vram, scale_, {}), false);
  readBack_.reset();
  resynced_++;
  if (view.reach > 0 && !view.crop.active()) {
    for (const CanvasWindows &carried : canvases) {
      const RecordCanvas shape{.buffer = carried.buffer, .reach = view.reach};
      if (canvasFor({carried.buffer, view.reach, {}}) == nullptr &&
          gpu::canvasSurvives(shape, view.display.x1 - view.display.x0, view.reach)) {
        addCanvas(shape, {carried.view, carried.drawn}).shown = ++shows_;
      }
    }
  }
  if (records.empty() || !std::all_of(records.begin(), records.end(), [](const present::FrameRecord &record) {
        return record.complete();
      })) {
    chained_ = false;
    return;
  }
  showCanvas(view, records.back().windows());
  for (const present::FrameRecord &record : records) {
    if (!record.entries().empty()) {
      apply(record);
    }
  }
  applied_ = records.back().sequence();
  chained_ = true;
}

bool RecordRasterizer::drawInBetween(const present::FrameRecord &record, int scale, const RecordView &view) {
  if (vram_.image == nullptr || std::max(scale, 1) != scale_ || !chained_ || !record.complete() ||
      ((view.reach > 0 || view.crop.active()) && canvasFor(view) == nullptr)) {
    return false;
  }
  const bool onImage = record.sequence() == applied_ + 1;
  if (!onImage && !(beforeHeld_ && record.sequence() == beforeOf_)) {
    return false;
  }
  const present::RecordWindows windows = record.windows();
  const present::HorizontalMargins margins = windows.drawn;
  if (onImage) {
    copyRegion(vram_.image, vram_.inBetween, vram_.inBetweenDiffers, vram_.scale);
    vram_.inBetweenDiffers.clear();
  } else {
    // The kept state differs from the image only where the last record drew.
    vram_.inBetweenDiffers.add(vram_.beforeDiffers);
    copyRegion(vram_.before, vram_.inBetween, vram_.inBetweenDiffers, vram_.scale);
    vram_.inBetweenDiffers = vram_.beforeDiffers;
    vram_.snapshotDiffers.add(vram_.beforeDiffers);
    copyRegion(vram_.before, vram_.snapshot, vram_.snapshotDiffers, vram_.scale);
    vram_.snapshotDiffers = vram_.beforeDiffers;
  }
  for (int index = 1; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    for (const RecordRect &copied : copiedStrips(index, margins)) {
      copyTexture({.from = onImage ? target.image : target.before,
                   .source = copied,
                   .to = target.inBetween,
                   .toX = copied.x0,
                   .scale = target.scale});
      if (!onImage) {
        copyTexture(
            {.from = target.before, .source = copied, .to = target.snapshot, .toX = copied.x0, .scale = target.scale});
      }
    }
  }
  stage("record in-between seed");
  const gpu::RecordRasterPlan &plan =
      planner_.plan(record, scale_, canvasShapes(margins), readBack_, false, depthTest_);
  execute(plan, true);
  copyRegion(vram_.image, vram_.snapshot, vram_.snapshotDiffers, vram_.scale);
  vram_.snapshotDiffers.clear();
  for (int index = 1; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    for (const RecordRect &copied : copiedStrips(index, margins)) {
      copyTexture(
          {.from = target.image, .source = copied, .to = target.snapshot, .toX = copied.x0, .scale = target.scale});
    }
  }
  stage("record snapshot restore");
  const Canvas *shown = canvasFor(view);
  const bool drewShown = shown != nullptr && drewCanvas(plan, static_cast<std::size_t>(shown - canvases_.data()));
  present(view, true, drewShown ? &windows : nullptr);
  selectField(record);
  return true;
}

bool RecordRasterizer::show(SDL_GPUCommandBuffer *&cmd,
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
  cmd_ = cmd;
  if (restorePending_) {
    ensureImage(std::max(scale, 1));
    redrawRestored(vram, view);
  }
  bool drewComposed = false;
  if (composed != nullptr && drawInBetween(*composed, scale, view)) {
    if (advanceToRecord) {
      advance(record, deviceAhead, vram);
    }
    drewComposed = true;
  } else {
    update(record, deviceAhead, vram, scale, view);
  }
  cmd = cmd_;
  cmd_ = nullptr;
  return drewComposed;
}

void RecordRasterizer::stage(const char *label) {
  if (!device_.timingSubmits() || cmd_ == nullptr) {
    return;
  }
  device_.submit(cmd_, label);
  cmd_ = device_.acquire();
}

void RecordRasterizer::copyRegion(SDL_GPUTexture *from, SDL_GPUTexture *to, const Region &region, int scale) {
  for (const RecordRect &rect : region.rects()) {
    copyTexture({.from = from, .source = rect, .to = to, .toX = rect.x0, .toY = rect.y0, .scale = scale});
  }
}

void RecordRasterizer::copyTexture(const TextureCopy &copy) {
  const RecordRect &source = copy.source;
  const int s = copy.scale;
  if (source.x1 <= source.x0 || source.y1 <= source.y0) {
    return;
  }
  SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd_);
  SDL_GPUTextureLocation origin = {};
  origin.texture = copy.from;
  origin.x = static_cast<Uint32>(source.x0 * s);
  origin.y = static_cast<Uint32>(source.y0 * s);
  SDL_GPUTextureLocation destination = {};
  destination.texture = copy.to;
  destination.x = static_cast<Uint32>(copy.toX * s);
  destination.y = static_cast<Uint32>(copy.toY * s);
  SDL_CopyGPUTextureToTexture(pass,
                              &origin,
                              &destination,
                              static_cast<Uint32>((source.x1 - source.x0) * s),
                              static_cast<Uint32>((source.y1 - source.y0) * s),
                              1,
                              false);
  SDL_EndGPUCopyPass(pass);
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

void RecordRasterizer::capture(const gpu::RecordCapture &capture) {
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
  copyTexture({.from = source.image,
               .source = {rect.x0 - source.originX,
                          rect.y0 - source.originY,
                          rect.x1 - source.originX,
                          rect.y1 - source.originY},
               .to = capture_,
               .scale = source.scale});
}

void RecordRasterizer::execute(const gpu::RecordRasterPlan &plan, bool inBetween) {
  // The image's pixels the plan wrote so far, which margin canvases take into their buffer columns.
  RecordRect written{gpu::kVramWidth, gpu::kVramHeight, 0, 0};
  // Only a plan that advances the image declares, so a capture never sees an in-between.
  const auto captureBefore = [&](std::size_t batch) {
    if (plan.capture && plan.capture->beforeBatch == batch) {
      copyInteriors(written, false);
      capture(*plan.capture);
    }
  };
  if (plan.batches.empty()) {
    captureBefore(0);
    return;
  }
  std::array<std::uint32_t, gpu::kRecordPlanes> firstShape{};
  std::size_t shapeCount = 0;
  for (std::size_t p = 0; p < gpu::kRecordPlanes; p++) {
    firstShape[p] = static_cast<std::uint32_t>(shapeCount);
    shapeCount += plan.shapes[p].size();
  }
  const std::uint32_t shapeBytes = paddedBytes(shapeCount * sizeof(gpu::RecordShape));
  const std::uint32_t opBytes = paddedBytes(plan.ops.size() * sizeof(gpu::RecordOp));
  const std::uint32_t clutBytes = paddedBytes(plan.clutPool.size() * sizeof(std::uint16_t));
  const std::uint32_t pixelBytes = paddedBytes(plan.uploadPool.size() * sizeof(std::uint16_t));
  ensureBuffer(shapeBuffer_, shapeBytes);
  ensureBuffer(ops_, opBytes);
  ensureBuffer(cluts_, clutBytes);
  ensureBuffer(pixels_, pixelBytes);
  ensureTransfer(
      upload_, uploadSize_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, shapeBytes + opBytes + clutBytes + pixelBytes);

  auto *staging = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(device_.handle(), upload_, true));
  GPUCHK(staging, "record upload map");
  // The padding past each array is never read.
  std::uint8_t *shapes = staging;
  for (std::size_t p = 0; p < gpu::kRecordPlanes; p++) {
    std::memcpy(shapes, plan.shapes[p].data(), plan.shapes[p].size() * sizeof(gpu::RecordShape));
    shapes += plan.shapes[p].size() * sizeof(gpu::RecordShape);
  }
  std::memcpy(staging + shapeBytes, plan.ops.data(), plan.ops.size() * sizeof(gpu::RecordOp));
  std::memcpy(staging + shapeBytes + opBytes, plan.clutPool.data(), plan.clutPool.size() * sizeof(std::uint16_t));
  std::memcpy(staging + shapeBytes + opBytes + clutBytes,
              plan.uploadPool.data(),
              plan.uploadPool.size() * sizeof(std::uint16_t));
  SDL_UnmapGPUTransferBuffer(device_.handle(), upload_);

  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_);
  std::uint32_t offset = 0;
  for (auto [buffer, bytes] : {std::pair{shapeBuffer_.buffer, shapeBytes},
                               std::pair{ops_.buffer, opBytes},
                               std::pair{cluts_.buffer, clutBytes},
                               std::pair{pixels_.buffer, pixelBytes}}) {
    SDL_GPUTransferBufferLocation source = {upload_, offset};
    SDL_GPUBufferRegion destination = {buffer, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    offset += bytes;
  }
  SDL_EndGPUCopyPass(copy);
  work_.uploadBytes += shapeBytes + opBytes + clutBytes + pixelBytes;
  stage("record upload");

  const int planes = 1 + static_cast<int>(canvases_.size());
  // A plane any depth-tested shape lands on draws every pass of this record against its depth, cleared first.
  std::array<bool, gpu::kRecordPlanes> depthCleared{};
  std::array<bool, gpu::kRecordPlanes> usesDepth{};
  // The depth is the record's alone: only a pass with a later one on its plane keeps it, so a tile-based GPU never
  // writes the whole plane's depth and stencil back to memory for nothing.
  std::array<std::size_t, gpu::kRecordPlanes> lastPass{};
  for (std::size_t b = 0; b < plan.batches.size(); b++) {
    for (std::size_t p = 0; p < gpu::kRecordPlanes; p++) {
      const gpu::RecordPlaneBatch &range = plan.batches[b].planes.at(p);
      lastPass.at(p) = range.endShape > range.firstShape ? b : lastPass.at(p);
    }
  }
  for (int p = 0; p < planes; p++) {
    const auto &shapes = plan.shapes[static_cast<std::size_t>(p)];
    usesDepth[static_cast<std::size_t>(p)] = std::ranges::any_of(shapes, [&](const gpu::RecordShape &shape) {
      return depthRun(plan, shape, true).flags != 0u;
    });
    Plane &target = plane(p);
    if (usesDepth[static_cast<std::size_t>(p)] && target.depth == nullptr) {
      target.depth = device_.makeTexture({target.width * target.scale, target.height * target.scale},
                                         depthFormat_,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    }
  }
  for (std::size_t b = 0; b < plan.batches.size(); b++) {
    const gpu::RecordBatch &batch = plan.batches[b];
    captureBefore(b);
    for (int p = 0; p < planes; p++) {
      const gpu::RecordPlaneBatch &range = batch.planes[static_cast<std::size_t>(p)];
      if (range.endShape == range.firstShape) {
        continue;
      }
      const Plane &target = plane(p);
      const int s = target.scale;
      const TargetUniform extent{static_cast<float>(target.width * s), static_cast<float>(target.height * s), {}};
      ParamsUniform params{.scale = s, .originX = target.originX, .originY = target.originY, .imageScale = plan.scale};
      if (outlineFields_ != nullptr) {
        params.outlineScale = outlineScale_;
        for (std::size_t f = 0; f < outlineFonts_.size(); f++) {
          const present::OutlineAtlasFont &font = outlineFonts_.at(f);
          params.outlineFontPlaces.at(f) = {font.originX, font.originY, font.atlasY, 0};
          params.outlineFontSizes.at(f) = {font.width, font.height, 0, 0};
        }
        for (std::size_t slot = 0; slot < outlineFaces_.size(); slot++) {
          params.outlineFaces.at(slot) = static_cast<std::int32_t>(outlineFaces_.at(slot));
        }
      }
      if (p > 0 && canvases_[static_cast<std::size_t>(p - 1)].shape.crop()) {
        const RecordCanvas &shape = canvases_[static_cast<std::size_t>(p - 1)].shape;
        params.uiScale = shape.uiScale;
        params.uiOriginX = shape.uiOriginX;
        params.uiOriginY = shape.uiOriginY;
      }
      SDL_PushGPUVertexUniformData(cmd_, 0, &extent, sizeof(extent));
      SDL_PushGPUFragmentUniformData(cmd_, 0, &params, sizeof(params));
      SDL_GPUColorTargetInfo colour = {};
      colour.texture = inBetween ? target.inBetween : target.image;
      colour.load_op = SDL_GPU_LOADOP_LOAD;
      colour.store_op = SDL_GPU_STOREOP_STORE;
      const bool depth = usesDepth[static_cast<std::size_t>(p)];
      SDL_GPUDepthStencilTargetInfo depthInfo = {};
      if (depth) {
        bool &cleared = depthCleared[static_cast<std::size_t>(p)];
        depthInfo.texture = target.depth;
        depthInfo.clear_depth = 0.0f; // 1/z: nothing drawn lies infinitely far
        depthInfo.clear_stencil = 0;
        depthInfo.load_op = cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        depthInfo.store_op =
            b < lastPass.at(static_cast<std::size_t>(p)) ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        depthInfo.stencil_load_op = depthInfo.load_op;
        depthInfo.stencil_store_op = depthInfo.store_op;
        cleared = true;
      }
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd_, &colour, 1, depth ? &depthInfo : nullptr);
      ++work_.renderPasses;
      SDL_GPUBuffer *vertexBuffers[2] = {shapeBuffer_.buffer, ops_.buffer};
      SDL_BindGPUVertexStorageBuffers(pass, 0, vertexBuffers, 2);
      SDL_GPUSampler *nearest = device_.nearestSampler();
      // With no capture the binding is never sampled: only a held read-back plans read-back ops.
      SDL_GPUTexture *readBack = capture_ != nullptr ? capture_ : vram_.snapshot;
      // Likewise the atlas: with none, params.outline names no scale and no op samples it.
      SDL_GPUTexture *fields = outlineFields_ != nullptr ? outlineFields_ : vram_.snapshot;
      SDL_GPUTexture *indices = outlineIndices_ != nullptr ? outlineIndices_ : vram_.snapshot;
      const SDL_GPUTextureSamplerBinding snapshots[5] = {{target.snapshot, nearest},
                                                         {vram_.snapshot, nearest},
                                                         {readBack, nearest},
                                                         {fields, device_.linearSampler()},
                                                         {indices, nearest}};
      SDL_BindGPUFragmentSamplers(pass, 0, snapshots, 5);
      SDL_GPUBuffer *fragmentBuffers[3] = {ops_.buffer, cluts_.buffer, pixels_.buffer};
      SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 3);
      const auto &shapes = plan.shapes[static_cast<std::size_t>(p)];
      drawShapes(pass,
                 plan,
                 std::span(shapes).subspan(range.firstShape, range.endShape - range.firstShape),
                 firstShape[static_cast<std::size_t>(p)] + range.firstShape,
                 depth);
      SDL_EndGPURenderPass(pass);
    }
    stage("record draw");
    // Every pass of the batch read the snapshots as they were; refresh them only now.
    for (int p = 0; p < planes; p++) {
      const gpu::RecordPlaneBatch &range = batch.planes[static_cast<std::size_t>(p)];
      Plane &target = plane(p);
      // The pass drew no farther than the snapshot refresh reaches.
      if (p == 0 && range.dirty.x0 < range.dirty.x1 && range.dirty.y0 < range.dirty.y1) {
        written = {std::min(written.x0, range.dirty.x0),
                   std::min(written.y0, range.dirty.y0),
                   std::max(written.x1, range.dirty.x1),
                   std::max(written.y1, range.dirty.y1)};
        (inBetween ? target.snapshotDiffers : target.beforeDiffers).add(range.dirty);
        target.inBetweenDiffers.add(range.dirty);
      }
      copyTexture({.from = inBetween ? target.inBetween : target.image,
                   .source = range.dirty,
                   .to = target.snapshot,
                   .toX = range.dirty.x0,
                   .toY = range.dirty.y0,
                   .scale = target.scale});
    }
    stage("record snapshot");
  }
  captureBefore(plan.batches.size());
  // An in-between canvas is seeded in its margins alone, so it takes the whole of its buffer's columns.
  copyInteriors(inBetween ? RecordRect{0, 0, gpu::kVramWidth, gpu::kVramHeight} : written, inBetween);
}

void RecordRasterizer::drawShapes(SDL_GPURenderPass *pass,
                                  const gpu::RecordRasterPlan &plan,
                                  std::span<const gpu::RecordShape> shapes,
                                  std::uint32_t firstVertexShape,
                                  bool depthTarget) {
  // Each draw is six vertices a shape (record.vert), its fragments the shapes' covered pixels.
  std::span<const gpu::RecordShape> runShapes;
  const Pipelines *set = nullptr;
  SDL_GPUGraphicsPipeline *bound = nullptr;
  const auto draw = [&](SDL_GPUGraphicsPipeline *pipeline, Uint32 first, Uint32 count) {
    if (pipeline != bound) {
      SDL_BindGPUGraphicsPipeline(pass, pipeline);
      bound = pipeline;
      ++work_.pipelineBinds;
    }
    SDL_DrawGPUPrimitives(pass, count, 1, first, 0);
    ++work_.draws;
    work_.triangles += shapeTriangles(runShapes);
    const std::uint64_t pixels = countPixels_ ? coveredPixels(runShapes) : 0;
    work_.pixels += pixels;
    const bool marks = pipeline == set->groupMark || pipeline == set->modelMark || pipeline == set->modelClear;
    work_.shadedPixels += marks ? 0 : pixels;
  };
  // Runs in record order, each through the pipelines its reading of the target and its shader pick; with a depth
  // target, also by its depth run.
  std::size_t run = 0;
  while (run < shapes.size()) {
    const DepthRun key = depthRun(plan, shapes[run], depthTarget);
    std::size_t end = run + 1;
    while (end < shapes.size() && depthRun(plan, shapes[end], depthTarget) == key) {
      end++;
    }
    const auto count = static_cast<Uint32>(end - run) * 6;
    runShapes = shapes.subspan(run, end - run);
    const Uint32 first = (firstVertexShape + static_cast<std::uint32_t>(run)) * 6;
    set = &pipelines(key.reads, key.full);
    if (!depthTarget) {
      draw(set->plain, first, count);
    } else if ((key.flags & gpu::kOpDepthModel) != 0u) {
      // Its parts' colour where they pass or cover their own model, then the depth they leave.
      SDL_SetGPUStencilReference(pass, static_cast<std::uint8_t>(kPassed | key.model));
      for (SDL_GPUGraphicsPipeline *pipeline : {set->modelTest, set->modelPaint, set->modelMark, set->modelClear}) {
        draw(pipeline, first, count);
      }
    } else if ((key.flags & gpu::kOpDepthObject) != 0u) {
      draw(set->object, first, count);
    } else if ((key.flags & gpu::kOpDepthTest) != 0u) {
      // Its polygons cover no pixel twice, so each wrote its colour exactly where it passed; now its depth there.
      SDL_SetGPUStencilReference(pass, kPassed);
      draw(set->groupTest, first, count);
      draw(set->groupMark, first, count);
    } else {
      draw(set->slot, first, count);
    }
    run = end;
  }
}

MappedTransfer RecordRasterizer::download(const RecordPicture &picture) {
  const RecordRect &rect = picture.rect;
  const int s = std::max(picture.scale, 1);
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

std::vector<CanvasWindows> RecordRasterizer::canvasWindows() const {
  std::vector<CanvasWindows> windows;
  for (const Canvas &canvas : canvases_) {
    if (!canvas.shape.crop()) {
      windows.push_back({canvas.shape.buffer, canvas.view, {canvas.shape.left, canvas.shape.right}});
    }
  }
  return windows;
}

void RecordRasterizer::restore(std::vector<CanvasWindows> canvases, std::vector<present::FrameRecord> records) {
  for (Canvas &canvas : canvases_) {
    releasePlane(canvas.plane);
  }
  canvases_.clear();
  chained_ = false;
  beforeHeld_ = false;
  readBack_.reset();
  restoredCanvases_ = std::move(canvases);
  restoredRecords_ = std::move(records);
  restorePending_ = true;
}

} // namespace psx::render
