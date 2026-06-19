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
#include <string.h>

struct Core;   // CPU/RAM handle (core.h); the public ndl_* API takes Core* to reach core->game->ndl

typedef struct {
  uint32_t node;       // OT bucket-anchor phys addr (otaddr & 0x1FFFFC) — the DrawOTag-walk lookup key
  int32_t  bnext;      // next prim in THIS bucket (LIFO chain), -1 = end of bucket
  uint8_t  nwords;     // GP0 payload word count (9 = POLY_GT3, 12 = POLY_GT4)
  uint8_t  npz;        // valid per-vertex view-Z entries (3 = tri, 4 = quad)
  uint32_t words[16];  // the GP0 payload words — byte-identical to the guest packet that would've been
  float    pz[4];      // per-vertex native view-space Z (parse order V0,V1,V2[,V3]); renderer depth
} NativePrim;

// Field main OT is ~514 polys + overlay emitters; size generously so the arena never drops geometry.
#define NDL_MAX   32768
#define NDL_HASH  65536           // power-of-two open-addressing table (load factor < 0.5)
#define NDL_MASK  (NDL_HASH - 1)

// NdlState — the native classified display list's per-instance, per-frame arena + node-keyed lookup.
// De-globalization (2026-06-19): this was file-scope state in native_dl.cpp; it now lives on Game
// (game.h has `NdlState ndl;`), reached via core->game->ndl, so two cores build independent lists. The
// arena is HOST render data (never written to guest RAM), so it does not affect a Core::ram lockstep
// diff — migrated for the literal "nothing file-scope global" goal + future per-core render diffing.
// Method bodies keep the historical s_-names (member access via this), unchanged by the move.
struct NdlState {
  NativePrim s_prim[NDL_MAX] = {};
  uint32_t   s_banchor[NDL_HASH] = {};  // bucket-anchor otaddr (phys), 0 = empty slot
  int32_t    s_bhead[NDL_HASH] = {};    // head prim index for that anchor (LIFO chain via NativePrim.bnext)
  int        s_n = 0;
  int        s_consumed = 1;            // start consumed so the first alloc begins a clean frame
  void        reset();
  uint32_t    slot(uint32_t otaddr);
  NativePrim* alloc(uint32_t otaddr);
  NativePrim* lookup(uint32_t addr);
  NativePrim* next(const NativePrim* p);
  void        mark_consumed();
};

// Native display list active (the port default). PSXPORT_DL_GUESTPKT=1 reverts the owned submit fns to
// writing the full guest packet into the guest OT (the pre-port behaviour) for A/B verification.
int         ndl_active(void);
// Append a prim to the host display list for OT bucket-anchor `otaddr` (phys). Prepends to that
// bucket's LIFO chain. Resets the per-frame list if it was just consumed by a DrawOTag pass (lazy
// reset — robust to the engine's setup-OT + main-OT double draw). NULL on arena overflow.
NativePrim* ndl_alloc(Core* core, uint32_t otaddr);
// Head prim of the host list for bucket-anchor `addr` (phys), or NULL if no host prims target it.
NativePrim* ndl_lookup(Core* core, uint32_t addr);
// Next prim in `p`'s bucket chain, or NULL at the end.
NativePrim* ndl_next(Core* core, const NativePrim* p);
// Mark the list consumed after a DrawOTag walk completes; the next ndl_alloc starts a fresh frame.
void        ndl_mark_consumed(Core* core);

#endif
