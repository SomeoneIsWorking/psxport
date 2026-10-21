// frame_composer.h — the frame at t: a record with each produced object drawn by its render.
#pragma once

#include "frame_record.h"
#include "frame_state.h"
#include "state_producer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace psx::present {

// Composes frames into storage it keeps across presents, so a steady stream of frames allocates nothing.
class FrameComposer {
public:
  // `shown` with the entries of every object a render covers replaced by that render at `t`, placed in
  // the OT slots it emits. An object is covered when its producer has a render and `to` holds its state;
  // `from` null (a cut) renders every object at `to`. An object one of the two frames skipped, whose
  // producer names the skip as a state (StateProducer::rests), moves from or toward that state. The n-th
  // primitive in a slot goes where the object's n-th entry in it was (after its last when there are fewer),
  // else at the slot's head, else where the table's walk order puts that slot. A CLUT `shown` never sampled
  // is read from `vram`. An object whose render names a table `shown` never walked keeps its entries. Null
  // when nothing is replaced, and for an in-between that would keep any primitive as `shown` drew it: the
  // frame is `shown`. The record is valid until the next compose.
  const FrameRecord *compose(const FrameRecord &shown,
                             const FrameState *from,
                             const FrameState &to,
                             float t,
                             const StateProducers &producers,
                             std::span<const std::uint16_t> vram);

private:
  // A keyed primitive of `shown`: its object, its slot (kNoSlot when none) and its entry.
  struct Keyed {
    std::uint64_t object = 0;
    std::uint64_t slot = 0;
    std::size_t entry = 0;
  };
  // A covered object: its run in keyed_ (sorted by slot, then entry) and its first entry in `shown`.
  struct Covered {
    ObjectId id;
    const StateProducer *render = nullptr;
    std::span<const std::byte> to;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t firstEntry = 0;
  };
  struct Insert {
    std::size_t position = 0;
    std::size_t order = 0;
    DrawPrimitive primitive;
  };

  // What one object's render became: placed, kept as recorded, or a refusal of the in-between.
  enum class Placement : std::uint8_t { Drawn, Kept, Refused };

  // The object at `states` rendered into placed_, each primitive at its entry in `run`, else by slot. With
  // `refuses`, a render that cannot be drawn refuses the in-between; without, the object keeps its entries.
  Placement place(const FrameRecord &shown,
                  ObjectId id,
                  const StateProducer &render,
                  const StateInterval &states,
                  std::span<const Keyed> run,
                  std::span<const std::uint16_t> vram,
                  bool refuses);
  // resting_ sized to `bytes`, for StateProducer::rests.
  std::span<std::byte> resting(std::size_t bytes);
  // Indexes `shown`; false when an in-between must refuse it.
  bool index(const FrameRecord &shown, const FrameState &to, const StateProducers &producers, bool inBetween);
  // Where the walk puts `slot` when the object had no entry in it; false when `shown` never walked its table.
  bool slotPosition(const FrameRecord &shown, OtSlot slot, std::size_t &position) const;
  std::uint32_t *clutOffset(std::uint32_t key);
  // The output, holding `shown`'s pools.
  FrameRecord &output(const FrameRecord &shown);

  std::vector<std::uint64_t> baked_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> clutOffsets_; // sorted by CLUT key
  std::vector<Keyed> keyed_;
  std::vector<Covered> covered_;
  std::vector<std::pair<std::uint64_t, std::size_t>> heads_; // slot key, slot start index
  std::vector<std::size_t> tableStarts_;                     // slot start indices, by table, in walk order
  std::vector<std::pair<std::uint16_t, std::size_t>> tableEnds_;
  std::vector<const RecordDrawState *> environments_;
  std::vector<bool> removed_;
  std::vector<Insert> inserted_;
  std::vector<std::pair<std::size_t, DrawPrimitive>> placed_;
  std::vector<std::pair<std::uint64_t, std::size_t>> emitted_; // primitives the object emitted, by slot
  std::vector<RecordEntry> entries_;
  std::vector<std::byte> resting_;
  PrimitiveList sink_;
  FrameRecord output_;
  bool outputPooled_ = false;
};

} // namespace psx::present
