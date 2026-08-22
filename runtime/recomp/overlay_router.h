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
struct RecOverlay;

struct FixedOverlayResolution {
  const RecOverlay *overlay = nullptr;
  bool addressInOverlayRange = false;
  bool ambiguous = false;
};

// Called by an overlay LOADER immediately after an image has been written to `dest`, to record which
// overlay is now resident in that slot. Order matters: the freshly-written image still matches its
// baked-in signature at this moment, before the game mutates the image's header pointer table, so the
// router can key on IDENTITY afterwards instead of re-scanning. `dest` values that are not a slot base
// (bulk data loads, say) are ignored, so it is safe to call unconditionally after any load.
void overlay_note_load(Core *c, uint32_t dest);

// ---- RELOCATABLE MODULES: the live placement registry -------------------------------------------
//
// A game that loads code modules at runtime and relocates them (Spider-Man's 30 CD.WAD modules) has
// no fixed address to route by: the game's own allocator picks each module's base at load time, it
// differs per module and per playthrough, and several modules are live at once. Such a module is
// recompiled BASE-RELATIVE against a link base, and where it actually IS lives in a per-Core
// registry (Core::ovBase / Core::ovDelta) that the game's loader keeps up to date.
//
// This supersedes signature identification for those modules, and has to: guest RAM holds the image
// relocated to the LIVE base, so it no longer matches the signature baked in at recompile time.
// Routing is by live RANGE instead, which is exact — two resident modules cannot share an address.

// The game's loader calls this the moment the module body's address is known (its allocation), and
// before the module's entry point runs. `name` is the emitted overlay name (the upper-cased file
// stem). Returns the module's overlay index, or -1 if no recompiled module has that name.
//
// FAIL-FAST: if the incoming module's live range overlaps one that is already resident, this aborts.
// That is the invariant the whole design rests on — an overlap means two modules' code occupies the
// same guest bytes, so the next call through either one runs the other's instructions. It used to be
// unavoidable (every module was pinned to one address) and merely warned about; it is now impossible
// unless the loader intercept has mis-identified an allocation, which is a defect, not a condition
// to continue through.
int overlay_place(Core *c, const char *name, uint32_t base, uint32_t size);

// The game's loader calls this when the guest frees a module body. Returns the evicted overlay index,
// or -1 if `base` is not a live module (an ordinary heap free — the common case).
int overlay_evict_at(Core *c, uint32_t base);

// Which live relocatable module owns guest address `addr`? -1 if none. This is the router's own
// lookup, exposed because a game's loader diagnostics want the same answer.
int overlay_live_index(Core *c, uint32_t addr);

// Resolve a fixed-address overlay from the image signatures currently present in guest RAM. When
// nested ranges both match, the narrowest image owns the address; equal-width matches are refused as
// ambiguous. Exposed so the routing rule can be falsified without dispatching arbitrary game code.
FixedOverlayResolution overlay_resolve_fixed(Core *c, uint32_t addr);

// For a slot-range address, the name of the overlay currently resident in that slot — "none" when the
// slot is empty or its content matches no known overlay, and null when `addr` is in no slot range.
// Diagnostic only (the dispatch-miss reporter uses it to say which overlay was routed to).
const char *overlay_router_resident_name(Core *c, uint32_t addr);
