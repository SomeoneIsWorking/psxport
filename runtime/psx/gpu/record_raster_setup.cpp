// record_raster_setup.cpp — gpu.c's polygon, sprite, line and transfer rules, producing RecordPlan work.
#include "record_raster_setup.h"

#include "record_raster_regions.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>
#include <variant>

namespace psx::gpu {
namespace {

using present::DrawPrimitive;
using present::FrameRecord;
using present::PrimitiveKind;
using present::RecordDrawState;

// Two's-complement wrap of `value` into [-half, half).
std::int64_t wrapSigned(std::int64_t value, std::int64_t half) {
  const std::int64_t range = half * 2;
  std::int64_t wrapped = (value + half) % range;
  if (wrapped < 0) {
    wrapped += range;
  }
  return wrapped - half;
}

struct TriVertex {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t u = 0;
  std::int32_t v = 0;
  std::int32_t r = 0;
  std::int32_t g = 0;
  std::int32_t b = 0;
};

// The native draw area, with a draw area below VRAM row 511 clipped there.
struct Clip {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
};

Clip nativeClip(const RecordDrawState &state) {
  return Clip{state.clipX0, state.clipY0, state.clipX1, std::min(state.clipY1, kVramHeight - 1)};
}

bool skipsRow(const RecordDrawState &state, int nativeY) {
  return state.skipRowParity >= 0 && (nativeY & 1) == state.skipRowParity;
}

// An inclusive texel coordinate range, or the whole axis.
struct TexelRange {
  int low = 256;
  int high = -1;
  void cover(int texel) {
    low = std::min(low, texel);
    high = std::max(high, texel);
  }
  void all() {
    low = 0;
    high = 255;
  }
  bool empty() const {
    return high < low;
  }
};

// The clip one plane draws a primitive with, in VRAM-space native pixels, inclusive.
struct PlaneClip {
  int plane = 0;
  Clip clip;
};
using PlaneClips = FixedList<PlaneClip, kRecordPlanes>;

// gpu_polygon_sub.c Calc_UVOffsets_Adjust_Verts, in exact integer arithmetic.
void adjustUv(TriVertex vertices[3], int one, bool &offU, bool &offV) {
  const std::int64_t abx = vertices[1].x - vertices[0].x;
  const std::int64_t aby = vertices[1].y - vertices[0].y;
  const std::int64_t bcx = vertices[2].x - vertices[1].x;
  const std::int64_t bcy = vertices[2].y - vertices[1].y;
  const std::int64_t cax = vertices[0].x - vertices[2].x;
  const std::int64_t cay = vertices[0].y - vertices[2].y;
  const std::int64_t dudx = -aby * vertices[2].u - bcy * vertices[0].u - cay * vertices[1].u;
  const std::int64_t dvdx = -aby * vertices[2].v - bcy * vertices[0].v - cay * vertices[1].v;
  const std::int64_t dudy = abx * vertices[2].u + bcx * vertices[0].u + cax * vertices[1].u;
  const std::int64_t dvdy = abx * vertices[2].v + bcx * vertices[0].v + cax * vertices[1].v;
  const std::int64_t area = bcx * cay - bcy * cax;
  const std::int64_t texArea =
      static_cast<std::int64_t>(vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
      static_cast<std::int64_t>(vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);
  if (area == 0 || texArea == 0) {
    return;
  }
  const std::int64_t sign = area > 0 ? 1 : -1;
  const bool negDudx = dudx * sign < 0;
  const bool negDudy = dudy * sign < 0;
  const bool negDvdx = dvdx * sign < 0;
  const bool negDvdy = dvdy * sign < 0;
  const bool zeroDudx = dudx == 0;
  const bool zeroDudy = dudy == 0;
  const bool zeroDvdx = dvdx == 0;
  const bool zeroDvdy = dvdy == 0;
  if ((negDudx && zeroDudy) || (negDudy && zeroDudx)) {
    offU = true;
  }
  if ((negDvdx && zeroDvdy) || (negDvdy && zeroDvdx)) {
    offV = true;
  }
  // gpu.c's Wild Arms 2 sprite fix.
  const bool hasOne = aby == one || bcy == one || cay == one;
  const bool hasZero = aby == 0 || bcy == 0 || cay == 0;
  const bool hasMinusOne = aby == -one || bcy == -one || cay == -one;
  if (zeroDvdx && hasOne && hasZero && hasMinusOne && negDvdy) {
    if (aby == -one) {
      vertices[0].v = vertices[1].v - 1;
    } else if (bcy == -one) {
      vertices[1].v = vertices[2].v - 1;
    } else if (cay == -one) {
      vertices[2].v = vertices[0].v - 1;
    }
    if (aby == one) {
      vertices[1].v = vertices[0].v - 1;
    } else if (bcy == one) {
      vertices[2].v = vertices[1].v - 1;
    } else if (cay == one) {
      vertices[0].v = vertices[2].v - 1;
    }
  }
}

// Target pixels a vertex's place lies past its whole pixel, truncated as the GTE truncates; native scale keeps the
// guest's pixel.
std::int32_t placeOffset(float sub, int scale) {
  return scale == 1 ? 0 : static_cast<std::int32_t>(std::floor(sub * static_cast<float>(scale)));
}

std::int32_t scaledCoordinate(int whole, float sub, int scale) {
  return whole * scale + placeOffset(sub, scale);
}

TriVertex scaledVertex(const present::RecordVertex &vertex, int scale) {
  return TriVertex{scaledCoordinate(vertex.x, vertex.subX, scale),
                   scaledCoordinate(vertex.y, vertex.subY, scale),
                   vertex.u,
                   vertex.v,
                   vertex.r,
                   vertex.g,
                   vertex.b};
}

constexpr std::uint64_t makePolyXFP(std::int32_t x) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) + ((1ull << 32) - (1ull << 11));
}

std::int64_t makePolyXFPStep(std::int32_t dx, std::int32_t dy) {
  std::int64_t dxEx = static_cast<std::int64_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(dx)) << 32);
  if (dxEx < 0) {
    dxEx -= dy - 1;
  }
  if (dxEx > 0) {
    dxEx += dy - 1;
  }
  return dxEx / dy;
}

std::int32_t polyXInt(std::uint64_t xfp) {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(xfp) >> 32);
}

std::int64_t lineDivide(std::int64_t delta, std::int32_t dk) {
  delta = static_cast<std::int64_t>(static_cast<std::uint64_t>(delta) << 32);
  if (delta < 0) {
    delta -= dk - 1;
  }
  if (delta > 0) {
    delta += dk - 1;
  }
  return delta / dk;
}

// One part of a triangle's edge walk (rows A..B or B..C), in VRAM-space scaled pixels. gpu.c plots
// interpolant row r at row r + rowShift and covers [wrapped left, wrapped left + right - left) there.
struct TrianglePart {
  std::int32_t rowShift = 0;
  std::uint64_t edge[2] = {}; // left, right x at interpolant row 0
  std::uint64_t step[2] = {};
  int rowLow = 0; // plotted rows, half open
  int rowHigh = 0;
  std::int64_t half = 0; // the 11-bit x wrap at the walk's scale
};

// Plotted columns [x0, x1), and how far wrapping moved them from the interpolant columns.
struct PlottedSpan {
  int x0 = 0;
  int x1 = 0;
  std::int64_t shift = 0;
};

PlottedSpan plottedSpan(const TrianglePart &part, int row) {
  const auto yi = static_cast<std::uint64_t>(static_cast<std::int64_t>(row - part.rowShift));
  const std::int32_t left = polyXInt(part.edge[0] + yi * part.step[0]);
  const std::int32_t right = polyXInt(part.edge[1] + yi * part.step[1]);
  const auto x0 = static_cast<int>(wrapSigned(left, part.half));
  return {x0, x0 + (right - left), x0 - static_cast<std::int64_t>(left)};
}

// `area` narrowed to the columns any of its rows covers. Each edge is linear in the row.
RecordRect coveredColumns(const TrianglePart &part, RecordRect area) {
  const PlottedSpan a = plottedSpan(part, area.y0);
  const PlottedSpan b = plottedSpan(part, area.y1 - 1);
  if (a.shift == b.shift) {
    area.x0 = std::max(std::min(a.x0, b.x0), area.x0);
    area.x1 = std::min(std::max(a.x1, b.x1), area.x1);
  }
  return area;
}

std::int64_t calcis(const TriVertex vertices[3], std::int32_t TriVertex::*x, std::int32_t TriVertex::*y) {
  const TriVertex &A = vertices[0];
  const TriVertex &B = vertices[1];
  const TriVertex &C = vertices[2];
  return static_cast<std::int64_t>(B.*x - A.*x) * (C.*y - B.*y) -
         static_cast<std::int64_t>(C.*x - B.*x) * (B.*y - A.*y);
}

// gpu_polygon.c DrawTriangle's vertex order and edge walk, in VRAM-space pixels at `s`.
struct TriangleWalk {
  TriVertex vertices[3]; // sorted by y
  unsigned core = 0;
  std::int64_t denom = 0;
  TrianglePart parts[2];
};

// gpu.c skips a polygon with an edge as long as VRAM in either axis.
bool withinVramSpan(const TriVertex input[3], int s) {
  const auto spanY = static_cast<std::int64_t>(kVramHeight) * s;
  const auto spanX = static_cast<std::int64_t>(kVramWidth) * s;
  for (int a = 0; a < 3; a++) {
    const TriVertex &p = input[a];
    const TriVertex &q = input[(a + 1) % 3];
    if (std::llabs(static_cast<long long>(q.y) - p.y) >= spanY ||
        std::llabs(static_cast<long long>(q.x) - p.x) >= spanX) {
      return false;
    }
  }
  return true;
}

std::optional<TriangleWalk> walkTriangle(const TriVertex input[3], int s) {
  if (!withinVramSpan(input, s)) {
    return std::nullopt;
  }
  TriangleWalk walk;
  TriVertex *vertices = walk.vertices;
  std::copy(input, input + 3, vertices);
  unsigned cvtemp = 0;
  if (vertices[1].x <= vertices[0].x) {
    cvtemp = vertices[2].x <= vertices[1].x ? (1u << 2) : (1u << 1);
  } else if (vertices[2].x < vertices[0].x) {
    cvtemp = 1u << 2;
  } else {
    cvtemp = 1u << 0;
  }
  if (vertices[2].y < vertices[1].y) {
    std::swap(vertices[2], vertices[1]);
    cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
  }
  if (vertices[1].y < vertices[0].y) {
    std::swap(vertices[1], vertices[0]);
    cvtemp = ((cvtemp >> 1) & 0x1) | ((cvtemp << 1) & 0x2) | (cvtemp & 0x4);
  }
  if (vertices[2].y < vertices[1].y) {
    std::swap(vertices[2], vertices[1]);
    cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
  }
  walk.core = cvtemp >> 1;
  if (vertices[0].y == vertices[2].y) {
    return std::nullopt;
  }
  walk.denom = calcis(vertices, &TriVertex::x, &TriVertex::y);
  if (walk.denom == 0) {
    return std::nullopt;
  }

  const std::uint64_t baseCoord = makePolyXFP(vertices[0].x);
  const std::int64_t baseStep = makePolyXFPStep(vertices[2].x - vertices[0].x, vertices[2].y - vertices[0].y);
  std::int64_t boundUpper = 0;
  bool rightFacing = false;
  if (vertices[1].y == vertices[0].y) {
    rightFacing = vertices[1].x > vertices[0].x;
  } else {
    boundUpper = makePolyXFPStep(vertices[1].x - vertices[0].x, vertices[1].y - vertices[0].y);
    rightFacing = boundUpper > baseStep;
  }
  const std::int64_t boundLower = vertices[2].y == vertices[1].y
                                      ? 0
                                      : makePolyXFPStep(vertices[2].x - vertices[1].x, vertices[2].y - vertices[1].y);
  const unsigned vo = walk.core != 0 ? 1u : 0u;
  const unsigned vp = walk.core == 2 ? 3u : 0u;
  struct Tripart {
    std::uint64_t coord[2] = {};
    std::uint64_t step[2] = {};
    std::int32_t yCoord = 0;
    std::int32_t yBound = 0;
    bool decrement = false;
  } tripart[2];
  const auto along = [&](std::int32_t y) {
    return baseCoord + static_cast<std::uint64_t>(static_cast<std::int64_t>(y - vertices[0].y) * baseStep);
  };
  const int r = rightFacing ? 1 : 0;
  {
    Tripart &part = tripart[vo];
    part.yCoord = vertices[0 ^ vo].y;
    part.yBound = vertices[1 ^ vo].y;
    part.coord[r] = makePolyXFP(vertices[0 ^ vo].x);
    part.step[r] = static_cast<std::uint64_t>(boundUpper);
    part.coord[1 - r] = along(vertices[vo].y);
    part.step[1 - r] = static_cast<std::uint64_t>(baseStep);
    part.decrement = vo != 0;
  }
  {
    Tripart &part = tripart[vo ^ 1];
    part.yCoord = vertices[1 ^ vp].y;
    part.yBound = vertices[2 ^ vp].y;
    part.coord[r] = makePolyXFP(vertices[1 ^ vp].x);
    part.step[r] = static_cast<std::uint64_t>(boundLower);
    part.coord[1 - r] = along(vertices[1 ^ vp].y);
    part.step[1 - r] = static_cast<std::uint64_t>(baseStep);
    part.decrement = vp != 0;
  }

  // gpu.c plots 11-bit rows: a part stops at the clip, before its rows could wrap, so one shift serves it.
  const std::int64_t half = static_cast<std::int64_t>(kVramWidth) * s;
  for (int i = 0; i < 2; i++) {
    const Tripart &rows = tripart[i];
    TrianglePart &part = walk.parts[i];
    const std::int32_t first = rows.decrement ? rows.yCoord - 1 : rows.yCoord;
    const std::int32_t count = rows.decrement ? rows.yCoord - rows.yBound : rows.yBound - rows.yCoord;
    part.half = half;
    part.rowShift = static_cast<std::int32_t>(wrapSigned(first, half) - first);
    part.rowLow = (rows.decrement ? rows.yBound : rows.yCoord) + part.rowShift;
    part.rowHigh = part.rowLow + std::max(count, 0);
    for (int side = 0; side < 2; side++) {
      part.step[side] = rows.step[side];
      part.edge[side] =
          rows.coord[side] - static_cast<std::uint64_t>(static_cast<std::int64_t>(rows.yCoord)) * rows.step[side];
    }
  }
  return walk;
}

void storeWords(RecordOp &op, int index, std::uint64_t value) {
  op[static_cast<std::size_t>(index)] = static_cast<std::uint32_t>(value);
  op[static_cast<std::size_t>(index) + 1] = static_cast<std::uint32_t>(value >> 32);
}

} // namespace

class RecordPlanner::Builder {
public:
  void begin(int scale,
             std::span<const std::uint16_t> clutPool,
             std::span<const std::uint16_t> uploadPool,
             std::span<const RecordCanvas> canvases) {
    plan_.scale = scale;
    plan_.ops.clear();
    for (std::vector<RecordQuad> &quads : plan_.quads) {
      quads.clear();
    }
    plan_.batches.clear();
    plan_.clutPool = clutPool;
    plan_.uploadPool = uploadPool;
    canvases_ = canvases;
    batchStart_ = {};
    lines_ = 0;
    target(0);
  }

  void addPrimitive(const DrawPrimitive &primitive);
  void addFill(const present::VramFill &fill);
  void addCopy(const present::VramCopy &copy);
  void addUpload(const present::VramUpload &upload);

  // `sequence` is the planned record's, 0 for a VRAM upload.
  const RecordRasterPlan &finish(std::uint64_t sequence) {
    closeBatch();
    std::size_t quads = 0;
    for (const std::vector<RecordQuad> &plane : plan_.quads) {
      quads += plane.size();
    }
    lucent::debug("record_plan",
                  "record {} scale {}: {} ops, {} quads, {} batches, {} lines",
                  sequence,
                  plan_.scale,
                  plan_.ops.size(),
                  quads,
                  plan_.batches.size(),
                  lines_);
    return plan_;
  }

private:
  struct TriangleSetup {
    std::uint32_t base[5] = {};
    std::uint32_t dx[5] = {};
    std::uint32_t dy[5] = {};
  };
  struct PlaneQuad {
    int plane = 0;
    RecordQuad quad;
  };
  struct PlaneRect {
    int plane = 0;
    RecordRect rect; // native pixels of the plane
  };

  void addPolygon(const DrawPrimitive &primitive);
  // `scaled` at the plan scale draws; `native`, the same vertices at 1x, decides what the op writes.
  void addTriangle(
      const TriVertex scaled[3], const TriVertex native[3], const DrawPrimitive &primitive, bool offU, bool offV);
  void addSprite(const DrawPrimitive &primitive);
  void addLine(const DrawPrimitive &primitive);
  // A flat rectangle spanning a buffer's columns spans its canvas's, as a fill does.
  void addCanvasMargins(const DrawPrimitive &primitive);

  PlaneClips planeClips(const RecordDrawState &state) const;
  void target(int plane);
  RecordOp primitiveOp(const DrawPrimitive &primitive, RecordOpKind kind) const;
  void textureReads(const RecordDrawState &state, const TexelRange &u, const TexelRange &v);
  // A quad in VRAM-space scaled pixels placed on the current target plane, its pixels marked written separately.
  void place(RecordQuad quad);
  // VRAM-space native pixels the op writes on the current target plane.
  void markWritten(const RecordRect &rect);
  // A VRAM-space native rect drawn and written on the current target plane.
  void emit(const RecordRect &rect, std::uint32_t param = 0);
  // A VRAM-space scaled quad drawn on the current target plane, `written` its native footprint.
  void emit(const RecordRect &written, const RecordQuad &quad);
  // VRAM rects written by a transfer, into VRAM and where they land in each canvas buffer.
  void emitTransferRects(const WrappedRects &rects);
  // Keeps the op when its native footprint reached a plane, whatever its quads at this scale.
  void commit(const RecordOp &op, bool readsDestination);
  void closeBatch();

  RecordRasterPlan plan_;
  std::span<const RecordCanvas> canvases_;
  int plane_ = 0;
  int originX_ = 0;
  int originY_ = 0;
  std::array<WrittenMap, kRecordPlanes> written_;
  std::array<std::uint32_t, kRecordPlanes> batchStart_{};
  std::vector<PlaneQuad> opQuads_;
  std::vector<PlaneRect> opWrites_;
  std::vector<RecordRect> opReads_;
  bool reaches_ = false;
  int lines_ = 0; // line primitives planned
};

PlaneClips RecordPlanner::Builder::planeClips(const RecordDrawState &state) const {
  const Clip area = nativeClip(state);
  PlaneClips clips;
  clips.push_back({0, area});
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    const RecordCanvas &canvas = canvases_[i];
    const RecordRect &buffer = canvas.buffer;
    const bool drawsBuffer = drawAreaSpansBuffer(state, buffer);
    Clip clip{std::max(area.x0, buffer.x0),
              std::max(area.y0, buffer.y0),
              std::min(area.x1, buffer.x1 - 1),
              std::min(area.y1, buffer.y1 - 1)};
    if (drawsBuffer) {
      clip.x0 = buffer.x0 - canvas.margin;
      clip.x1 = buffer.x1 - 1 + canvas.margin;
    }
    if (clip.x0 <= clip.x1 && clip.y0 <= clip.y1) {
      clips.push_back(PlaneClip{1 + static_cast<int>(i), clip});
    }
  }
  return clips;
}

void RecordPlanner::Builder::target(int plane) {
  plane_ = plane;
  originX_ = plane == 0 ? 0 : canvases_[static_cast<std::size_t>(plane - 1)].originX();
  originY_ = plane == 0 ? 0 : canvases_[static_cast<std::size_t>(plane - 1)].originY();
}

RecordOp RecordPlanner::Builder::primitiveOp(const DrawPrimitive &primitive, RecordOpKind kind) const {
  const RecordDrawState &state = primitive.state;
  RecordOp op{};
  std::uint32_t flags = static_cast<std::uint32_t>(kind);
  flags |= primitive.textured ? kOpTextured : 0u;
  flags |= primitive.modulate ? kOpModulate : 0u;
  flags |= primitive.gouraud ? kOpGouraud : 0u;
  flags |= primitive.semiTransparent ? kOpSemi : 0u;
  flags |= state.maskCheck ? kOpMaskCheck : 0u;
  flags |= state.maskSet ? kOpMaskSet : 0u;
  flags |= static_cast<std::uint32_t>(state.blendMode & 3) << kOpBlendShift;
  flags |= static_cast<std::uint32_t>(state.texMode & 3) << kOpTexModeShift;
  if (state.skipRowParity >= 0) {
    flags |= kOpSkipRows | (state.skipRowParity != 0 ? kOpSkipParity : 0u);
  }
  flags |= primitive.flipX ? kOpFlipX : 0u;
  flags |= primitive.flipY ? kOpFlipY : 0u;
  op[kOpFlags] = flags;
  const present::RecordVertex &v0 = primitive.vertices[0];
  op[kOpColour] = static_cast<std::uint32_t>(v0.r) | (static_cast<std::uint32_t>(v0.g) << 8) |
                  (static_cast<std::uint32_t>(v0.b) << 16);
  // gpu.c RecalcTexWindowStuff.
  const auto tww = static_cast<std::uint32_t>(state.windowMaskX);
  const auto twh = static_cast<std::uint32_t>(state.windowMaskY);
  const auto twx = static_cast<std::uint32_t>(state.windowOffsetX);
  const auto twy = static_cast<std::uint32_t>(state.windowOffsetY);
  op[kOpWindowXAnd] = ~(tww << 3);
  op[kOpWindowXAdd] = ((twx & tww) << 3) + (static_cast<std::uint32_t>(state.texPageX) << (2 - state.texMode));
  op[kOpWindowYAnd] = ~(twh << 3);
  op[kOpWindowYAdd] = ((twy & twh) << 3) + static_cast<std::uint32_t>(state.texPageY);
  op[kOpClut] = primitive.clutOffset == present::kNoClut ? 0u : primitive.clutOffset;
  return op;
}

void RecordPlanner::Builder::textureReads(const RecordDrawState &state, const TexelRange &u, const TexelRange &v) {
  if (u.empty() || v.empty()) {
    return;
  }
  const int uAnd = static_cast<int>(~(static_cast<unsigned>(state.windowMaskX) << 3) & 0xFFu);
  const int vAnd = static_cast<int>(~(static_cast<unsigned>(state.windowMaskY) << 3) & 0xFFu);
  const int uLow = state.windowMaskX != 0 ? 0 : u.low;
  const int uHigh = state.windowMaskX != 0 ? uAnd : u.high;
  const int vLow = state.windowMaskY != 0 ? 0 : v.low;
  const int vHigh = state.windowMaskY != 0 ? vAnd : v.high;
  const int shift = 2 - state.texMode;
  const int uAdd = ((state.windowOffsetX & state.windowMaskX) << 3) + (state.texPageX << shift);
  const int vAdd = ((state.windowOffsetY & state.windowMaskY) << 3) + state.texPageY;
  const int columnLow = (uLow + uAdd) >> shift;
  const int columnHigh = (uHigh + uAdd) >> shift;
  for (const RecordRect &rect : wrappedRects({columnLow, vLow + vAdd, columnHigh - columnLow + 1, vHigh - vLow + 1})) {
    opReads_.push_back(rect);
  }
}

void RecordPlanner::Builder::place(RecordQuad quad) {
  const int s = plan_.scale;
  quad.x0 -= originX_ * s;
  quad.y0 -= originY_ * s;
  quad.x1 -= originX_ * s;
  quad.y1 -= originY_ * s;
  opQuads_.push_back(PlaneQuad{plane_, quad});
}

void RecordPlanner::Builder::markWritten(const RecordRect &rect) {
  opWrites_.push_back(
      PlaneRect{plane_, {rect.x0 - originX_, rect.y0 - originY_, rect.x1 - originX_, rect.y1 - originY_}});
}

void RecordPlanner::Builder::emit(const RecordRect &rect, std::uint32_t param) {
  const int s = plan_.scale;
  emit(rect, {.x0 = rect.x0 * s, .y0 = rect.y0 * s, .x1 = rect.x1 * s, .y1 = rect.y1 * s, .param = param});
}

void RecordPlanner::Builder::emit(const RecordRect &written, const RecordQuad &quad) {
  place(quad);
  markWritten(written);
  reaches_ = true;
}

void RecordPlanner::Builder::emitTransferRects(const WrappedRects &rects) {
  target(0);
  for (const RecordRect &rect : rects) {
    emit(rect);
  }
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    const RecordRect &buffer = canvases_[i].buffer;
    target(1 + static_cast<int>(i));
    for (const RecordRect &rect : rects) {
      const RecordRect inside{std::max(rect.x0, buffer.x0),
                              std::max(rect.y0, buffer.y0),
                              std::min(rect.x1, buffer.x1),
                              std::min(rect.y1, buffer.y1)};
      if (inside.x0 < inside.x1 && inside.y0 < inside.y1) {
        emit(inside);
      }
    }
  }
  target(0);
}

void RecordPlanner::Builder::commit(const RecordOp &op, bool readsDestination) {
  if (!reaches_) {
    opQuads_.clear();
    opWrites_.clear();
    opReads_.clear();
    return;
  }
  reaches_ = false;
  bool conflict = false;
  for (const RecordRect &read : opReads_) {
    conflict = conflict || written_[0].any(read);
  }
  if (readsDestination) {
    for (const PlaneRect &write : opWrites_) {
      conflict = conflict || written_[static_cast<std::size_t>(write.plane)].any(write.rect);
    }
  }
  if (conflict) {
    closeBatch();
  }
  const auto opIndex = static_cast<std::uint32_t>(plan_.ops.size());
  plan_.ops.push_back(op);
  const int s = plan_.scale;
  for (PlaneQuad &pending : opQuads_) {
    pending.quad.op = opIndex;
    plan_.quads[static_cast<std::size_t>(pending.plane)].push_back(pending.quad);
    const RecordQuad &quad = pending.quad;
    written_[static_cast<std::size_t>(pending.plane)].extend(
        {quad.x0 / s, quad.y0 / s, (quad.x1 + s - 1) / s, (quad.y1 + s - 1) / s});
  }
  for (const PlaneRect &write : opWrites_) {
    written_[static_cast<std::size_t>(write.plane)].mark(write.rect);
  }
  opQuads_.clear();
  opWrites_.clear();
  opReads_.clear();
}

void RecordPlanner::Builder::closeBatch() {
  RecordBatch batch;
  bool any = false;
  for (std::size_t plane = 0; plane < kRecordPlanes; plane++) {
    const auto end = static_cast<std::uint32_t>(plan_.quads[plane].size());
    batch.planes[plane] =
        RecordPlaneBatch{batchStart_[plane], end, written_[plane].empty() ? RecordRect{} : written_[plane].bounds()};
    any = any || end > batchStart_[plane];
    batchStart_[plane] = end;
    written_[plane].clear();
  }
  if (any) {
    plan_.batches.push_back(batch);
  }
}

void RecordPlanner::Builder::addPrimitive(const DrawPrimitive &primitive) {
  switch (primitive.kind) {
  case PrimitiveKind::Polygon:
    addPolygon(primitive);
    break;
  case PrimitiveKind::Sprite:
    addSprite(primitive);
    break;
  case PrimitiveKind::Line:
    addLine(primitive);
    break;
  }
  addCanvasMargins(primitive);
}

void RecordPlanner::Builder::addPolygon(const DrawPrimitive &primitive) {
  const int s = plan_.scale;
  TriVertex scaled[4];
  TriVertex native[4];
  for (std::size_t i = 0; i < 4; i++) {
    scaled[i] = scaledVertex(primitive.vertices[i], s);
    native[i] = scaledVertex(primitive.vertices[i], 1);
  }
  TriVertex first[3] = {scaled[0], scaled[1], scaled[2]};
  // The second triangle starts from the first's vertices before the UV adjustment.
  TriVertex second[3] = {scaled[1], scaled[2], scaled[3]};
  bool offU = false;
  bool offV = false;
  if (primitive.textured) {
    adjustUv(first, s, offU, offV);
  }
  addTriangle(first, native, primitive, offU, offV);
  if (primitive.vertexCount == 4) {
    if (primitive.textured) {
      adjustUv(second, s, offU, offV);
    }
    addTriangle(second, native + 1, primitive, offU, offV);
  }
}

void RecordPlanner::Builder::addTriangle(
    const TriVertex scaled[3], const TriVertex native[3], const DrawPrimitive &primitive, bool offU, bool offV) {
  const int s = plan_.scale;
  const std::optional<TriangleWalk> walk = walkTriangle(scaled, s);
  if (!walk || !withinVramSpan(native, 1)) {
    return;
  }
  // The native walk decides what the op writes, so ops and batches are the same at every scale. A triangle whose
  // places give it area where its words give none (a sliver a clip cut) writes the native pixels its quads
  // reach.
  const std::optional<TriangleWalk> nativeWalk = s == 1 ? walk : walkTriangle(native, 1);
  const TriVertex *vertices = walk->vertices;
  TriangleSetup setup;
  std::int32_t TriVertex::*const channels[5] = {
      &TriVertex::r, &TriVertex::g, &TriVertex::b, &TriVertex::u, &TriVertex::v};
  const bool interpolated[5] = {
      primitive.gouraud, primitive.gouraud, primitive.gouraud, primitive.textured, primitive.textured};
  for (int c = 0; c < 5; c++) {
    if (interpolated[c]) {
      setup.dx[c] = static_cast<std::uint32_t>(calcis(vertices, channels[c], &TriVertex::y) * 4096 / walk->denom) << 12;
      setup.dy[c] = static_cast<std::uint32_t>(calcis(vertices, &TriVertex::x, channels[c]) * 4096 / walk->denom) << 12;
    }
  }
  const TriVertex &coreVertex = vertices[walk->core];
  const auto uvBias = static_cast<std::uint32_t>(2048 / s);
  for (int c = 0; c < 5; c++) {
    const auto value = static_cast<std::uint32_t>(coreVertex.*channels[c]);
    setup.base[c] = ((value << 12) + (c < 3 ? 2048u : uvBias)) << 12;
  }
  if (s > 1) {
    const auto offset = static_cast<std::uint32_t>(4096 - 4096 / s) << 12;
    setup.base[3] += offU ? offset : 0u;
    setup.base[4] += offV ? offset : 0u;
  }
  for (int c = 0; c < 5; c++) {
    setup.base[c] += setup.dx[c] * static_cast<std::uint32_t>(-coreVertex.x);
    setup.base[c] += setup.dy[c] * static_cast<std::uint32_t>(-coreVertex.y);
  }

  for (const PlaneClip &planeClip : planeClips(primitive.state)) {
    target(planeClip.plane);
    const Clip &clip = planeClip.clip;
    for (int i = 0; i < 2; i++) {
      const TrianglePart &part = walk->parts[i];
      const int rowLow = std::max(part.rowLow, clip.y0 * s);
      const int rowHigh = std::min(part.rowHigh, (clip.y1 + 1) * s);
      if (rowLow < rowHigh) {
        const RecordRect covered = coveredColumns(part, {clip.x0 * s, rowLow, (clip.x1 + 1) * s, rowHigh});
        if (covered.x0 < covered.x1) {
          place({.x0 = covered.x0,
                 .y0 = covered.y0,
                 .x1 = covered.x1,
                 .y1 = covered.y1,
                 .param = static_cast<std::uint32_t>(i)});
          if (!nativeWalk) {
            reaches_ = true;
            markWritten(nativeReach(covered, s));
          }
        }
      }
      if (!nativeWalk) {
        continue;
      }
      const TrianglePart &nativePart = nativeWalk->parts[i];
      const int nativeLow = std::max(nativePart.rowLow, clip.y0);
      const int nativeHigh = std::min(nativePart.rowHigh, clip.y1 + 1);
      reaches_ = reaches_ || nativeLow < nativeHigh;
      for (int row = nativeLow; row < nativeHigh; row++) {
        const RecordRect covered = coveredColumns(nativePart, {clip.x0, row, clip.x1 + 1, row + 1});
        if (covered.x0 < covered.x1) {
          markWritten(covered);
        }
      }
    }
  }
  target(0);

  RecordOp op = primitiveOp(primitive, RecordOpKind::Triangle);
  for (std::size_t c = 0; c < 5; c++) {
    op[kOpBase + c] = setup.base[c];
    op[kOpDx + c] = setup.dx[c];
    op[kOpDy + c] = setup.dy[c];
  }
  for (int i = 0; i < 2; i++) {
    const int words = kOpParts + i * kPartWords;
    const TrianglePart &part = walk->parts[i];
    op[static_cast<std::size_t>(words) + kPartRowShift] = static_cast<std::uint32_t>(part.rowShift);
    storeWords(op, words + kPartLeft, part.edge[0]);
    storeWords(op, words + kPartRight, part.edge[1]);
    storeWords(op, words + kPartLeftStep, part.step[0]);
    storeWords(op, words + kPartRightStep, part.step[1]);
  }
  if (primitive.textured) {
    // Every covered pixel lies within a pixel of the vertices' hull, where the interpolant is linear.
    TexelRange ranges[2];
    for (int c = 3; c < 5; c++) {
      const auto dx = static_cast<std::int64_t>(static_cast<std::int32_t>(setup.dx[c]));
      const auto dy = static_cast<std::int64_t>(static_cast<std::int32_t>(setup.dy[c]));
      std::int64_t low = INT64_MAX;
      std::int64_t high = INT64_MIN;
      for (int v = 0; v < 3; v++) {
        const std::int64_t at = static_cast<std::int64_t>(setup.base[c]) + dx * vertices[v].x + dy * vertices[v].y;
        low = std::min(low, at);
        high = std::max(high, at);
      }
      const std::int64_t pad = std::llabs(dx) + std::llabs(dy) + 1;
      low -= pad;
      high += pad;
      TexelRange &range = ranges[c - 3];
      if ((low >> 32) != (high >> 32)) {
        range.all();
      } else {
        range.cover(static_cast<int>((static_cast<std::uint64_t>(low) & 0xFFFFFFFFull) >> 24));
        range.cover(static_cast<int>((static_cast<std::uint64_t>(high) & 0xFFFFFFFFull) >> 24));
      }
    }
    textureReads(primitive.state, ranges[0], ranges[1]);
  }
  commit(op, primitive.semiTransparent || primitive.state.maskCheck);
}

// gpu_sprite.c DrawSprite. The op keeps the unclipped origin; u and v wrap at 256 either way.
void RecordPlanner::Builder::addSprite(const DrawPrimitive &primitive) {
  const present::RecordVertex &vertex = primitive.vertices[0];
  std::uint8_t u = 0;
  std::uint8_t v = 0;
  int uIncrement = 1;
  int vIncrement = 1;
  if (primitive.textured) {
    u = vertex.u;
    v = vertex.v;
    if (primitive.flipX) {
      uIncrement = -1;
      u |= 1;
    }
    if (primitive.flipY) {
      vIncrement = -1;
    }
  }
  TexelRange uRange;
  TexelRange vRange;
  const auto texels = [](TexelRange &range, int first, int count, int increment) {
    const int last = first + (count - 1) * increment;
    if (last < 0 || last > 255) {
      range.all();
    } else {
      range.cover(first);
      range.cover(last);
    }
  };
  // The sprite moves whole by its place; native scale keeps the guest's pixels.
  const int s = plan_.scale;
  const std::int32_t shiftX = placeOffset(vertex.subX, s);
  const std::int32_t shiftY = placeOffset(vertex.subY, s);
  for (const PlaneClip &planeClip : planeClips(primitive.state)) {
    const Clip &clip = planeClip.clip;
    const RecordRect covered{std::max(vertex.x * s + shiftX, clip.x0 * s),
                             std::max(vertex.y * s + shiftY, clip.y0 * s),
                             std::min((vertex.x + primitive.width) * s + shiftX, (clip.x1 + 1) * s),
                             std::min((vertex.y + primitive.height) * s + shiftY, (clip.y1 + 1) * s)};
    if (covered.x1 <= covered.x0 || covered.y1 <= covered.y0) {
      continue;
    }
    target(planeClip.plane);
    place({.x0 = covered.x0, .y0 = covered.y0, .x1 = covered.x1, .y1 = covered.y1});
    markWritten(nativeReach(covered, s));
    reaches_ = true;
    if (primitive.textured) {
      const int xStart = floorDivide(covered.x0 - shiftX, s);
      const int yStart = floorDivide(covered.y0 - shiftY, s);
      const auto u0 = static_cast<std::uint8_t>(u + (xStart - vertex.x) * uIncrement);
      const auto v0 = static_cast<std::uint8_t>(v + (yStart - vertex.y) * vIncrement);
      texels(uRange, u0, floorDivide(covered.x1 - 1 - shiftX, s) + 1 - xStart, uIncrement);
      texels(vRange, v0, floorDivide(covered.y1 - 1 - shiftY, s) + 1 - yStart, vIncrement);
    }
  }
  target(0);
  RecordOp op = primitiveOp(primitive, RecordOpKind::Sprite);
  op[kOpOriginX] = static_cast<std::uint32_t>(vertex.x);
  op[kOpOriginY] = static_cast<std::uint32_t>(vertex.y);
  op[kOpSourceX] = u;
  op[kOpSourceY] = v;
  op[kOpShiftX] = static_cast<std::uint32_t>(shiftX);
  op[kOpShiftY] = static_cast<std::uint32_t>(shiftY);
  if (primitive.textured) {
    textureReads(primitive.state, uRange, vRange);
  }
  commit(op, primitive.semiTransparent || primitive.state.maskCheck);
}

// gpu_line.c DrawLine: the pixels are walked here and drawn as single-pixel quads.
void RecordPlanner::Builder::addLine(const DrawPrimitive &primitive) {
  present::RecordVertex p0 = primitive.vertices[0];
  present::RecordVertex p1 = primitive.vertices[1];
  const int deltaX = std::abs(p1.x - p0.x);
  const int deltaY = std::abs(p1.y - p0.y);
  if (deltaX >= kVramWidth || deltaY >= kVramHeight) {
    return;
  }
  lines_++;
  const int k = std::max(deltaX, deltaY);
  if (p0.x > p1.x && k != 0) {
    std::swap(p0, p1);
  }
  std::int64_t dxdk = 0;
  std::int64_t dydk = 0;
  std::int32_t drdk = 0;
  std::int32_t dgdk = 0;
  std::int32_t dbdk = 0;
  if (k != 0) {
    dxdk = lineDivide(p1.x - p0.x, k);
    dydk = lineDivide(p1.y - p0.y, k);
    if (primitive.gouraud) {
      drdk = static_cast<std::int32_t>(static_cast<std::uint32_t>(p1.r - p0.r) << 12) / k;
      dgdk = static_cast<std::int32_t>(static_cast<std::uint32_t>(p1.g - p0.g) << 12) / k;
      dbdk = static_cast<std::int32_t>(static_cast<std::uint32_t>(p1.b - p0.b) << 12) / k;
    }
  }
  std::uint64_t x = (static_cast<std::uint64_t>(static_cast<std::int64_t>(p0.x)) << 32) | (1ull << 31);
  std::uint64_t y = (static_cast<std::uint64_t>(static_cast<std::int64_t>(p0.y)) << 32) | (1ull << 31);
  x -= 1024;
  if (dydk < 0) {
    y -= 1024;
  }
  std::uint32_t r = (static_cast<std::uint32_t>(p0.r) << 12) | 2048u;
  std::uint32_t g = (static_cast<std::uint32_t>(p0.g) << 12) | 2048u;
  std::uint32_t b = (static_cast<std::uint32_t>(p0.b) << 12) | 2048u;
  const PlaneClips clips = planeClips(primitive.state);
  const int s = plan_.scale;
  for (int i = 0; i <= k; i++) {
    // Each native pixel of the walk drawn at the line's place there.
    const float along = k == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(k);
    const std::int32_t shiftX = placeOffset(p0.subX + (p1.subX - p0.subX) * along, s);
    const std::int32_t shiftY = placeOffset(p0.subY + (p1.subY - p0.subY) * along, s);
    const int px = static_cast<int>((x >> 32) & 2047u);
    const int py = static_cast<int>((y >> 32) & 2047u);
    if (!skipsRow(primitive.state, py)) {
      const std::uint32_t cr = primitive.gouraud ? static_cast<std::uint8_t>(r >> 12) : p0.r;
      const std::uint32_t cg = primitive.gouraud ? static_cast<std::uint8_t>(g >> 12) : p0.g;
      const std::uint32_t cb = primitive.gouraud ? static_cast<std::uint8_t>(b >> 12) : p0.b;
      const std::uint32_t colour = cr | (cg << 8) | (cb << 16);
      for (const PlaneClip &planeClip : clips) {
        // A canvas reaches left of VRAM, where the 11-bit position reads as negative.
        const int nx = planeClip.plane == 0 ? px : ((px ^ 1024) - 1024);
        const int ny = planeClip.plane == 0 ? py : ((py ^ 1024) - 1024);
        const Clip &clip = planeClip.clip;
        if (nx >= clip.x0 && nx <= clip.x1 && ny >= clip.y0 && ny <= clip.y1) {
          target(planeClip.plane);
          const RecordQuad quad{.x0 = std::max(nx * s + shiftX, clip.x0 * s),
                                .y0 = std::max(ny * s + shiftY, clip.y0 * s),
                                .x1 = std::min((nx + 1) * s + shiftX, (clip.x1 + 1) * s),
                                .y1 = std::min((ny + 1) * s + shiftY, (clip.y1 + 1) * s),
                                .param = colour};
          if (quad.x0 < quad.x1 && quad.y0 < quad.y1) {
            place(quad);
          }
          markWritten({nx, ny, nx + 1, ny + 1});
          reaches_ = true;
        }
      }
    }
    x += static_cast<std::uint64_t>(dxdk);
    y += static_cast<std::uint64_t>(dydk);
    r += static_cast<std::uint32_t>(drdk);
    g += static_cast<std::uint32_t>(dgdk);
    b += static_cast<std::uint32_t>(dbdk);
  }
  target(0);
  commit(primitiveOp(primitive, RecordOpKind::LinePixel), primitive.semiTransparent || primitive.state.maskCheck);
}

void RecordPlanner::Builder::addCanvasMargins(const DrawPrimitive &primitive) {
  const std::optional<FlatRectangle> flat = flatRectangle(primitive);
  if (!flat || flat->subX0 != 0.0f || flat->subX1 != 0.0f) {
    return;
  }
  const RecordRect &rect = flat->rect;
  const int s = plan_.scale;
  for (const PlaneClip &planeClip : planeClips(primitive.state)) {
    if (planeClip.plane == 0) {
      continue;
    }
    const RecordCanvas &canvas = canvases_[static_cast<std::size_t>(planeClip.plane - 1)];
    const Clip &clip = planeClip.clip;
    // The margins share the rectangle's rows at this scale, a placed edge's fraction included.
    const int y0 = std::max(scaledCoordinate(rect.y0, flat->subY0, s), clip.y0 * s);
    const int y1 = std::min(scaledCoordinate(rect.y1, flat->subY1, s), (clip.y1 + 1) * s);
    if (rect.x0 != canvas.buffer.x0 || rect.x1 != canvas.buffer.x1 || y0 >= y1) {
      continue;
    }
    const int rowFirst = y0 / s;
    const int rowEnd = (y1 + s - 1) / s;
    target(planeClip.plane);
    const int left = std::max(canvas.originX(), clip.x0);
    const int right = std::min(canvas.buffer.x1 + canvas.margin, clip.x1 + 1);
    if (left < canvas.buffer.x0) {
      emit({left, rowFirst, canvas.buffer.x0, rowEnd}, {left * s, y0, canvas.buffer.x0 * s, y1});
    }
    if (right > canvas.buffer.x1) {
      emit({canvas.buffer.x1, rowFirst, right, rowEnd}, {canvas.buffer.x1 * s, y0, right * s, y1});
    }
  }
  target(0);
  commit(primitiveOp(primitive, RecordOpKind::Sprite), primitive.semiTransparent || primitive.state.maskCheck);
}

void RecordPlanner::Builder::addFill(const present::VramFill &fill) {
  const WrappedRects rects = wrappedRects({fill.x, fill.y, fill.width, fill.height});
  emitTransferRects(rects);
  // A fill across the buffer's columns clears the canvas's whole width on those rows.
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    const RecordCanvas &canvas = canvases_[i];
    target(1 + static_cast<int>(i));
    for (const RecordRect &rect : rects) {
      const int y0 = std::max(rect.y0, canvas.buffer.y0);
      const int y1 = std::min(rect.y1, canvas.buffer.y1);
      if (rect.x0 <= canvas.buffer.x0 && rect.x1 >= canvas.buffer.x1 && y0 < y1) {
        emit({canvas.originX(), y0, canvas.buffer.x0, y1});
        emit({canvas.buffer.x1, y0, canvas.buffer.x1 + canvas.margin, y1});
      }
    }
  }
  target(0);
  RecordOp op{};
  op[kOpFlags] = static_cast<std::uint32_t>(RecordOpKind::Fill);
  if (fill.skipRowParity >= 0) {
    op[kOpFlags] |= kOpSkipRows | (fill.skipRowParity != 0 ? kOpSkipParity : 0u);
  }
  op[kOpColour] = fill.value;
  commit(op, false);
}

void RecordPlanner::Builder::addCopy(const present::VramCopy &copy) {
  RecordOp op{};
  op[kOpFlags] = static_cast<std::uint32_t>(RecordOpKind::Copy) | (copy.maskCheck ? kOpMaskCheck : 0u) |
                 (copy.maskSet ? kOpMaskSet : 0u);
  const bool overlapping = RingRun{copy.srcX, copy.width}.overlaps({copy.dstX, copy.width}, kVramWidth) &&
                           RingRun{copy.srcY, copy.height}.overlaps({copy.dstY, copy.height}, kVramHeight);
  // `dst` is the written rect; `src` the origin it reads from.
  const auto copyPart = [&](VramPoint src, VramRect dst) {
    emitTransferRects(wrappedRects(dst));
    for (const RecordRect &rect : wrappedRects({src.x, src.y, dst.width, dst.height})) {
      opReads_.push_back(rect);
    }
    op[kOpOriginX] = static_cast<std::uint32_t>(positiveMod(dst.x, kVramWidth));
    op[kOpOriginY] = static_cast<std::uint32_t>(positiveMod(dst.y, kVramHeight));
    op[kOpSourceX] = static_cast<std::uint32_t>(positiveMod(src.x, kVramWidth));
    op[kOpSourceY] = static_cast<std::uint32_t>(positiveMod(src.y, kVramHeight));
    commit(op, copy.maskCheck);
  };
  if (!overlapping) {
    copyPart({copy.srcX, copy.srcY}, {copy.dstX, copy.dstY, copy.width, copy.height});
    return;
  }
  // gpu.c copies row by row through a 128-pixel buffer; an overlapping copy sees its own writes.
  constexpr int kChunk = 128;
  for (int row = 0; row < copy.height; row++) {
    for (int column = 0; column < copy.width; column += kChunk) {
      const int width = std::min(kChunk, copy.width - column);
      copyPart({copy.srcX + column, copy.srcY + row}, {copy.dstX + column, copy.dstY + row, width, 1});
    }
  }
}

void RecordPlanner::Builder::addUpload(const present::VramUpload &upload) {
  emitTransferRects(wrappedRects({upload.x, upload.y, upload.width, upload.height}));
  RecordOp op{};
  op[kOpFlags] = static_cast<std::uint32_t>(RecordOpKind::Upload) | (upload.maskCheck ? kOpMaskCheck : 0u) |
                 (upload.maskSet ? kOpMaskSet : 0u);
  op[kOpOriginX] = static_cast<std::uint32_t>(upload.x);
  op[kOpOriginY] = static_cast<std::uint32_t>(upload.y);
  op[kOpSourceX] = static_cast<std::uint32_t>(upload.width);
  op[kOpSourceY] = static_cast<std::uint32_t>(upload.height);
  op[kOpPool] = upload.pixelOffset;
  commit(op, upload.maskCheck);
}

bool drawAreaSpansBuffer(const present::RecordDrawState &state, const RecordRect &buffer) {
  const Clip area = nativeClip(state);
  return buffer.x0 < buffer.x1 && buffer.y0 < buffer.y1 && area.x0 == buffer.x0 && area.x1 == buffer.x1 - 1 &&
         area.y0 < buffer.y1 && area.y1 >= buffer.y0;
}

bool canvasSurvives(const RecordCanvas &canvas, int displayWidth, int margin) {
  return margin > 0 && canvas.margin == margin && canvas.buffer.x1 - canvas.buffer.x0 == displayWidth;
}

RecordPlanner::RecordPlanner() : builder_(std::make_unique<Builder>()) {}

RecordPlanner::~RecordPlanner() = default;

const RecordRasterPlan &
RecordPlanner::plan(const FrameRecord &record, int scale, std::span<const RecordCanvas> canvases) {
  builder_->begin(std::max(scale, 1), record.clutPool(), record.uploadPixels(), canvases);
  for (const present::RecordEntry &entry : record.entries()) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entry)) {
      builder_->addPrimitive(*primitive);
    } else if (const auto *fill = std::get_if<present::VramFill>(&entry)) {
      builder_->addFill(*fill);
    } else if (const auto *copy = std::get_if<present::VramCopy>(&entry)) {
      builder_->addCopy(*copy);
    } else if (const auto *upload = std::get_if<present::VramUpload>(&entry)) {
      builder_->addUpload(*upload);
    }
  }
  return builder_->finish(record.sequence());
}

const RecordRasterPlan &RecordPlanner::planVramUpload(std::span<const std::uint16_t> vram, int scale) {
  builder_->begin(std::max(scale, 1), {}, vram, {});
  present::VramUpload upload;
  upload.width = kVramWidth;
  upload.height = kVramHeight;
  builder_->addUpload(upload);
  return builder_->finish(0);
}

} // namespace psx::gpu
