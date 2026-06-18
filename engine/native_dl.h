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
// NATIVE ORDERING (later-138): the host engine writes NOTHING to the guest OT/packet pool. An owned
// prim is appended to a HOST per-bucket display list keyed by its OT bucket-anchor address (otaddr =
// ot_base + idx*4, the slot the guest AddPrim would have prepended into). The DrawOTag walk renders
// each bucket's host prims when it visits that anchor node (gpu_native.c). The guest still owns the OT
// ARRAY (ClearOTagR builds the anchor chain) and its OWN inline 2D prims (its AddPrim writes those into
// the pool) — we read that chain but never write it, and never touch the pool ptr 0x800BF544. This
// removes the gameplay-diverging guest writes the old "1-word link node + advance pool" scheme made.
//
// Ordering is byte-identical to the old guest-OT merge: (1) ACROSS buckets the walk order (back→front)
// is unchanged; (2) WITHIN a bucket the guest AddPrim prepends (LIFO, most-recent renders first), so we
// prepend to the bucket list and render head-first — same order. (3) 3D-vs-2D interleave: measured
// MIXED=0 (PSXPORT_DEBUG=bucket) — a bucket holds EITHER host-3D OR guest-2D, never both — so rendering
// host prims at the anchor and guest prims at their own pool nodes needs no intra-bucket merge.
#ifndef NATIVE_DL_H
#define NATIVE_DL_H
#include <stdint.h>

typedef struct {
  uint32_t node;       // OT bucket-anchor phys addr (otaddr & 0x1FFFFC) — the DrawOTag-walk lookup key
  int32_t  bnext;      // next prim in THIS bucket (LIFO chain), -1 = end of bucket
  uint8_t  nwords;     // GP0 payload word count (9 = POLY_GT3, 12 = POLY_GT4)
  uint8_t  npz;        // valid per-vertex view-Z entries (3 = tri, 4 = quad)
  uint32_t words[16];  // the GP0 payload words — byte-identical to the guest packet that would've been
  float    pz[4];      // per-vertex native view-space Z (parse order V0,V1,V2[,V3]); renderer depth
} NativePrim;

// Native display list active (the port default). PSXPORT_DL_GUESTPKT=1 reverts the owned submit fns to
// writing the full guest packet into the guest OT (the pre-port behaviour) for A/B verification.
int         ndl_active(void);
// Append a prim to the host display list for OT bucket-anchor `otaddr` (phys). Prepends to that
// bucket's LIFO chain. Resets the per-frame list if it was just consumed by a DrawOTag pass (lazy
// reset — robust to the engine's setup-OT + main-OT double draw). NULL on arena overflow.
NativePrim* ndl_alloc(uint32_t otaddr);
// Head prim of the host list for bucket-anchor `addr` (phys), or NULL if no host prims target it.
NativePrim* ndl_lookup(uint32_t addr);
// Next prim in `p`'s bucket chain, or NULL at the end.
NativePrim* ndl_next(const NativePrim* p);
// Mark the list consumed after a DrawOTag walk completes; the next ndl_alloc starts a fresh frame.
void        ndl_mark_consumed(void);

#endif
