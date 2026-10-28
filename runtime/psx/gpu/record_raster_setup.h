// record_raster_setup.h — a FrameRecord turned into GPU draw work by gpu.c's own coverage rules.
//
// Each primitive costs one op and a constant number of quads whatever the scale: the CPU sets up gpu.c's
// integer edge and interpolant inputs at the target scale, and shaders_gpu/record.frag evaluates coverage,
// texel, modulation, blend and mask per pixel. Ops whose reads overlap pixels gpu.c wrote at 1x earlier in
// the same batch start a new batch, so at 1x every read sees VRAM exactly as the device would and the ops
// and batches are the same at every scale. This header is the op
// layout's one source; tools/gen_gpu_shaders.py writes it into the shader as record_op.glsl.
#pragma once

#include "frame_record.h"
#include "record_geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace psx::gpu {

inline constexpr int kRecordOpWords = 40;
using RecordOp = std::array<std::uint32_t, kRecordOpWords>;

enum class RecordOpKind : std::uint8_t { Triangle = 0, Sprite = 1, LinePixel = 2, Fill = 3, Copy = 4, Upload = 5 };

// RecordOp word indices.
enum RecordOpField : std::uint8_t {
  kOpFlags = 0,      // kind | RecordOpFlag bits
  kOpColour = 1,     // flat colour r | g << 8 | b << 16, or a fill value
  kOpWindowXAnd = 2, // gpu.c SUCV.TWX_AND
  kOpWindowXAdd = 3, // gpu.c SUCV.TWX_ADD
  kOpWindowYAnd = 4,
  kOpWindowYAdd = 5,
  kOpClut = 6,    // halfword index into the CLUT pool
  kOpBase = 7,    // r, g, b, u, v interpolant bases (7..11)
  kOpDx = 12,     // per-pixel x deltas (12..16)
  kOpShiftX = 12, // sprite: its place past its whole pixel, target pixels
  kOpShiftY = 13,
  kOpDy = 17,      // per-row y deltas (17..21)
  kOpOriginX = 22, // sprite/upload/copy destination origin, native pixels
  kOpOriginY = 23,
  kOpSourceX = 24, // copy source origin, native pixels; sprite u0; upload width
  kOpSourceY = 25, // copy source; sprite v0; upload height
  kOpPool = 26,    // upload pixel pool halfword offset
  kOpParts = 22,   // triangle: its two RecordPartField blocks, upper then lower
  // kOpReadBack: the capture pixel a fragment samples minus the fragment, native pixels (the u and v slots).
  kOpReadBackX = 10,
  kOpReadBackY = 11,
  kOpReadBackWeights = 15, // red | green << 8 | blue << 16 | shift << 24
};

// A triangle part's words from kOpParts + part * kPartWords. Edges are gpu.c's 32.32 x at interpolant row
// 0, low word first; row r's x is edge + r * step.
enum RecordPartField : std::uint8_t {
  kPartRowShift = 0, // plotted row minus interpolant row
  kPartLeft = 1,
  kPartRight = 3,
  kPartLeftStep = 5,
  kPartRightStep = 7,
  kPartWords = 9,
};

enum RecordOpFlag : std::uint32_t {
  kOpKindMask = 0xFFu,
  kOpTextured = 1u << 8,
  kOpModulate = 1u << 9,
  kOpGouraud = 1u << 10,
  kOpSemi = 1u << 12,
  kOpMaskCheck = 1u << 13,
  kOpMaskSet = 1u << 14,
  kOpBlendShift = 16,   // two bits
  kOpTexModeShift = 18, // two bits
  kOpSkipRows = 1u << 20,
  kOpSkipParity = 1u << 21,
  kOpFlipX = 1u << 22,
  kOpFlipY = 1u << 23,
  kOpReadBack = 1u << 24, // texels from the read-back capture, not VRAM
};

// One rectangle of a plane's pixels belonging to an op, scaled, half open. Op origins are in VRAM space,
// which record.frag reaches by adding the plane's origin.
struct RecordQuad {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  std::uint32_t op = 0;
  std::uint32_t param = 0; // triangle: its part; line pixel: its colour r | g << 8 | b << 16
  std::uint32_t unused[2] = {};
};

// The draw area is the buffer: its columns exactly, its rows covered.
bool drawAreaSpansBuffer(const present::RecordDrawState &state, const RecordRect &buffer);

// A display buffer's wide canvas: `buffer` (native VRAM pixels) with `margin` more columns each side.
// Its pixel (x, y) is VRAM-space pixel (originX() + x, originY() + y), which may lie outside VRAM.
struct RecordCanvas {
  RecordRect buffer;
  int margin = 0;

  int originX() const {
    return buffer.x0 - margin;
  }
  int originY() const {
    return buffer.y0;
  }
  int width() const {
    return buffer.x1 - buffer.x0 + 2 * margin;
  }
  int height() const {
    return buffer.y1 - buffer.y0;
  }
  bool operator==(const RecordCanvas &) const = default;
};

// Whether a kept canvas still serves displays of this margin and width. The two buffers of a double-buffered
// title may differ in height, so each keeps its own canvas and only a margin or width change retires one.
bool canvasSurvives(const RecordCanvas &canvas, int displayWidth, int margin);

// Plane 0 is the VRAM image; plane 1 + i is canvas i.
inline constexpr int kRecordMaxCanvases = 2;
inline constexpr int kRecordPlanes = 1 + kRecordMaxCanvases;

// Quads [first, end) of one plane, in that plane's pixels; afterwards `dirty` (native pixels of the
// plane) must be copied to its read snapshot.
struct RecordPlaneBatch {
  std::uint32_t firstQuad = 0;
  std::uint32_t endQuad = 0;
  RecordRect dirty;
};

// One render pass per plane. Every pass of a batch reads the snapshots as they were before it.
struct RecordBatch {
  std::array<RecordPlaneBatch, kRecordPlanes> planes;
};

// A read-back texture (present::ReadBackTexture) the planner samples from the rasterizer's capture in place of
// its page. A textured primitive whose texels all lie in the page and map one to one onto its pixels (a sprite,
// or a polygon whose u and v advance one per pixel, no texture window) reads the capture at its pixel moved by
// one offset, so at scale it shows the captured picture's full detail, and a rectangle of it on a canvas
// buffer's edge continues into that margin. Any write into the page ends it; the page is then read as VRAM.
struct RecordReadBack {
  RecordRect page;     // texels at `texMode`, VRAM rows
  RecordRect pageVram; // the same, VRAM halfwords
  int texMode = 0;
  RecordRect read;     // VRAM-space native pixels; the page's first texel is read's first pixel
  RecordRect captured; // what the capture holds: `read`, widened by the margin on a side where it meets its canvas edge
  std::uint32_t weights = 0; // kOpReadBackWeights

  bool operator==(const RecordReadBack &) const = default;
};

// Canvas pixels the rasterizer copies into the read-back capture before batch `beforeBatch` runs: `rect`,
// VRAM-space native pixels, of `plane`.
struct RecordCapture {
  std::uint32_t beforeBatch = 0;
  int plane = 0;
  RecordRect rect;
};

struct RecordRasterPlan {
  int scale = 1;
  std::vector<RecordOp> ops;
  std::array<std::vector<RecordQuad>, kRecordPlanes> quads;
  std::vector<RecordBatch> batches;
  std::span<const std::uint16_t> clutPool;
  std::span<const std::uint16_t> uploadPool;
  // The record's last read-back declaration that lands in a canvas, captured where it stands.
  std::optional<RecordCapture> capture;
  // The read-back texture after the record: its own declaration from the next record on, else the one it was
  // planned with unless the record wrote into its page.
  std::optional<RecordReadBack> readBack;
};

// Plans records into reused buffers; a plan stays valid until the next call.
class RecordPlanner {
public:
  RecordPlanner();
  ~RecordPlanner();
  RecordPlanner(const RecordPlanner &) = delete;
  RecordPlanner &operator=(const RecordPlanner &) = delete;

  // Every entry of a complete record, in order, into VRAM and into each canvas it reaches. A primitive
  // whose draw area spans exactly a canvas's buffer columns and covers its rows draws there with the
  // horizontal clip widened by the margin; any other write lands in a canvas only where it lands in the
  // buffer, except a fill spanning the buffer's columns, which spans the canvas's. Primitives sample
  // `readBack` where it serves them. `declares`: the plan advances the image, so its read-back declarations
  // are captured and take effect; an in-between's are not.
  const RecordRasterPlan &plan(const present::FrameRecord &record,
                               int scale,
                               std::span<const RecordCanvas> canvases,
                               const std::optional<RecordReadBack> &readBack,
                               bool declares);
  // The whole of `vram` written over the image; how a record that cannot be replayed is shown. It ends any
  // read-back texture: the records it skips may have written the page.
  const RecordRasterPlan &planVramUpload(std::span<const std::uint16_t> vram, int scale);

private:
  class Builder;
  std::unique_ptr<Builder> builder_;
};

} // namespace psx::gpu
