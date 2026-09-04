#ifndef PSXPORT_PAINTER_OBJECT_LAYER_H
#define PSXPORT_PAINTER_OBJECT_LAYER_H

#include <span>
#include <stddef.h>
#include <stdint.h>
#include <vector>

struct RqItem;

// Host-only identity for a set of world faces whose producer-authored order must win locally.
// Zero keeps the ordinary RenderQueue path.  The value is deliberately opaque to the framework: games
// allocate identities, while the renderer only groups equal non-zero values.
using PainterObjectId = uint32_t;
using PainterReplayDomainId = uint32_t;

// Exact position in one guest-authored painter replay. The game owns the values; the framework owns
// only the ordering rule. A domain is one independently replayable command stream (normally one OT).
// Descending OT bins are visited first, head-inserted links are visited newest first, and commands
// inside one linked chain retain forward packet order.
struct PainterReplayKey {
  uint16_t ot_bin = 0;
  uint32_t link_ordinal = 0;
  uint32_t chain_suborder = 0;
};

struct PainterReplayOrder {
  PainterReplayDomainId domain = 0;
  PainterReplayKey key{};

  bool authored() const {
    return domain != 0;
  }
};

// One authoritative comparator for producer validation, planning, and focused tests.
bool painterReplayBefore(const PainterReplayOrder &left, const PainterReplayOrder &right);

enum PainterObjectFlags : uint8_t {
  PAINTER_OBJECT_NONE = 0,
  // Compatibility scope flag: emitOrQueue/push maps it into each RqItem::dither. New producers should
  // pass the per-item DTD value explicitly because a single object may contain both draw states.
  PAINTER_OBJECT_DITHER = 1u << 0,
};

enum class PainterObjectRefusal : uint8_t {
  None = 0,
  Empty,
  NonWorld,
  NonDepth,
  TooManyObjects,
  TooManyFaces,
  ActiveScope,
  InvalidObjectId,
  UnsortedQueue,
  UnsupportedMaterial,
  MixedReplayPolicy,
  UnorderedWorldMix,
  DuplicateReplayKey,
  ObjectInMultipleDomains,
  MixedFlushEpoch,
};

struct PainterObjectLimits {
  size_t max_objects = 256;
  size_t max_faces = 16384;
};

struct PainterObjectStats {
  size_t items_scanned = 0;
  size_t grouped_faces = 0;
  size_t objects = 0;
  size_t authored_domains = 0;
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
  bool semi_transparent = false;
  uint8_t blend_mode = 0;
  PainterReplayOrder replay{};
};

enum class PainterObjectAdmissionRefusal : uint8_t {
  None = 0,
  Empty,
  InvalidObjectId,
  ActiveScope,
  InvalidLifecycle,
  DuplicateObject,
  TooManyObjects,
  TooManyFaces,
  QueueCapacity,
  InvalidExistingFace,
  MixedReplayPolicy,
  UnorderedWorldMix,
};

struct PainterObjectAdmission {
  PainterObjectAdmissionRefusal refusal = PainterObjectAdmissionRefusal::None;
  size_t queued_items = 0;
  size_t existing_objects = 0;
  size_t existing_faces = 0;
  size_t refusal_item = SIZE_MAX;
  bool accepted() const {
    return refusal == PainterObjectAdmissionRefusal::None;
  }
};

// One producer's declaration for a painter object that has not been queued yet. A batch may contain
// several distinct objects; all entries are checked together before the producer starts pushing.
struct PainterObjectBatchEntry {
  PainterObjectId object = 0;
  size_t new_faces = 0;
  PainterReplayDomainId replay_domain = 0;
};

enum class PainterPlaybackKind : uint8_t { IsolatedObject, AuthoredDomain };

struct PainterPlaybackRange {
  PainterPlaybackKind kind = PainterPlaybackKind::IsolatedObject;
  uint32_t identity = 0;
  size_t first_command = 0;
  size_t command_count = 0;
};

// Phase-0 output. ordinary_items retains every ungrouped queue index in input order. Commands are
// contiguous per object and strictly increasing by RqItem::seq, without material bucketing.
struct PainterObjectPlan {
  PainterObjectStats stats;
  std::vector<size_t> ordinary_items;
  // Non-face primitives cannot participate in a painter replay. A trailing world line can still be
  // preserved after the replayed faces, which is the ordering needed by line-producing native effects.
  std::vector<size_t> ordinary_items_after_ranges;
  std::vector<PainterCommand> commands;
  std::vector<PainterPlaybackRange> ranges;
  // Dense rank of each input item's original sequence inside this physical flush. Captured frames
  // rebase RqItem::seq across flushes; the depth tie channel needs ordering, not that global offset.
  std::vector<uint32_t> presentation_ranks;
  bool accepted() const {
    return stats.refusal == PainterObjectRefusal::None;
  }
};

// Plans an already-finished presentation stream. The stream may point into more than one captured
// queue (the fps60 present merge does exactly that), but every item must already be sorted by
// (layer, seq). This is the one planner used by both direct queue emission and presentation-time replay.
PainterObjectPlan planPainterItemStream(std::span<const RqItem *const> items, PainterObjectLimits limits = {});

#endif
