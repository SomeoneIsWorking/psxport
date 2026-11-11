// record_cluts.h — the CLUTs a record sampled, so a primitive drawn into a copy of it samples the same entries.
#pragma once

#include "frame_record.h"
#include "palette_cycle.h"

#include <cstdint>
#include <span>
#include <vector>

namespace psx::present {

class RecordCluts {
public:
  // An in-between `t` of the way from `earlier` to the shown record, with the palette cycles of its sample.
  struct Between {
    const FrameRecord &earlier;
    float t;
    const CycledRows &cycles;
  };
  // The pool offsets `shown` sampled, by CLUT attribute and texture depth; the lowest offset for each.
  void index(const FrameRecord &shown);
  // As index(shown), and a cycled CLUT row both records sampled as different frames of its cycle resolves to
  // their blend at `between.t`.
  void index(const FrameRecord &shown, Between between);
  // Sets `primitive`'s CLUT offset in `into`, which holds the indexed record's pools: the entries the record
  // sampled (blended with the earlier record's at a cycle step), else the ones `vram` holds now,
  // appended. False when the pool is full.
  [[nodiscard]] bool resolve(FrameRecord &into, DrawPrimitive &primitive, std::span<const std::uint16_t> vram);

private:
  struct Indexed {
    std::uint32_t key;
    std::uint32_t offset;
    bool settled; // offset is final in the record being assembled
  };
  static const Indexed *find(std::span<const Indexed> offsets, std::uint32_t key);
  static void collect(const FrameRecord &record, std::vector<Indexed> &out);

  std::vector<Indexed> offsets_;        // sorted by CLUT key
  std::vector<Indexed> earlierOffsets_; // sorted by CLUT key
  const FrameRecord *earlier_ = nullptr;
  const CycledRows *cycles_ = nullptr;
  float t_ = 1.0f;
};

} // namespace psx::present
