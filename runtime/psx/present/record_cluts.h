// record_cluts.h — the CLUTs a record sampled, so a primitive drawn into a copy of it samples the same entries.
#pragma once

#include "frame_record.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace psx::present {

class RecordCluts {
public:
  // The pool offsets `shown` sampled, by CLUT attribute and texture depth; the lowest offset for each.
  void index(const FrameRecord &shown);
  // Sets `primitive`'s CLUT offset in `into`, which holds the indexed record's pools: the entries the record
  // sampled, else the ones `vram` holds now, appended. False when the pool is full.
  [[nodiscard]] bool resolve(FrameRecord &into, DrawPrimitive &primitive, std::span<const std::uint16_t> vram);

private:
  std::vector<std::pair<std::uint32_t, std::uint32_t>> offsets_; // sorted by CLUT key
};

} // namespace psx::present
