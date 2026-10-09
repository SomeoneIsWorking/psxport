// record_raster_setup.cpp — gpu.c's polygon, sprite, line and transfer rules, producing RecordPlan work.
#include "record_raster_setup.h"

#include "record_raster_regions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <variant>

namespace psx::gpu {
namespace {

using present::DrawPrimitive;
using present::FrameRecord;
using present::PrimitiveKind;
using present::RecordDrawState;

// gpu.c dither_table.
constexpr int kDitherTable[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

int ditherChannel(int value, int x, int y) {
  return std::clamp((value + kDitherTable[y & 3][x & 3]) >> 3, 0, 31);
}

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
  return Clip{state.clipX0, state.clipY0, state.clipX1, std::min(state.clipY1, kRecordVramHeight - 1)};
}

bool skipsRow(const RecordDrawState &state, int nativeY) {
  return state.skipRowParity >= 0 && (nativeY & 1) == state.skipRowParity;
}

// An inclusive texel coordinate range, or the whole axis.
struct TexelRange {
  int low = 256;
  int high = -1;
  void add(int lowValue, int highValue) {
    low = std::min(low, lowValue);
    high = std::max(high, highValue);
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

class PlanBuilder {
public:
  PlanBuilder(int scale,
              std::span<const std::uint16_t> clutPool,
              std::span<const std::uint16_t> uploadPool,
              std::span<const RecordCanvas> canvases)
      : canvases_(canvases.begin(), canvases.end()) {
    plan_.scale = scale;
    plan_.clutPool = clutPool;
    plan_.uploadPool = uploadPool;
  }

  void addPrimitive(const DrawPrimitive &primitive);
  void addFill(const present::VramFill &fill);
  void addCopy(const present::VramCopy &copy);
  void addUpload(const present::VramUpload &upload);

  RecordRasterPlan finish() {
    closeBatch();
    return std::move(plan_);
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

  void addPolygon(const DrawPrimitive &primitive);
  void addTriangle(TriVertex vertices[3], const DrawPrimitive &primitive, bool offU, bool offV);
  void addSprite(const DrawPrimitive &primitive);
  void addLine(const DrawPrimitive &primitive);

  std::vector<PlaneClip> planeClips(const RecordDrawState &state) const;
  void target(int plane);
  RecordOp primitiveOp(const DrawPrimitive &primitive, RecordOpKind kind) const;
  void textureReads(const RecordDrawState &state, const TexelRange &u, const TexelRange &v);
  RecordRect nativeOf(const RecordQuad &quad) const;
  // A quad in VRAM-space scaled pixels, placed on the current target plane.
  void emit(std::int32_t x0,
            std::int32_t y0,
            std::int32_t x1,
            std::int32_t y1,
            std::uint32_t param,
            std::uint32_t rowOffset = 0);
  // VRAM rects written by a transfer, into VRAM and where they land in each canvas buffer.
  void emitTransferRects(const std::vector<RecordRect> &rects);
  void emitNativeRect(const RecordRect &rect);
  void commit(const RecordOp &op, bool readsDestination);
  void closeBatch();

  RecordRasterPlan plan_;
  std::vector<RecordCanvas> canvases_;
  int plane_ = 0;
  int originX_ = 0;
  int originY_ = 0;
  std::array<WrittenMap, kRecordPlanes> written_;
  std::array<std::uint32_t, kRecordPlanes> batchStart_{};
  std::vector<PlaneQuad> opQuads_;
  std::vector<RecordRect> opReads_;
};

std::vector<PlaneClip> PlanBuilder::planeClips(const RecordDrawState &state) const {
  const Clip area = nativeClip(state);
  std::vector<PlaneClip> clips{{0, area}};
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

void PlanBuilder::target(int plane) {
  plane_ = plane;
  originX_ = plane == 0 ? 0 : canvases_[static_cast<std::size_t>(plane - 1)].originX();
  originY_ = plane == 0 ? 0 : canvases_[static_cast<std::size_t>(plane - 1)].originY();
}

RecordOp PlanBuilder::primitiveOp(const DrawPrimitive &primitive, RecordOpKind kind) const {
  const RecordDrawState &state = primitive.state;
  RecordOp op{};
  std::uint32_t flags = static_cast<std::uint32_t>(kind);
  flags |= primitive.textured ? kOpTextured : 0u;
  flags |= primitive.modulate ? kOpModulate : 0u;
  flags |= primitive.gouraud ? kOpGouraud : 0u;
  flags |= state.dither ? kOpDither : 0u;
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

void PlanBuilder::textureReads(const RecordDrawState &state, const TexelRange &u, const TexelRange &v) {
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
  for (const RecordRect &rect : wrappedRects(columnLow, vLow + vAdd, columnHigh - columnLow + 1, vHigh - vLow + 1)) {
    opReads_.push_back(rect);
  }
}

RecordRect PlanBuilder::nativeOf(const RecordQuad &quad) const {
  const int s = plan_.scale;
  return RecordRect{quad.x0 / s, quad.y0 / s, (quad.x1 + s - 1) / s, (quad.y1 + s - 1) / s};
}

void PlanBuilder::emit(
    std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t param, std::uint32_t rowOffset) {
  const int s = plan_.scale;
  RecordQuad quad;
  quad.x0 = x0 - originX_ * s;
  quad.y0 = y0 - originY_ * s;
  quad.x1 = x1 - originX_ * s;
  quad.y1 = y1 - originY_ * s;
  quad.param = param;
  quad.rowOffset = rowOffset;
  opQuads_.push_back(PlaneQuad{plane_, quad});
}

void PlanBuilder::emitNativeRect(const RecordRect &rect) {
  const int s = plan_.scale;
  emit(rect.x0 * s, rect.y0 * s, rect.x1 * s, rect.y1 * s, 0);
}

void PlanBuilder::emitTransferRects(const std::vector<RecordRect> &rects) {
  target(0);
  for (const RecordRect &rect : rects) {
    emitNativeRect(rect);
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
        emitNativeRect(inside);
      }
    }
  }
  target(0);
}

void PlanBuilder::commit(const RecordOp &op, bool readsDestination) {
  if (opQuads_.empty()) {
    opReads_.clear();
    return;
  }
  bool conflict = false;
  for (const RecordRect &read : opReads_) {
    conflict = conflict || written_[0].any(read);
  }
  if (readsDestination) {
    for (const PlaneQuad &pending : opQuads_) {
      conflict = conflict || written_[static_cast<std::size_t>(pending.plane)].any(nativeOf(pending.quad));
    }
  }
  if (conflict) {
    closeBatch();
  }
  const auto opIndex = static_cast<std::uint32_t>(plan_.ops.size());
  plan_.ops.push_back(op);
  for (PlaneQuad &pending : opQuads_) {
    const auto plane = static_cast<std::size_t>(pending.plane);
    pending.quad.op = opIndex;
    written_[plane].mark(nativeOf(pending.quad));
    plan_.quads[plane].push_back(pending.quad);
  }
  opQuads_.clear();
  opReads_.clear();
}

void PlanBuilder::closeBatch() {
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

void PlanBuilder::addPrimitive(const DrawPrimitive &primitive) {
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
}

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

std::int32_t scaledCoordinate(int whole, float fraction, int scale) {
  return whole * scale + static_cast<std::int32_t>(std::floor(fraction * static_cast<float>(scale) + 0.5f));
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

void PlanBuilder::addPolygon(const DrawPrimitive &primitive) {
  const int s = plan_.scale;
  TriVertex first[3] = {scaledVertex(primitive.vertices[0], s),
                        scaledVertex(primitive.vertices[1], s),
                        scaledVertex(primitive.vertices[2], s)};
  // The second triangle starts from the first's vertices before the UV adjustment.
  TriVertex second[3] = {first[1], first[2], scaledVertex(primitive.vertices[3], s)};
  bool offU = false;
  bool offV = false;
  if (primitive.textured) {
    adjustUv(first, s, offU, offV);
  }
  addTriangle(first, primitive, offU, offV);
  if (primitive.vertexCount == 4) {
    if (primitive.textured) {
      adjustUv(second, s, offU, offV);
    }
    addTriangle(second, primitive, offU, offV);
  }
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

void PlanBuilder::addTriangle(TriVertex vertices[3], const DrawPrimitive &primitive, bool offU, bool offV) {
  const int s = plan_.scale;
  const auto spanY = static_cast<std::int64_t>(512) * s;
  const auto spanX = static_cast<std::int64_t>(1024) * s;
  for (int a = 0; a < 3; a++) {
    const TriVertex &p = vertices[a];
    const TriVertex &q = vertices[(a + 1) % 3];
    if (std::llabs(static_cast<long long>(q.y) - p.y) >= spanY ||
        std::llabs(static_cast<long long>(q.x) - p.x) >= spanX) {
      return;
    }
  }
  // gpu_polygon.c DrawTriangle: core vertex, then sort by y.
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
  const unsigned core = cvtemp >> 1;
  if (vertices[0].y == vertices[2].y) {
    return;
  }
  const TriVertex &A = vertices[0];
  const TriVertex &B = vertices[1];
  const TriVertex &C = vertices[2];
  const auto calcis = [&](auto x, auto y) {
    return static_cast<std::int64_t>(B.*x - A.*x) * (C.*y - B.*y) -
           static_cast<std::int64_t>(C.*x - B.*x) * (B.*y - A.*y);
  };
  const std::int64_t denom = calcis(&TriVertex::x, &TriVertex::y);
  if (denom == 0) {
    return;
  }
  TriangleSetup setup;
  std::int32_t TriVertex::*const channels[5] = {
      &TriVertex::r, &TriVertex::g, &TriVertex::b, &TriVertex::u, &TriVertex::v};
  const bool interpolated[5] = {
      primitive.gouraud, primitive.gouraud, primitive.gouraud, primitive.textured, primitive.textured};
  for (int c = 0; c < 5; c++) {
    if (interpolated[c]) {
      setup.dx[c] = static_cast<std::uint32_t>(calcis(channels[c], &TriVertex::y) * 4096 / denom) << 12;
      setup.dy[c] = static_cast<std::uint32_t>(calcis(&TriVertex::x, channels[c]) * 4096 / denom) << 12;
    }
  }
  const TriVertex &coreVertex = vertices[core];
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
  const unsigned vo = core != 0 ? 1u : 0u;
  const unsigned vp = core == 2 ? 3u : 0u;
  struct Tripart {
    std::uint64_t coord[2] = {};
    std::uint64_t step[2] = {};
    std::int32_t yCoord = 0;
    std::int32_t yBound = 0;
    bool decrement = false;
  } parts[2];
  const auto along = [&](std::int32_t y) {
    return baseCoord + static_cast<std::uint64_t>(static_cast<std::int64_t>(y - vertices[0].y) * baseStep);
  };
  const int r = rightFacing ? 1 : 0;
  {
    Tripart &part = parts[vo];
    part.yCoord = vertices[0 ^ vo].y;
    part.yBound = vertices[1 ^ vo].y;
    part.coord[r] = makePolyXFP(vertices[0 ^ vo].x);
    part.step[r] = static_cast<std::uint64_t>(boundUpper);
    part.coord[1 - r] = along(vertices[vo].y);
    part.step[1 - r] = static_cast<std::uint64_t>(baseStep);
    part.decrement = vo != 0;
  }
  {
    Tripart &part = parts[vo ^ 1];
    part.yCoord = vertices[1 ^ vp].y;
    part.yBound = vertices[2 ^ vp].y;
    part.coord[r] = makePolyXFP(vertices[1 ^ vp].x);
    part.step[r] = static_cast<std::uint64_t>(boundLower);
    part.coord[1 - r] = along(vertices[1 ^ vp].y);
    part.step[1 - r] = static_cast<std::uint64_t>(baseStep);
    part.decrement = vp != 0;
  }

  const std::int64_t half = static_cast<std::int64_t>(1024) * s;
  TexelRange uRange;
  TexelRange vRange;
  int clipX0 = 0;
  int clipX1 = 0;
  int clipY0 = 0;
  int clipY1 = 0;
  // gpu.c plots at the 11-bit row and interpolates with the unwrapped one.
  const auto span = [&](std::int32_t yi, std::int32_t y, std::int32_t xStart, std::int32_t xBound) {
    if (skipsRow(primitive.state, y / s)) {
      return;
    }
    std::int32_t x = static_cast<std::int32_t>(wrapSigned(xStart, half));
    std::int32_t w = xBound - xStart;
    const std::int32_t interpolantOffset = xStart - x;
    if (x < clipX0) {
      const std::int32_t delta = clipX0 - x;
      x += delta;
      w -= delta;
    }
    if (x + w > clipX1 + 1) {
      w = clipX1 + 1 - x;
    }
    if (w <= 0) {
      return;
    }
    emit(x, y, x + w, y + 1, static_cast<std::uint32_t>(interpolantOffset), static_cast<std::uint32_t>(yi - y));
    if (primitive.textured) {
      for (int c = 3; c < 5; c++) {
        const auto at = [&](std::int32_t px) {
          return static_cast<std::int64_t>(setup.base[c]) +
                 static_cast<std::int64_t>(static_cast<std::int32_t>(setup.dx[c])) *
                     (static_cast<std::int64_t>(px) + interpolantOffset) +
                 static_cast<std::int64_t>(static_cast<std::int32_t>(setup.dy[c])) * yi;
        };
        const std::int64_t first = at(x);
        const std::int64_t last = at(x + w - 1);
        TexelRange &range = c == 3 ? uRange : vRange;
        if ((first >> 32) != (last >> 32)) {
          range.all();
        } else {
          const int a = static_cast<int>((static_cast<std::uint64_t>(first) & 0xFFFFFFFFull) >> 24);
          const int b = static_cast<int>((static_cast<std::uint64_t>(last) & 0xFFFFFFFFull) >> 24);
          range.add(std::min(a, b), std::max(a, b));
        }
      }
    }
  };
  for (const PlaneClip &planeClip : planeClips(primitive.state)) {
    target(planeClip.plane);
    clipX0 = planeClip.clip.x0 * s;
    clipX1 = planeClip.clip.x1 * s + s - 1;
    clipY0 = planeClip.clip.y0 * s;
    clipY1 = planeClip.clip.y1 * s + s - 1;
    for (const Tripart &part : parts) {
      const auto rowAt = [&](std::int32_t yi, int side) {
        return part.coord[side] +
               static_cast<std::uint64_t>(static_cast<std::int64_t>(yi - part.yCoord)) * part.step[side];
      };
      if (part.decrement) {
        for (std::int32_t yi = part.yCoord - 1; yi >= part.yBound; yi--) {
          const auto y = static_cast<std::int32_t>(wrapSigned(yi, half));
          if (y < clipY0) {
            break;
          }
          if (y > clipY1) {
            continue;
          }
          span(yi, y, polyXInt(rowAt(yi, 0)), polyXInt(rowAt(yi, 1)));
        }
      } else {
        for (std::int32_t yi = part.yCoord; yi < part.yBound; yi++) {
          const auto y = static_cast<std::int32_t>(wrapSigned(yi, half));
          if (y > clipY1) {
            break;
          }
          if (y < clipY0) {
            continue;
          }
          span(yi, y, polyXInt(rowAt(yi, 0)), polyXInt(rowAt(yi, 1)));
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
  if (primitive.textured) {
    textureReads(primitive.state, uRange, vRange);
  }
  commit(op, primitive.semiTransparent || primitive.state.maskCheck);
}

// gpu_sprite.c DrawSprite. The op keeps the unclipped origin; u and v wrap at 256 either way.
void PlanBuilder::addSprite(const DrawPrimitive &primitive) {
  const int s = plan_.scale;
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
      range.add(std::min(first, last), std::max(first, last));
    }
  };
  for (const PlaneClip &planeClip : planeClips(primitive.state)) {
    const Clip &clip = planeClip.clip;
    const int xStart = std::max(vertex.x, clip.x0);
    const int yStart = std::max(vertex.y, clip.y0);
    const int xBound = std::min(vertex.x + primitive.width, clip.x1 + 1);
    const int yBound = std::min(vertex.y + primitive.height, clip.y1 + 1);
    if (xBound <= xStart || yBound <= yStart) {
      continue;
    }
    target(planeClip.plane);
    emit(xStart * s, yStart * s, xBound * s, yBound * s, 0);
    if (primitive.textured) {
      const auto u0 = static_cast<std::uint8_t>(u + (xStart - vertex.x) * uIncrement);
      const auto v0 = static_cast<std::uint8_t>(v + (yStart - vertex.y) * vIncrement);
      texels(uRange, u0, xBound - xStart, uIncrement);
      texels(vRange, v0, yBound - yStart, vIncrement);
    }
  }
  target(0);
  RecordOp op = primitiveOp(primitive, RecordOpKind::Sprite);
  op[kOpOriginX] = static_cast<std::uint32_t>(vertex.x);
  op[kOpOriginY] = static_cast<std::uint32_t>(vertex.y);
  op[kOpSourceX] = u;
  op[kOpSourceY] = v;
  if (primitive.textured) {
    textureReads(primitive.state, uRange, vRange);
  }
  commit(op, primitive.semiTransparent || primitive.state.maskCheck);
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

// gpu_line.c DrawLine: the pixels are walked here and drawn as single-pixel quads.
void PlanBuilder::addLine(const DrawPrimitive &primitive) {
  const int s = plan_.scale;
  present::RecordVertex p0 = primitive.vertices[0];
  present::RecordVertex p1 = primitive.vertices[1];
  const int deltaX = std::abs(p1.x - p0.x);
  const int deltaY = std::abs(p1.y - p0.y);
  if (deltaX >= 1024 || deltaY >= 512) {
    return;
  }
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
  const std::vector<PlaneClip> clips = planeClips(primitive.state);
  const bool dither = primitive.state.dither;
  for (int i = 0; i <= k; i++) {
    const int px = static_cast<int>((x >> 32) & 2047u);
    const int py = static_cast<int>((y >> 32) & 2047u);
    if (!skipsRow(primitive.state, py)) {
      const int cr = primitive.gouraud ? static_cast<std::uint8_t>(r >> 12) : p0.r;
      const int cg = primitive.gouraud ? static_cast<std::uint8_t>(g >> 12) : p0.g;
      const int cb = primitive.gouraud ? static_cast<std::uint8_t>(b >> 12) : p0.b;
      std::uint32_t pixel = 0x8000u;
      if (dither) {
        pixel |= static_cast<std::uint32_t>(ditherChannel(cr, px, py)) |
                 (static_cast<std::uint32_t>(ditherChannel(cg, px, py)) << 5) |
                 (static_cast<std::uint32_t>(ditherChannel(cb, px, py)) << 10);
      } else {
        pixel |= static_cast<std::uint32_t>(cr >> 3) | (static_cast<std::uint32_t>(cg >> 3) << 5) |
                 (static_cast<std::uint32_t>(cb >> 3) << 10);
      }
      for (const PlaneClip &planeClip : clips) {
        // A canvas reaches left of VRAM, where the 11-bit position reads as negative.
        const int x = planeClip.plane == 0 ? px : ((px ^ 1024) - 1024);
        const int y = planeClip.plane == 0 ? py : ((py ^ 1024) - 1024);
        const Clip &clip = planeClip.clip;
        if (x >= clip.x0 && x <= clip.x1 && y >= clip.y0 && y <= clip.y1) {
          target(planeClip.plane);
          emit(x * s, y * s, (x + 1) * s, (y + 1) * s, pixel);
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

void PlanBuilder::addFill(const present::VramFill &fill) {
  const std::vector<RecordRect> rects = wrappedRects(fill.x, fill.y, fill.width, fill.height);
  emitTransferRects(rects);
  // A fill across the buffer's columns clears the canvas's whole width on those rows.
  for (std::size_t i = 0; i < canvases_.size(); i++) {
    const RecordCanvas &canvas = canvases_[i];
    target(1 + static_cast<int>(i));
    for (const RecordRect &rect : rects) {
      const int y0 = std::max(rect.y0, canvas.buffer.y0);
      const int y1 = std::min(rect.y1, canvas.buffer.y1);
      if (rect.x0 <= canvas.buffer.x0 && rect.x1 >= canvas.buffer.x1 && y0 < y1) {
        emitNativeRect({canvas.originX(), y0, canvas.buffer.x0, y1});
        emitNativeRect({canvas.buffer.x1, y0, canvas.buffer.x1 + canvas.margin, y1});
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

void PlanBuilder::addCopy(const present::VramCopy &copy) {
  RecordOp op{};
  op[kOpFlags] = static_cast<std::uint32_t>(RecordOpKind::Copy) | (copy.maskCheck ? kOpMaskCheck : 0u) |
                 (copy.maskSet ? kOpMaskSet : 0u);
  const bool overlapping = ringsOverlap(copy.srcX, copy.width, copy.dstX, copy.width, kRecordVramWidth) &&
                           ringsOverlap(copy.srcY, copy.height, copy.dstY, copy.height, kRecordVramHeight);
  const auto copyPart = [&](int srcX, int srcY, int dstX, int dstY, int width, int height) {
    emitTransferRects(wrappedRects(dstX, dstY, width, height));
    for (const RecordRect &rect : wrappedRects(srcX, srcY, width, height)) {
      opReads_.push_back(rect);
    }
    op[kOpOriginX] = static_cast<std::uint32_t>(positiveMod(dstX, kRecordVramWidth));
    op[kOpOriginY] = static_cast<std::uint32_t>(positiveMod(dstY, kRecordVramHeight));
    op[kOpSourceX] = static_cast<std::uint32_t>(positiveMod(srcX, kRecordVramWidth));
    op[kOpSourceY] = static_cast<std::uint32_t>(positiveMod(srcY, kRecordVramHeight));
    commit(op, copy.maskCheck);
  };
  if (!overlapping) {
    copyPart(copy.srcX, copy.srcY, copy.dstX, copy.dstY, copy.width, copy.height);
    return;
  }
  // gpu.c copies row by row through a 128-pixel buffer; an overlapping copy sees its own writes.
  constexpr int kChunk = 128;
  for (int row = 0; row < copy.height; row++) {
    for (int column = 0; column < copy.width; column += kChunk) {
      const int width = std::min(kChunk, copy.width - column);
      copyPart(copy.srcX + column, copy.srcY + row, copy.dstX + column, copy.dstY + row, width, 1);
    }
  }
}

void PlanBuilder::addUpload(const present::VramUpload &upload) {
  emitTransferRects(wrappedRects(upload.x, upload.y, upload.width, upload.height));
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

} // namespace

RecordRect clampedVramRect(int x, int y, int width, int height) {
  const int x0 = std::clamp(x, 0, kRecordVramWidth);
  const int y0 = std::clamp(y, 0, kRecordVramHeight);
  return {x0, y0, std::clamp(x + width, x0, kRecordVramWidth), std::clamp(y + height, y0, kRecordVramHeight)};
}

bool drawAreaSpansBuffer(const present::RecordDrawState &state, const RecordRect &buffer) {
  const Clip area = nativeClip(state);
  return buffer.x0 < buffer.x1 && buffer.y0 < buffer.y1 && area.x0 == buffer.x0 && area.x1 == buffer.x1 - 1 &&
         area.y0 < buffer.y1 && area.y1 >= buffer.y0;
}

bool canvasSurvives(const RecordCanvas &canvas, int displayWidth, int margin) {
  return margin > 0 && canvas.margin == margin && canvas.buffer.x1 - canvas.buffer.x0 == displayWidth;
}

RecordRasterPlan planRecord(const FrameRecord &record, int scale, std::span<const RecordCanvas> canvases) {
  PlanBuilder builder(std::max(scale, 1), record.clutPool(), record.uploadPixels(), canvases);
  for (const present::RecordEntry &entry : record.entries()) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entry)) {
      builder.addPrimitive(*primitive);
    } else if (const auto *fill = std::get_if<present::VramFill>(&entry)) {
      builder.addFill(*fill);
    } else if (const auto *copy = std::get_if<present::VramCopy>(&entry)) {
      builder.addCopy(*copy);
    } else if (const auto *upload = std::get_if<present::VramUpload>(&entry)) {
      builder.addUpload(*upload);
    }
  }
  return builder.finish();
}

RecordRasterPlan planVramUpload(std::span<const std::uint16_t> vram, int scale) {
  PlanBuilder builder(std::max(scale, 1), {}, vram, {});
  present::VramUpload upload;
  upload.width = kRecordVramWidth;
  upload.height = kRecordVramHeight;
  builder.addUpload(upload);
  return builder.finish();
}

} // namespace psx::gpu
