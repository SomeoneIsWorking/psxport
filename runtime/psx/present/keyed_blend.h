// keyed_blend.h — the one interpolation rule: frame N with its keyed vertices moved toward N-1.
#pragma once

#include "frame_record.h"

namespace psx::present {

// The record an in-between at `t` (0 = previous, 1 = current) rasterizes. Every entry of `current` in
// order; a keyed primitive whose key `previous` also holds, with the same kind, vertex count, texture
// page, texture mode and blend mode, has its x/y blended at t; everything else is `current` verbatim.
// The nth primitive carrying a key pairs with the nth in `previous` carrying it.
FrameRecord keyedBlend(const FrameRecord &previous, const FrameRecord &current, float t);

} // namespace psx::present
