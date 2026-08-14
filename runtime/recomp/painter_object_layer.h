#ifndef PSXPORT_PAINTER_OBJECT_LAYER_H
#define PSXPORT_PAINTER_OBJECT_LAYER_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

// Host-only identity for a set of opaque world faces whose producer-authored order must win locally.
// Zero keeps the ordinary RenderQueue path.  The value is deliberately opaque to the framework: games
// allocate identities, while the renderer only groups equal non-zero values.
using PainterObjectId = uint32_t;

enum PainterObjectFlags : uint8_t {
  PAINTER_OBJECT_NONE   = 0,
  // Compatibility scope flag: emitOrQueue/push maps it into each RqItem::dither. New producers should
  // pass the per-item DTD value explicitly because a single object may contain both draw states.
  PAINTER_OBJECT_DITHER = 1u << 0,
};

enum class PainterObjectRefusal : uint8_t {
  None = 0,
  Empty,
  SemiTransparent,
  NonWorld,
  NonDepth,
  TooManyObjects,
  TooManyFaces,
  ActiveScope,
  InvalidObjectId,
  UnsortedQueue,
  UnsupportedMaterial,
};

struct PainterObjectLimits {
  size_t max_objects = 256;
  size_t max_faces = 16384;
};

struct PainterObjectStats {
  size_t items_scanned = 0;
  size_t grouped_faces = 0;
  size_t objects = 0;
  PainterObjectRefusal refusal = PainterObjectRefusal::None;
  size_t refusal_item = SIZE_MAX;
  size_t partitioned_items = 0;
};

enum class PainterMaterial : uint8_t { Untextured, Textured };

struct PainterCommand {
  size_t item_index = 0;
  PainterObjectId object = 0;
  uint32_t seq = 0;
  PainterMaterial material = PainterMaterial::Untextured;
  bool shade_gouraud = false;
  bool dither = false;
};

struct PainterObjectRange {
  PainterObjectId object = 0;
  size_t first_command = 0;
  size_t command_count = 0;
};

// Phase-0 output. ordinary_items retains every ungrouped queue index in input order. Commands are
// contiguous per object and strictly increasing by RqItem::seq, without material bucketing.
struct PainterObjectPlan {
  PainterObjectStats stats;
  std::vector<size_t> ordinary_items;
  std::vector<PainterCommand> commands;
  std::vector<PainterObjectRange> objects;
  bool accepted() const { return stats.refusal == PainterObjectRefusal::None; }
};

#endif
