// frame_composer.h — the frame at t: a record with each produced object drawn by its render.
#pragma once

#include "frame_record.h"
#include "frame_state.h"
#include "state_producer.h"

#include <optional>

namespace psx::present {

// `shown` with the entries of every object a render covers replaced by that render at `t`, placed in
// the OT slots it emits. An object is covered when its producer has a render and `to` holds its state;
// `from` null (a cut) renders every object at `to`. The n-th primitive in a slot goes where
// the object's n-th entry in it was (after its last when there are fewer), else at the slot's head, else where the
// table's walk order puts that slot. An object whose render names a table `shown` never walked, or a CLUT `shown` never
// sampled, keeps its entries. Nullopt when nothing is replaced: the frame is `shown`.
std::optional<FrameRecord> composeFrame(
    const FrameRecord &shown, const FrameState *from, const FrameState &to, float t, const StateProducers &producers);

} // namespace psx::present
