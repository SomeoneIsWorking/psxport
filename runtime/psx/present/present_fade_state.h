// present_fade_state.h — the fade a present composites, for one Core.
//
// The seam between a `Core` (which owns the title hook table and the PresentFade endpoints) and
// psxport::fade (which owns the resolution arithmetic and stays free of both). Renderer call sites
// ask here rather than reading the title's CURRENT fade level directly: doing that on both presents
// of a logic frame is what put the in-between present a whole frame early, which during a fade is
// every pixel on screen (Tomba! 2 issue 0021).
#pragma once

class Core;
struct FadeState;

// The fade to composite on the present now in flight. With no temporal presentation driving the
// endpoints this is the title's live state, unchanged, so a product without interpolation is
// unaffected. A core or game that is not stood up yet also reads live.
//
// NOT for a path that must reproduce a REAL frame's guest-visible state — the SBS readback compares
// against the guest and deliberately keeps the live read.
FadeState present_fade_state(Core *core);
