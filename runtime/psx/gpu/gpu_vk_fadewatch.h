// gpu_vk_fadewatch — the `debug fadewatch` guest-state tap taken during the 2026-07-01 "garbage
// during fade" investigation, extracted from gpu_vk_present on 2026-09-19.
//
// It fires only when the fade state or the presented rect CHANGES, so it reports transitions rather
// than one line per present, and it is silent unless the `fadewatch` channel is on.
//
// KNOWN BOUNDARY DEFECT, recorded here rather than tombstoned in place: the addresses it reads
// (0x80100400's bg_scene_transition_sm struct, and ov_sop_field_mode's sm+0x50/0x6c reached through
// 0x1f800138) are TOMBA! 2 addresses, and this is the game-agnostic framework. It reads those
// addresses on every title that turns the channel on. Its proper home is a Tomba! 2 diagnostic
// behind a title hook; it was extracted rather than moved because the investigation that wants it
// may still be open, and it was extracted rather than left because gpu_vk.cpp is a critical legacy
// monolith at its shrink-only cap.
#pragma once

class Core;
struct FadeState;

// `sx/sy/w/h` are the rect just presented; a change in them counts as a transition just as a change
// in the fade state does.
void gpu_vk_fadewatch_tap(Core *core, int sx, int sy, int w, int h);

// The other half of the same diagnostic: the fade actually COMPOSITED on this present, with the rect
// it was composited into. Separate from the tap above because it reports the resolved present-time
// value (present_fade_state.h) rather than the title's live guest state, and the two answers differ
// on an in-between present — which is the whole point of looking. Same transition-only discipline.
void gpu_vk_fadewatch_present(const FadeState &fade, int sx, int sy, int w, int h);
