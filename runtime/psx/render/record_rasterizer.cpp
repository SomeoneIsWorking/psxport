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
constexpr ShaderCode kRecordFragment{spv_g_record_frag, spv_g_record_frag_len, {2, 1, 3}};

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

// Storage buffers are read as whole words, and never sized zero.
std::uint32_t paddedBytes(std::size_t bytes) {
  return static_cast<std::uint32_t>(std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 16));
}

} // namespace

RecordRasterizer::RecordRasterizer(RenderDevice &device)
    : device_(device), pipeline_(device.makePipeline(kRecordVertex, kRecordFragment, kVramFormat)) {
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
  for (SDL_GPUTexture **texture : {&plane.image, &plane.snapshot, &plane.inBetween, &plane.before}) {
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
    if (canvas.shape.buffer == view.display && canvas.shape.margin == view.margin) {
      return &canvas;
    }
  }
  return nullptr;
}

std::span<const RecordCanvas> RecordRasterizer::canvasShapes() {
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    shapes_[i] = canvases_[i].shape;
  }
  return {shapes_.data(), canvases_.size()};
}

RecordRasterizer::Plane &RecordRasterizer::plane(int index) {
  return index == 0 ? vram_ : canvases_[static_cast<std::size_t>(index - 1)].plane;
}

void RecordRasterizer::showCanvas(SDL_GPUCommandBuffer *cmd, const RecordView &view) {
  const int width = view.display.x1 - view.display.x0;
  const int height = view.display.y1 - view.display.y0;
  // A margin or display width change makes every canvas another shape.
  std::erase_if(canvases_, [&](Canvas &canvas) {
    const bool stale = !gpu::canvasSurvives(canvas.shape, width, view.margin);
    if (stale) {
      releasePlane(canvas.plane);
    }
    return stale;
  });
  if (view.margin <= 0 || width <= 0 || height <= 0) {
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
    added.shape = RecordCanvas{view.display, view.margin};
    added.plane = makePlane(added.shape);
    canvases_.push_back(added);
    canvas = &canvases_.back();
    seedCanvas(cmd, *canvas);
  }
  canvas->shown = ++shows_;
}

void RecordRasterizer::seedCanvas(SDL_GPUCommandBuffer *cmd, Canvas &canvas) {
  for (auto [from, to] : {std::pair{vram_.image, canvas.plane.image}, std::pair{vram_.before, canvas.plane.before}}) {
    SDL_GPUColorTargetInfo clear = {};
    clear.texture = to;
    clear.load_op = SDL_GPU_LOADOP_CLEAR;
    clear.store_op = SDL_GPU_STOREOP_STORE;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &clear, 1, nullptr));
    copyTexture(cmd, from, canvas.shape.buffer, to, canvas.shape.margin, 0);
  }
  copyTexture(cmd, canvas.plane.image, {0, 0, canvas.plane.width, canvas.plane.height}, canvas.plane.snapshot, 0, 0);
}

void RecordRasterizer::present(const RecordView &view, bool inBetween) {
  if (Canvas *canvas = view.margin > 0 ? canvasFor(view) : nullptr) {
    presented_ = {inBetween ? canvas->plane.inBetween : canvas->plane.image,
                  {0, 0, canvas->plane.width, canvas->plane.height}};
    wovenOriginY_ = canvas->plane.originY;
  } else {
    presented_ = {inBetween ? vram_.inBetween : vram_.image, view.display};
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
  }
  SDL_EndGPUCopyPass(copy);
  presented_ = {field_, {0, 0, width, height}};
}

void RecordRasterizer::update(SDL_GPUCommandBuffer *cmd,
                              const present::FrameRecord &record,
                              bool deviceAhead,
                              std::span<const std::uint16_t> vram,
                              int scale,
                              const RecordView &view) {
  ensureImage(std::max(scale, 1));
  showCanvas(cmd, view);
  present(view, false);
  advance(cmd, record, deviceAhead, vram);
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
      copyTexture(cmd, target.image, {0, 0, target.width, target.height}, target.before, 0, 0);
    }
    execute(cmd, planner_.plan(record, scale_, canvasShapes()), false);
    beforeOf_ = applied_;
    beforeHeld_ = true;
    return;
  }
  execute(cmd, planner_.planVramUpload(vram, scale_), false);
  for (Canvas &canvas : canvases_) {
    seedCanvas(cmd, canvas);
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
      (view.margin > 0 && canvasFor(view) == nullptr)) {
    return false;
  }
  const bool onImage = record.sequence() == applied_ + 1;
  if (!onImage && !(beforeHeld_ && record.sequence() == beforeOf_)) {
    return false;
  }
  for (int index = 0; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    const RecordRect whole{0, 0, target.width, target.height};
    copyTexture(cmd, onImage ? target.image : target.before, whole, target.inBetween, 0, 0);
    if (!onImage) {
      copyTexture(cmd, target.before, whole, target.snapshot, 0, 0);
    }
  }
  execute(cmd, planner_.plan(record, scale_, canvasShapes()), true);
  for (int index = 0; index <= static_cast<int>(canvases_.size()); index++) {
    const Plane &target = plane(index);
    copyTexture(cmd, target.image, {0, 0, target.width, target.height}, target.snapshot, 0, 0);
  }
  present(view, true);
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
                            const RecordView &view) {
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

void RecordRasterizer::execute(SDL_GPUCommandBuffer *cmd, const gpu::RecordRasterPlan &plan, bool inBetween) {
  if (plan.batches.empty()) {
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

  const int s = plan.scale;
  const int planes = 1 + static_cast<int>(canvases_.size());
  for (const gpu::RecordBatch &batch : plan.batches) {
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
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &colour, 1, nullptr);
      SDL_BindGPUGraphicsPipeline(pass, pipeline_);
      SDL_BindGPUVertexStorageBuffers(pass, 0, &quads_.buffer, 1);
      SDL_GPUSampler *nearest = device_.nearestSampler();
      const SDL_GPUTextureSamplerBinding snapshots[2] = {{target.snapshot, nearest}, {vram_.snapshot, nearest}};
      SDL_BindGPUFragmentSamplers(pass, 0, snapshots, 2);
      SDL_GPUBuffer *fragmentBuffers[3] = {ops_.buffer, cluts_.buffer, pixels_.buffer};
      SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 3);
      SDL_DrawGPUPrimitives(pass,
                            (range.endQuad - range.firstQuad) * 6,
                            1,
                            (firstQuad[static_cast<std::size_t>(p)] + range.firstQuad) * 6,
                            0);
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
