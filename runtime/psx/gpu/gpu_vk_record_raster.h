// gpu_vk_record_raster.h — FrameRecords rasterized in record order into one VRAM image.
#pragma once

#include "frame_record.h"
#include "record_raster_setup.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <span>
#include <vector>

namespace psx::gpu {

// A texture and the rect of it to show, in native pixels; the texture holds them at the image scale.
struct RecordPicture {
  SDL_GPUTexture *texture = nullptr;
  RecordRect rect;
};

// What a present shows: the guest display (native VRAM pixels) and the columns added each side of it.
struct RecordView {
  RecordRect display;
  int margin = 0;
};

// The image is VRAM at an integer scale, RG8 holding 1555 halfwords. Each record applies on top of
// the one before it; when that chain breaks, the device's VRAM is uploaded instead. With a margin,
// each displayed buffer also has a wide canvas (RecordCanvas) that the record draws into beside VRAM;
// the VRAM image stays exactly the device's, and textures and copies read only it.
class RecordRasterizer {
public:
  explicit RecordRasterizer(SDL_GPUDevice *device);
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
  // kept state before the last applied record with entries when `record` is that one. False, with nothing drawn,
  // when neither holds or `view` has no canvas yet.
  bool drawInBetween(SDL_GPUCommandBuffer *cmd, const present::FrameRecord &record, int scale, const RecordView &view);

  // One present: `composed` when given and drawable, then with `advanceToRecord` the image is brought to
  // `record` without showing it; else `record` through update(). True when `composed` was drawn.
  bool show(SDL_GPUCommandBuffer *cmd,
            const present::FrameRecord &record,
            const present::FrameRecord *composed,
            bool advanceToRecord,
            bool deviceAhead,
            std::span<const std::uint16_t> vram,
            int scale,
            const RecordView &view);

  // Records a download of `picture` at the image scale; read it after the submit.
  void download(SDL_GPUCommandBuffer *cmd, const RecordPicture &picture);
  // The last download, row-major halfwords, (width * scale) x (height * scale).
  std::vector<std::uint16_t> downloaded();

  SDL_GPUTexture *image() const {
    return vram_.image;
  }
  // What the last update or drawInBetween put on screen.
  RecordPicture presented() const {
    return presented_;
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
    int width = 0; // native pixels
    int height = 0;
    int originX = 0; // VRAM-space position of pixel (0, 0)
    int originY = 0;
  };
  struct Canvas {
    RecordCanvas shape;
    Plane plane;
    std::uint64_t shown = 0;
  };

  void ensureImage(int scale);
  // Applies `record` on top of the image, or uploads the device VRAM when it does not follow.
  void advance(SDL_GPUCommandBuffer *cmd,
               const present::FrameRecord &record,
               bool deviceAhead,
               std::span<const std::uint16_t> vram);
  Plane makePlane(int width, int height, int originX, int originY);
  void releasePlane(Plane &plane);
  // Keeps a canvas for `view`'s display, created from the image with empty margins.
  void showCanvas(SDL_GPUCommandBuffer *cmd, const RecordView &view);
  void seedCanvas(SDL_GPUCommandBuffer *cmd, Canvas &canvas);
  Canvas *canvasFor(const RecordView &view);
  std::vector<RecordCanvas> canvasShapes() const;
  Plane &plane(int index);
  void present(const RecordView &view, bool inBetween);
  void execute(SDL_GPUCommandBuffer *cmd, const RecordRasterPlan &plan, bool inBetween);
  void copyTexture(
      SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *from, const RecordRect &source, SDL_GPUTexture *to, int toX, int toY);
  void ensureBuffer(Buffer &buffer, std::uint32_t bytes, SDL_GPUBufferUsageFlags usage);
  void ensureUpload(std::uint32_t bytes);
  void releaseImage();

  SDL_GPUDevice *device_;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUSampler *sampler_ = nullptr;
  Plane vram_;
  std::vector<Canvas> canvases_;
  std::uint64_t shows_ = 0;
  RecordPicture presented_;
  int scale_ = 0;

  Buffer quads_;
  Buffer ops_;
  Buffer cluts_;
  Buffer pixels_;
  SDL_GPUTransferBuffer *upload_ = nullptr;
  std::uint32_t uploadSize_ = 0;
  SDL_GPUTransferBuffer *readback_ = nullptr;
  std::uint32_t readbackSize_ = 0;
  RecordRect downloadRect_;

  bool chained_ = false; // the image holds the device's VRAM after record `applied_`
  std::uint64_t applied_ = 0;
  bool beforeHeld_ = false;    // every plane's `before` holds it before record `beforeOf_`
  std::uint64_t beforeOf_ = 0; // the last applied record with entries; those after it changed nothing
  std::uint64_t replayed_ = 0;
  std::uint64_t resynced_ = 0;
};

} // namespace psx::gpu
