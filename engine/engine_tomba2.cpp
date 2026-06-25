// Tomba2Engine — the PC-native engine layer (Phase 1: the entity-list walk).
//
// Reimplements Tomba!2's per-frame object driver FUN_8007a904 in native C. The engine OWNS the
// walk; the per-object handlers (gameplay/render logic) STAY as recompiled PSX code, invoked via
// rec_dispatch on the same guest entity structs. This is the seam the user wants: native engine,
// PSX gameplay in PSX guest memory. RE: docs/engine_re.md "entity-list walk".
//
// Faithful to the recomp body (the oracle): two doubly-linked lists (heads T2_OBJLIST_HEAD_1/2),
// each walked via next@+0x24; per node, clear render_flag@+1 then call handler@+0x1c(node) with the
// node in a0. `next` is read BEFORE the handler runs (a handler may unlink/free its own node) and is
// held in a host local, so it survives the handler clobbering guest registers.
//
// The native walk IS the engine now — registered unconditionally (no gating). Was verified native==recomp:
// VRAM bit-identical at frames 4000 and 4720 of real gameplay (1 MB cmp PASS each) — see docs/journal.md.
#include "core.h"
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include "tomba2_types.h"
#include "margin_render.hpp"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern void     rec_dispatch(Core* c, uint32_t addr);   // run a function by address (override or interp)

static int  s_dbg   = -1;
static long s_walks = 0;

// Call one node's handler exactly as the recomp does: a0 = node, jalr *(node+0x1c). Tag g_current_object
// = node around the WHOLE handler so fps60 attributes ALL geometry it submits to this entity (the
// interpolation identity), independent of whether projection happens inside the cull subtree.
// Owned per-object BEHAVIOR handlers (node+0x1C). Each was RE'd 1:1 from its recomp body and carries its
// own full RAM+scratchpad A/B verify gate (sm40558 / obj739acverify / obj741dcverify / obj73cd8verify) —
// they were written but ORPHANED (call_handler always rec_dispatched the raw address). Routing them here
// makes the native bodies LIVE: native-only when the gate channel is off, A/B-vs-recomp when it is on.
// This is the start of owning the entity-behavior layer (the object SYSTEM's logic), top-down by hotness.
void ov_sm40558(Core* c);          // 0x80040558 (entity.h)
void ov_beh_739ac_run(Core* c);    // 0x800739AC (objbeh_739ac.cpp)
void ov_beh_73cd8_run(Core* c);    // 0x80073CD8 (objbeh_73cd8.cpp)
void ov_beh_741dc_run(Core* c);    // 0x800741DC (objbeh_741dc.cpp)
void ov_beh_8012eb54_run(Core* c); // 0x8012EB54 (objbeh_8012eb54.cpp — overlay)
void ov_beh_80124e74_run(Core* c); // 0x80124E74 (objbeh_80124e74.cpp — overlay)
void ov_beh_80133c14_run(Core* c); // 0x80133C14 (objbeh_80133c14.cpp — overlay)
void ov_beh_80138fc8_run(Core* c); // 0x80138FC8 (objbeh_80138fc8.cpp — overlay)
void ov_beh_8013259c_run(Core* c); // 0x8013259C (objbeh_8013259c.cpp — overlay)
void ov_beh_80145230_run(Core* c); // 0x80145230 (objbeh_80145230.cpp — overlay)
void ov_beh_8012d4ec_run(Core* c); // 0x8012D4EC (objbeh_8012d4ec.cpp — overlay)
void ov_beh_8012d404_run(Core* c); // 0x8012D404 (objbeh_8012d404.cpp — overlay)
void ov_beh_801395c0_run(Core* c); // 0x801395C0 (objbeh_801395c0.cpp — overlay)
void ov_beh_8004c238_run(Core* c); // 0x8004C238 (objbeh_8004c238.cpp — resident)
void ov_beh_8004ce14_run(Core* c); // 0x8004CE14 (objbeh_8004ce14.cpp — resident)
void ov_beh_80071a3c_run(Core* c); // 0x80071A3C (objbeh_80071a3c.cpp — resident)
void ov_beh_8006f2d0_run(Core* c); // 0x8006F2D0 (objbeh_8006f2d0.cpp — resident)
void ov_beh_8013c538_run(Core* c); // 0x8013C538 (objbeh_8013c538.cpp — overlay)
void ov_beh_8013c3f4_run(Core* c); // 0x8013C3F4 (objbeh_8013c3f4.cpp — overlay)
void ov_beh_8013c9c0_run(Core* c); // 0x8013C9C0 (objbeh_8013c9c0.cpp — overlay)
void ov_beh_80136d9c_run(Core* c); // 0x80136D9C (objbeh_80136d9c.cpp — overlay)
void ov_beh_80129c00_run(Core* c); // 0x80129C00 (objbeh_80129c00.cpp — overlay)
static bool dispatch_native_behavior(Core* c, uint32_t h) {
  switch (h) {
    case 0x80040558u: ov_sm40558(c);          return true;
    case 0x8004CE14u: ov_beh_8004ce14_run(c); return true;
    case 0x8006F2D0u: ov_beh_8006f2d0_run(c); return true;
    case 0x80071A3Cu: ov_beh_80071a3c_run(c); return true;
    case 0x800739ACu: ov_beh_739ac_run(c);    return true;
    case 0x80073CD8u: ov_beh_73cd8_run(c);    return true;
    case 0x800741DCu: ov_beh_741dc_run(c);    return true;
    case 0x8012EB54u: ov_beh_8012eb54_run(c); return true;
    case 0x80124E74u: ov_beh_80124e74_run(c); return true;
    case 0x80133C14u: ov_beh_80133c14_run(c); return true;
    case 0x80138FC8u: ov_beh_80138fc8_run(c); return true;
    case 0x8013259Cu: ov_beh_8013259c_run(c); return true;
    case 0x80145230u: ov_beh_80145230_run(c); return true;
    case 0x8012D4ECu: ov_beh_8012d4ec_run(c); return true;
    case 0x8012D404u: ov_beh_8012d404_run(c); return true;
    case 0x801395C0u: ov_beh_801395c0_run(c); return true;
    case 0x8013C538u: ov_beh_8013c538_run(c); return true;
    case 0x8013C3F4u: ov_beh_8013c3f4_run(c); return true;
    case 0x8013C9C0u: ov_beh_8013c9c0_run(c); return true;
    case 0x80136D9Cu: ov_beh_80136d9c_run(c); return true;
    case 0x80129C00u: ov_beh_80129c00_run(c); return true;
    // 0x8004C238: native body written (objbeh_8004c238.cpp) but A/B gate shows 40 MISMATCH (later-232c) —
    // NOT wired until fixed; runs as PSX (rec_dispatch).
    default: return false;
  }
}

static inline void call_handler(Core* c, uint32_t node) {
  uint32_t prev = c->game->fps60.current_object;
  c->game->fps60.current_object = node;
  c->r[4] = node;                                  // $a0
  uint32_t h = c->mem_r32(node + T2OBJ_HANDLER);
  // DIAG `behhist`: tally distinct per-object behavior handlers (node+0x1C) so we know the concrete
  // entity-behavior set to own natively (the bulk of the still-PSX object SYSTEM logic). (later-230)
  if (cfg_dbg("behhist")) {
    static uint32_t addr[64]; static long cnt[64]; static int nh=0; static long w=0;
    int i=0; for(; i<nh; i++) if(addr[i]==h) break;
    if(i==nh && nh<64){ addr[nh]=h; cnt[nh]=0; nh++; }
    if(i<64) cnt[i]++;
    if((++w % 300)==0){ fprintf(stderr,"[behhist] distinct=%d handlers:\n", nh);
      for(int j=0;j<nh;j++) fprintf(stderr,"   %08X  x%ld\n", addr[j], cnt[j]); }
  }
  if (!dispatch_native_behavior(c, h)) rec_dispatch(c, h);   // owned behavior → native; else PSX leaf
  c->game->fps60.current_object = prev;
}

static void walk_list(Core* c, uint32_t head, long* count) {
  for (uint32_t n = head; n; ) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);       // read next BEFORE the handler may unlink the node
    c->mem_w8(n + T2OBJ_RENDER_FLAG, 0);              // engine clears the per-frame render flag
    call_handler(c, n);
    n = next; (*count)++;
  }
}

// Native FUN_8007a904. Second list head is re-read fresh after list 1 (handlers in list 1 may mutate
// it) — matches the recomp body, which reloads DAT_800f2624 across the first loop.
// Non-static: called DIRECTLY from the native field per-frame update (engine_stage.cpp ov_field_frame).
void ov_objwalk(Core* c) {
  long nodes = 0;
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_1), &nodes);
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_2), &nodes);

  // Native widescreen margin (later-133): render the objects the wide frustum re-included (collected by
  // ov_object_cull during the walk) via per-node flushes here — after all handlers ran, before the OT is
  // submitted. No +1 poke -> the handlers stayed in their culled branch -> gameplay 0-diff.
  margin_render_flush(c);

  if (s_dbg < 0) s_dbg = cfg_dbg("engine") ? 1 : 0;
  if (s_dbg && (s_walks % 300) == 0)
    fprintf(stderr, "[engine] objwalk #%ld: %ld nodes\n", s_walks, nodes);
  s_walks++;
}

void engine_tomba2_init(void) {
  if (cfg_dbg("engine"))
    fprintf(stderr, "[engine] native object-list walk active (FUN_8007a904)\n");
}
