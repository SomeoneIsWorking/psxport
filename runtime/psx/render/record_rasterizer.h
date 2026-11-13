// record_rasterizer.h — FrameRecords rasterized in record order into one VRAM image.
#pragma once

#include "frame_record.h"
#include "gpu_work.h"
#include "guest_widescreen_projection.h"
#include "outline_text.h"
#include "record_raster_setup.h"
#include "render_device.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psx::render {

// A texture and the rect of it to show, in its native pixels, each `scale` texels a side. Only the columns of `drawn`
// (inside `rect`, same rows) hold picture; the rest of `rect` shows black.
struct RecordPicture {
  SDL_GPUTexture *texture = nullptr;
  gpu::RecordRect rect;
  gpu::RecordRect drawn;
  int scale = 1;
};

// A margin canvas's buffer and the windows of the record that last drew it, as a machine state carries it.
struct CanvasWindows {
  gpu::RecordRect buffer;
  present::HorizontalMargins view;
  present::HorizontalMargins drawn;
};

// What a present shows: the guest display (native VRAM pixels) and the widest margin either side can take, or the
// band of it a crop keeps.
struct RecordView {
  gpu::RecordRect display;
  int reach = 0;
  present::CropBand crop;
};

// The image is VRAM at an integer scale, RG8 holding 1555 halfwords. Each record applies on top of
// the one before it; when that chain breaks, the device's VRAM is uploaded instead. With a reach,
// each displayed buffer also has a wide canvas (RecordCanvas) that the record draws into beside VRAM, inside the
// drawn window of the record's own windows (FrameRecord::windows), and a present shows the view window of the record
// that last drew that canvas, black where that record drew nothing;
// the VRAM image stays exactly the device's, and textures and copies read only it, except a declared
// read-back page, which is read from the canvas pixels captured for it (gpu::RecordReadBack). With a crop, each
// displayed buffer instead has a crop canvas (gpu::cropCanvas) holding the band at its own scale, and a present shows
// it whole.
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

  // The outline font atlas kOpOutline ops sample from the next show() on, uploaded when its generation is new, and
  // its button slots' face colours (present::buttonFaceColours).
  void useOutline(const present::OutlineAtlas *atlas, const std::array<std::uint32_t, 4> &faces);

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

  // Each margin canvas's buffer and windows.
  std::vector<CanvasWindows> canvasWindows() const;
  // A state load: the image and canvases are the replaced run's. The next show uploads the device's VRAM, makes a
  // canvas for each of `canvases` and draws `records` (oldest first) again at scale on top, then continues the chain
  // from the last of them, so the pictures they drew show as the saving run showed them.
  void restore(std::vector<CanvasWindows> canvases, std::vector<present::FrameRecord> records);

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
    int scale = 0;   // texels per native pixel
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
  Plane makePlane(const gpu::RecordCanvas &shape, int scale);
  void releasePlane(Plane &plane);
  // Keeps a canvas for `view`'s display, created from the image with empty margins and `windows`.
  void showCanvas(SDL_GPUCommandBuffer *cmd, const RecordView &view, const present::RecordWindows &windows);
  // The buffer's columns from the image; a new canvas is cleared first, a kept one keeps its margins.
  void seedCanvas(SDL_GPUCommandBuffer *cmd, Canvas &canvas, bool fresh);
  // A new canvas of `shape` and `windows`, seeded from the image; the least recently shown goes when all are taken.
  Canvas &addCanvas(SDL_GPUCommandBuffer *cmd, const gpu::RecordCanvas &shape, const present::RecordWindows &windows);
  // `record` rasterized on top of the image and canvases, the state before it kept.
  void apply(SDL_GPUCommandBuffer *cmd, const present::FrameRecord &record);
  // The restore(): the device's VRAM, the carried canvases and the carried records drawn again.
  void redrawRestored(SDL_GPUCommandBuffer *cmd, std::span<const std::uint16_t> vram, const RecordView &view);
  // A new crop canvas: its band copied from the image at the canvas's scale.
  void seedCrop(SDL_GPUCommandBuffer *cmd, Canvas &canvas);
  // The canvas `view` shows at this image scale.
  gpu::RecordCanvas viewShape(const RecordView &view) const;
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
  // `source` of `from` to (`toX`, `toY`) of `to`, both in native pixels of textures at `scale`.
  struct TextureCopy {
    SDL_GPUTexture *from = nullptr;
    gpu::RecordRect source;
    SDL_GPUTexture *to = nullptr;
    int toX = 0;
    int toY = 0;
    int scale = 1;
  };
  void copyTexture(SDL_GPUCommandBuffer *cmd, const TextureCopy &copy);
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
  // A state load's canvases and records, until the next show draws them.
  std::vector<CanvasWindows> restoredCanvases_;
  std::vector<present::FrameRecord> restoredRecords_;
  RecordPicture presented_;
  RecordPicture woven_;
  SDL_GPUTexture *field_ = nullptr;
  int fieldWidth_ = 0; // native pixels, at scale_
  int fieldHeight_ = 0;
  int scale_ = 0;
  int wovenOriginY_ = 0; // VRAM row of the woven picture's first row
  // The read-back texture records sample (gpu::RecordReadBack) and the canvas pixels captured for it.
  std::optional<gpu::RecordReadBack> readBack_;
  SDL_GPUTexture *capture_ = nullptr;
  int captureWidth_ = 0; // native pixels, at scale_
  int captureHeight_ = 0;
  // The outline atlas (present::OutlineAtlas) as uploaded.
  SDL_GPUTexture *outlineFields_ = nullptr;
  SDL_GPUTexture *outlineIndices_ = nullptr;
  std::uint64_t outlineGeneration_ = 0;
  int outlineScale_ = 0;
  int outlineOriginX_ = 0;
  int outlineOriginY_ = 0;
  std::array<std::uint32_t, 4> outlineFaces_{};

  Buffer quads_;
  Buffer ops_;
  Buffer cluts_;
  Buffer pixels_;
  SDL_GPUTransferBuffer *upload_ = nullptr;
  SDL_GPUTransferBuffer *readback_ = nullptr;
  std::uint32_t uploadSize_ = 0;
  std::uint32_t readbackSize_ = 0;

  std::uint64_t applied_ = 0;
  std::uint64_t beforeOf_ = 0; // the last applied record with entries; those after it changed nothing
  std::uint64_t replayed_ = 0;
  std::uint64_t resynced_ = 0;
  GpuWork work_;
  SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  bool chained_ = false;    // the image holds the device's VRAM after record `applied_`
  bool beforeHeld_ = false; // every plane's `before` holds it before record `beforeOf_`
  bool restorePending_ = false;
  bool countPixels_ = false;
};

} // namespace psx::render
