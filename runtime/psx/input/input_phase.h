// input_phase.h — the title-defined INPUT PHASE that pad recordings are keyed on.
//
// A phase is the title's answer to "which screen or game state is taking input right now": a
// gamestate, a level, a menu sub-state. It is opaque to the framework, which only compares phases
// for equality and records when they change. A recording keyed on phases stores each frame's pad
// mask as an offset from the moment its phase was entered, so a load or boot sequence that takes a
// different number of frames than when it was recorded moves the phase boundary, not the presses
// inside it. See pad_phase_replay.h for the matching rules and pad_recording.h for the file format.
//
// A title declares its phase through GameRuntime::inputPhase. A title that declares none answers
// kUnkeyedPhase, and its recordings are then ABSOLUTE from boot, which is exactly the pre-phase
// behaviour and is written into the file as such rather than inferred on replay.
#pragma once

#include <cstdint>

namespace psx::input {

// Reserved: "no phase". A segment carrying it matches whatever phase the game is in, so a recording
// made entirely under it replays frame-for-frame from boot. A title's own keys must never use it.
inline constexpr std::uint64_t kUnkeyedPhase = 0xFFFFFFFFFFFFFFFFull;

} // namespace psx::input
