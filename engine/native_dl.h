// Native classified display list (Tomba2Engine — engine_re.md "Native ownership plan" step 1).
//
// The engine's per-entity render handlers and the geometry submit library build GPU primitives into
// the ordering table (OT) in GUEST RAM, and DrawOTag walks that OT decoding each guest packet. That is
// the "render data living in guest memory" the port is removing: a primitive's geometry/colour/uv is
// game-engine output, not gameplay state, and belongs in native PC structs we own.
//
// For the primitives whose submit path we OWN natively (engine_submit.c — POLY_GT3/GT4 + the byte-
// packed GT4 field emitter), the submit no longer writes the packet bytes into guest RAM. Instead it
// fills a NativePrim (the would-be packet words + each vertex's real view-space Z, which the integer
// guest packet always threw away) and links a ZERO-LENGTH ordering node into the guest OT purely to
// preserve draw order. DrawOTag's walk (gpu_native.c) renders an owned node straight from this native
// arena — carrying the native view-Z into the renderer's depth path, so the value-keyed "attach"
// address bridge is gone (engine_re.md later-122).
//
// The OT *ordering* (the 1-word link node) still lives in guest RAM because the game's own inline
// AddPrim handlers build the same OT; the render DATA (verts/colour/uv/depth) does not.
#ifndef NATIVE_DL_H
#define NATIVE_DL_H
#include <stdint.h>

typedef struct {
  uint32_t node;       // guest OT node phys addr (madr & 0x1FFFFC) — the DrawOTag-walk lookup key
  uint8_t  nwords;     // GP0 payload word count (9 = POLY_GT3, 12 = POLY_GT4)
  uint8_t  npz;        // valid per-vertex view-Z entries (3 = tri, 4 = quad)
  uint32_t words[16];  // the GP0 payload words — byte-identical to the guest packet that would've been
  float    pz[4];      // per-vertex native view-space Z (parse order V0,V1,V2[,V3]); renderer depth
} NativePrim;

// Native display list active (the port default). PSXPORT_DL_GUESTPKT=1 reverts the owned submit fns to
// writing the full guest packet (the pre-port behaviour) for A/B verification.
int         ndl_active(void);
// Begin a prim for guest OT `node`. Resets the per-frame list if it was just consumed by a DrawOTag
// pass (lazy reset — robust to the engine's setup-OT + main-OT double draw). NULL on arena overflow.
NativePrim* ndl_alloc(uint32_t node);
// The owned prim linked at `node` (phys addr), or NULL if this node is an ordinary guest packet.
NativePrim* ndl_lookup(uint32_t node);
// Mark the list consumed after a DrawOTag walk completes; the next ndl_alloc starts a fresh frame.
void        ndl_mark_consumed(void);

#endif
