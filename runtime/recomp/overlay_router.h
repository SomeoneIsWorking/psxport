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

// Record EXACTLY which overlay is resident, by name, bypassing signature identification entirely.
//
// Why this exists: signature routing assumes each overlay's first bytes are distinctive, which holds
// when overlays load to DIFFERENT bases (their relocated words differ). It fails when a port pins
// many relocatable modules to ONE shared base — Spider-Man's 30 CD.WAD modules collapse to 14
// distinct 32-byte signatures there, because the module entry prologues are identical boilerplate and
// relocating them all to the same address makes the words identical too. 12 of them share a single
// signature.
//
// A loader that KNOWS which module it just placed should say so rather than let the router guess.
// Returns 0 and logs if the name matches no emitted overlay — a silent miss here would route a call
// into a sibling module's switch and surface far away as a dispatch miss.
int overlay_set_resident(Core* c, const char* name);

// For a slot-range address, the name of the overlay currently resident in that slot — "none" when the
// slot is empty or its content matches no known overlay, and null when `addr` is in no slot range.
// Diagnostic only (the dispatch-miss reporter uses it to say which overlay was routed to).
const char* overlay_router_resident_name(Core* c, uint32_t addr);
