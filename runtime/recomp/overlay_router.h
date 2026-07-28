// Public entry points of the cross-module dispatch router (overlay_router.cpp).
//
// These were previously reachable only through function-local `void overlay_note_load(Core*,uint32_t);`
// forward declarations repeated at each call site (twice in cd_override.cpp), which meant a CONSUMER
// wanting to note its own overlay loads had to copy the signature by hand — with no compiler check
// that the copy still matched. A game whose overlays load through its own native loader (Spyro loads
// them out of WAD.WAD, not off the disc's file tree) has exactly that need.
#pragma once
#include <stdint.h>

struct Core;

// Called by an overlay LOADER immediately after an image has been written to `dest`, to record which
// overlay is now resident in that slot. Order matters: the freshly-written image still matches its
// baked-in signature at this moment, before the game mutates the image's header pointer table, so the
// router can key on IDENTITY afterwards instead of re-scanning. `dest` values that are not a slot base
// (bulk data loads, say) are ignored, so it is safe to call unconditionally after any load.
void overlay_note_load(Core* c, uint32_t dest);

// For a slot-range address, the name of the overlay currently resident in that slot — "none" when the
// slot is empty or its content matches no known overlay, and null when `addr` is in no slot range.
// Diagnostic only (the dispatch-miss reporter uses it to say which overlay was routed to).
const char* overlay_router_resident_name(Core* c, uint32_t addr);
