// record_raster_setup.h — a FrameRecord turned into GPU draw work by gpu.c's own coverage rules.
//
// Each primitive costs one op and a constant number of quads whatever the scale: the CPU sets up gpu.c's
// integer edge and interpolant inputs at the target scale, and shaders_gpu/record.frag evaluates coverage,
// texel, modulation, blend and mask per pixel. Ops whose reads overlap pixels gpu.c wrote at 1x earlier in
// the same batch start a new batch, so at 1x every read sees VRAM exactly as the device would and the ops
// and batches are the same at every scale. This header is the op
// layout's one source; tools/gen_gpu_shaders.py writes it into the shader as record_op.glsl.
#pragma once

#include "depth_test.h"
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

// A DepthPlane op follows each depth-tested triangle and is never drawn itself.
enum class RecordOpKind : std::uint8_t {
  Triangle = 0,
  Sprite = 1,
  LinePixel = 2,
  Fill = 3,
  Copy = 4,
  Upload = 5,
  DepthPlane = 6
};

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

// A DepthPlane op's floats: 1/z at its triangle's first vertex, its x and y slopes per native pixel, that
// vertex's native VRAM position, and the view units its written depth lies back by (its DepthMark::bin and back);
// then, a word, a Model part's stencil id (kModelIds).
enum RecordDepthField : std::uint8_t {
  kDepthInverse = 1,
  kDepthSlopeX = 2,
  kDepthSlopeY = 3,
  kDepthOriginX = 4,
  kDepthOriginY = 5,
  kDepthBin = 6,
  kDepthModel = 7,
};

// A record's Model parts carry their model as a stencil id, 1..kModelIds by first appearance, repeating after that.
inline constexpr std::uint32_t kModelIds = 0x7F;

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
  kOpReadBack = 1u << 24,    // texels from the read-back capture, not VRAM
  kOpDepthTest = 1u << 25,   // tests depth (present::DepthTest); the next op is its DepthPlane
  kOpDepthObject = 1u << 26, // DepthOrder::Object: draws over what came before it and writes its depth
  // A tested polygon's depth group parity. A group's polygons cover no native pixel twice, so the group tests in one
  // draw and then writes its depth in another; consecutive groups differ in parity.
  kOpDepthGroup = 1u << 27,
  kOpDepthModel = 1u << 28, // DepthOrder::Model: tests against all but its own model's parts, then writes its depth
  kOpOutline = 1u << 29,    // texels from the outline font atlas (present::OutlineAtlas), not VRAM; above native scale
};

// One rectangle of a plane's pixels belonging to an op, scaled, half open. Op origins are in VRAM space,
// which record.frag reaches by adding the plane's origin.
struct RecordQuad {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  std::uint32_t op = 0;
  std::uint32_t param = 0;   // triangle: its part; line pixel: its colour r | g << 8 | b << 16
  std::uint32_t mapping = 0; // RecordMapping: how the plane's pixels map to the op's VRAM space
  std::uint32_t unused = 0;
};

// A quad's mapping: the plane's own scale and origin, or a crop canvas's world or UI layer (RecordCanvas).
enum RecordMapping : std::uint32_t {
  kMappingPlane = 0,
  kMappingCropWorld = 1,
  kMappingCropUi = 2,
};

// The draw area is the buffer: its columns exactly, its rows covered.
bool drawAreaSpansBuffer(const present::RecordDrawState &state, const RecordRect &buffer);

// A display buffer's wide canvas: `buffer` (native VRAM pixels) with `reach` more columns allocated each side, of
// which the window `left` and `right` (each at most `reach`) is drawn and shown.
// Its pixel (x, y) is VRAM-space pixel (originX() + x, originY() + y), which may lie outside VRAM.
//
// A crop canvas (plan 0203) has no reach: `buffer` is the band of the display it shows, its pixels `worldScale`
// target pixels per native pixel, and the primitives the title marks UI draw the whole display `uiBuffer` at
// `uiScale`, its target pixel (x, y) being UI-scaled VRAM-space pixel (uiOriginX + x, uiOriginY + y).
struct RecordCanvas {
  RecordRect buffer;
  int reach = 0;
  int left = 0;
  int right = 0;
  int worldScale = 0;
  int uiScale = 0;
  RecordRect uiBuffer;
  int uiOriginX = 0;
  int uiOriginY = 0;

  bool crop() const {
    return uiScale > 0;
  }

  int originX() const {
    return buffer.x0 - reach;
  }
  int originY() const {
    return buffer.y0;
  }
  int width() const {
    return buffer.x1 - buffer.x0 + 2 * reach;
  }
  int height() const {
    return buffer.y1 - buffer.y0;
  }
  // The window in canvas pixels.
  RecordRect window() const {
    return {reach - left, 0, reach + buffer.x1 - buffer.x0 + right, height()};
  }
  bool operator==(const RecordCanvas &) const = default;
};

// Whether a kept canvas still serves displays of this width at this reach. The two buffers of a double-buffered
// title may differ in height, so each keeps its own canvas and only a widening or width change retires one.
bool canvasSurvives(const RecordCanvas &canvas, int displayWidth, int reach);

// The crop canvas of `display` keeping `p`/`q` of its rows at unit `unit` (present::CropBand, present::cropUnit): the
// world at q * unit, the UI at p * unit, centred over it.
RecordCanvas cropCanvas(const RecordRect &display, int p, int q, int unit);

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

// A read-back texture page, sampled from the rasterizer's capture until something writes the page.
struct RecordReadBack {
  RecordRect page;     // texels at `texMode`, VRAM rows
  RecordRect pageVram; // the same, VRAM halfwords
  int texMode = 0;
  RecordRect read;     // VRAM-space native pixels; the page's first texel is read's first pixel
  RecordRect captured; // what the capture holds: `read`, widened by that side's margin where it meets its canvas edge
  std::uint32_t weights = 0; // kOpReadBackWeights

  bool operator==(const RecordReadBack &) const = default;
};

// Canvas pixels (`rect`, native VRAM space) copied into the read-back capture before batch `beforeBatch`.
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
  // The read-back texture in force after this record.
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
  // buffer, except a fill spanning the buffer's columns, which spans the canvas's. An in-between plans with
  // `declares` false: it samples `readBack` but captures nothing. The polygons `depthTest` tests carry their
  // depth plane; with none, every primitive draws in slot order.
  const RecordRasterPlan &plan(const present::FrameRecord &record,
                               int scale,
                               std::span<const RecordCanvas> canvases,
                               const std::optional<RecordReadBack> &readBack,
                               bool declares,
                               const present::DepthTest *depthTest);
  // The whole of `vram` written over the image and the band of each crop canvas, for a record that cannot be
  // replayed; ends any read-back texture.
  const RecordRasterPlan &
  planVramUpload(std::span<const std::uint16_t> vram, int scale, std::span<const RecordCanvas> canvases);
  // The band of each crop canvas among `canvases` copied from the image, for a canvas just made.
  const RecordRasterPlan &planCropSeed(int scale, std::span<const RecordCanvas> canvases);

private:
  class Builder;
  std::unique_ptr<Builder> builder_;
};

} // namespace psx::gpu
