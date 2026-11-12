// record_rasterizer.h — FrameRecords rasterized in record order into one VRAM image.
#pragma once

#include "frame_record.h"
#include "gpu_work.h"
#include "guest_widescreen_projection.h"
#include "record_raster_setup.h"
#include "render_device.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psx::render {

// A texture and the rect of it to show, in native pixels; the texture holds them at the image scale. Only the
// columns of `drawn` (inside `rect`, same rows) hold picture; the rest of `rect` shows black.
struct RecordPicture {
  SDL_GPUTexture *texture = nullptr;
  gpu::RecordRect rect;
  gpu::RecordRect drawn;
};

// What a present shows: the guest display (native VRAM pixels) and the widest margin either side can take.
struct RecordView {
  gpu::RecordRect display;
  int reach = 0;
};

// The image is VRAM at an integer scale, RG8 holding 1555 halfwords. Each record applies on top of
// the one before it; when that chain breaks, the device's VRAM is uploaded instead. With a reach,
// each displayed buffer also has a wide canvas (RecordCanvas) that the record draws into beside VRAM, inside the
// drawn window of the record's own windows (FrameRecord::windows), and a present shows the view window of the record
// that last drew that canvas, black where that record drew nothing;
// the VRAM image stays exactly the device's, and textures and copies read only it, except a declared
// read-back page, which is read from the canvas pixels captured for it (gpu::RecordReadBack).
class RecordRasterizer {
public:
  explicit RecordRasterizer(RenderDevice &device);
  ~RecordRasterizer();
  RecordRasterizer(const RecordRasterizer &) = delete;
  RecordRasterizer &operator=(const RecordRasterizer &) = delete;

  // Brings the image to the end of `record` and shows `view`. `vram` is the device's VRAM, used when
  // `record` does not continue the image; `deviceAhead` says it already holds work after `record`.
  void update(SDL_GPUCommandBuffer *cmd,
              const present::FrameRecord &record,
              bool deviceAhead,
              std::span<const std::uint16_t> vram,
              int scale,
              const RecordView &view);

  // Rasterizes `record` onto copies of the VRAM state before it and shows `view` of them, leaving the
  // chain untouched. That state is the image when `record` follows the last applied record, or the
  // kept state before the last applied record with entries when `record` is that one. False, with
  // nothing drawn, when neither holds or `view` has no canvas yet.
  bool drawInBetween(SDL_GPUCommandBuffer *cmd, const present::FrameRecord &record, int scale, const RecordView &view);

  // One present: `composed` when given and drawable, then with `advanceToRecord` the image is brought to
  // `record` without showing it; else `record` through update(). True when `composed` was drawn. The polygons
  // `depthTest` tests order by depth among themselves; with none, everything draws in slot order.
  bool show(SDL_GPUCommandBuffer *cmd,
            const present::FrameRecord &record,
            const present::FrameRecord *composed,
            bool advanceToRecord,
            bool deviceAhead,
            std::span<const std::uint16_t> vram,
            int scale,
            const RecordView &view,
            const present::DepthTest *depthTest);

  // Whether the next show() counts its covered pixels; every other GpuWork count is always kept.
  void countPixels(bool counting) {
    countPixels_ = counting;
  }
  // The GPU work the last show() recorded.
  [[nodiscard]] const GpuWork &work() const {
    return work_;
  }

  // `picture` at the image scale as row-major halfwords, after everything submitted before it.
  MappedTransfer download(const RecordPicture &picture);

  SDL_GPUTexture *image() const {
    return vram_.image;
  }
  // What the last update or drawInBetween put on screen. A record that draws one field per frame is
  // shown as that field, each row doubled over its pair, as a display shows it.
  RecordPicture presented() const {
    return presented_;
  }
  // The same picture with both fields woven, as the device holds it.
  RecordPicture woven() const {
    return woven_;
  }
  int scale() const {
    return scale_;
  }
  std::uint64_t replayed() const {
    return replayed_;
  }
  std::uint64_t resynced() const {
    return resynced_;
  }

private:
  struct Buffer {
    SDL_GPUBuffer *buffer = nullptr;
    std::uint32_t size = 0;
  };
  // A render target, the copy of it every pass reads, the target an in-between draws into, and the
  // target as it was before the last applied record.
  struct Plane {
    SDL_GPUTexture *image = nullptr;
    SDL_GPUTexture *snapshot = nullptr; // equals the target at the start of every execute
    SDL_GPUTexture *inBetween = nullptr;
    SDL_GPUTexture *before = nullptr;
    SDL_GPUTexture *depth = nullptr; // made when a record first depth-tests on this plane; cleared per record
    int width = 0;                   // native pixels
    int height = 0;
    int originX = 0; // VRAM-space position of pixel (0, 0)
    int originY = 0;
  };
  // `shape`'s window is the drawn window of the record that last drew it, `view` that record's view.
  struct Canvas {
    gpu::RecordCanvas shape;
    present::HorizontalMargins view;
    Plane plane;
    std::uint64_t shown = 0;
  };

  void ensureImage(int scale);
  // Applies `record` on top of the image, or uploads the device VRAM when it does not follow.
  void advance(SDL_GPUCommandBuffer *cmd,
               const present::FrameRecord &record,
               bool deviceAhead,
               std::span<const std::uint16_t> vram);
  Plane makePlane(const gpu::RecordCanvas &shape);
  void releasePlane(Plane &plane);
  // Keeps a canvas for `view`'s display, created from the image with empty margins and `windows`.
  void showCanvas(SDL_GPUCommandBuffer *cmd, const RecordView &view, const present::RecordWindows &windows);
  // The buffer's columns from the image; a new canvas is cleared first, a kept one keeps its margins.
  void seedCanvas(SDL_GPUCommandBuffer *cmd, Canvas &canvas, bool fresh);
  Canvas *canvasFor(const RecordView &view);
  // The shapes the planner draws `margins` into, in plane order.
  std::span<const gpu::RecordCanvas> canvasShapes(present::HorizontalMargins margins);
  // A canvas `plan` drew into takes its windows.
  void adoptWindows(const gpu::RecordRasterPlan &plan, const present::RecordWindows &windows);
  // The part of plane `index` a record of `margins` can change or show: the canvas windows before and after it.
  gpu::RecordRect copiedRect(int index, present::HorizontalMargins margins);
  Plane &plane(int index);
  // Shows the canvas's windows, or `windows` for an in-between that drew the canvas.
  void present(const RecordView &view, bool inBetween, const present::RecordWindows *windows);
  // Replaces the presented picture by the field `record` drew, when it draws one field per frame.
  void selectField(SDL_GPUCommandBuffer *cmd, const present::FrameRecord &record);
  void execute(SDL_GPUCommandBuffer *cmd, const gpu::RecordRasterPlan &plan, bool inBetween);
  // Quads [first, end) of a pass, each run of depth-tested quads through its depth pipelines.
  void drawQuads(SDL_GPURenderPass *pass,
                 const gpu::RecordRasterPlan &plan,
                 std::span<const gpu::RecordQuad> quads,
                 std::uint32_t firstVertexQuad,
                 bool depthTarget);
  // Copies the capture's canvas pixels into capture_, sized to them.
  void capture(SDL_GPUCommandBuffer *cmd, const gpu::RecordCapture &capture);
  void copyTexture(SDL_GPUCommandBuffer *cmd,
                   SDL_GPUTexture *from,
                   const gpu::RecordRect &source,
                   SDL_GPUTexture *to,
                   int toX,
                   int toY);
  void ensureBuffer(Buffer &buffer, std::uint32_t bytes);
  void ensureTransfer(SDL_GPUTransferBuffer *&transfer,
                      std::uint32_t &size,
                      SDL_GPUTransferBufferUsage usage,
                      std::uint32_t bytes);
  void releaseImage();

  RenderDevice &device_;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  // With a depth target: slot order (no test, no write); a depth group's test, colour where it passes, marked in
  // stencil; the group's depth written where marked, moved back by its bin; an object over the scene, writing its
  // depth.
  SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUGraphicsPipeline *slotPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *groupTestPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *groupMarkPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *objectPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *modelTestPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *modelPaintPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *modelMarkPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *modelClearPipeline_ = nullptr;
  const present::DepthTest *depthTest_ = nullptr; // this present's
  gpu::RecordPlanner planner_;
  Plane vram_;
  std::vector<Canvas> canvases_;
  std::array<gpu::RecordCanvas, gpu::kRecordMaxCanvases> shapes_{};
  std::uint64_t shows_ = 0;
  RecordPicture presented_;
  RecordPicture woven_;
  int wovenOriginY_ = 0; // VRAM row of the woven picture's first row
  SDL_GPUTexture *field_ = nullptr;
  int fieldWidth_ = 0; // native pixels, at scale_
  int fieldHeight_ = 0;
  int scale_ = 0;
  // The read-back texture records sample (gpu::RecordReadBack) and the canvas pixels captured for it.
  std::optional<gpu::RecordReadBack> readBack_;
  SDL_GPUTexture *capture_ = nullptr;
  int captureWidth_ = 0; // native pixels, at scale_
  int captureHeight_ = 0;

  Buffer quads_;
  Buffer ops_;
  Buffer cluts_;
  Buffer pixels_;
  SDL_GPUTransferBuffer *upload_ = nullptr;
  std::uint32_t uploadSize_ = 0;
  SDL_GPUTransferBuffer *readback_ = nullptr;
  std::uint32_t readbackSize_ = 0;

  bool chained_ = false; // the image holds the device's VRAM after record `applied_`
  std::uint64_t applied_ = 0;
  bool beforeHeld_ = false;    // every plane's `before` holds it before record `beforeOf_`
  std::uint64_t beforeOf_ = 0; // the last applied record with entries; those after it changed nothing
  std::uint64_t replayed_ = 0;
  std::uint64_t resynced_ = 0;
  GpuWork work_;
  bool countPixels_ = false;
};

} // namespace psx::render
