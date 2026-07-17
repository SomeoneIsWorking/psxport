// ObjectList::walkAll / walkAux — see object_list.h.
//
// Faithful ports of guest FUN_8007A904 / FUN_80069B28. Both use the shared `dispatch_obj_method`
// helper (game/object/engine_tomba2.cpp), the same native-or-substrate handler dispatch used by
// TransitionState3::walkOnce. The `behhist` diagnostic (from the pre-restructure call_handler)
// is preserved in the main walk since it feeds top-down ownership decisions.
#include "object_list.h"
#include "game_ctx.h"
#include "core.h"
#include "cfg.h"
#include "game.h"                 // Fps60::current_object
#include "tomba2_types.h"         // T2_OBJLIST_HEAD_1/2, T2OBJ_HANDLER/NEXT/RENDER_FLAG
#include "render.h"               // rend(c)->margin.flush (native widescreen margin pass)
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core* c, uint32_t addr);

namespace {

// Per-node handler dispatch: fps60 current-object bookkeeping + native-or-substrate route via
// BehaviorDispatch (was static call_handler + walk_list helpers). Local to this TU. The `behhist`
// diagnostic feeds top-down ownership decisions, so it lives here on the main walk path.
inline void call_handler(Core* c, uint32_t node) {
  uint32_t h = c->mem_r32(node + T2OBJ_HANDLER);
  if (cfg_dbg("behhist")) {
    ObjectList& ol = eng(c).objectList;
    uint32_t* addr = ol.mBehAddr; long* cnt = ol.mBehCnt; int& nh = ol.mBehN; long& w = ol.mBehW;
    int i=0; for(; i<nh; i++) if(addr[i]==h) break;
    if(i==nh && nh<64){ addr[nh]=h; cnt[nh]=0; nh++; }
    if(i<64) cnt[i]++;
    if((++w % 300)==0){ fprintf(stderr,"[behhist] distinct=%d handlers:\n", nh);
      for(int j=0;j<nh;j++) fprintf(stderr,"   %08X  x%ld\n", addr[j], cnt[j]); }
  }
  eng(c).behaviors.dispatchObj(node, h);
}

inline void walk_list(Core* c, uint32_t head, long* count) {
  for (uint32_t n = head; n; ) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    call_handler(c, n);
    n = next; (*count)++;
  }
}

}  // namespace

void ObjectList::walkAll() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x8007A904u, walkAllFaithful()); return; }
  long nodes = 0;
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_1), &nodes);
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_2), &nodes);
  rend(c)->margin.flush(c);

  if (mDbg < 0) mDbg = cfg_dbg("engine") ? 1 : 0;
  if (mDbg && (mWalksAll % 300) == 0)
    fprintf(stderr, "[engine] objwalk #%ld: %ld nodes\n", mWalksAll, nodes);
  mWalksAll++;
}

// pc_faithful mirror of gen_func_8007A904 (guest FUN_8007A904). Guest frame (sp-24, ra@+20,
// s0(r16)@+16) descended/spilled up front, matching the gen's unconditional delay-slot spill
// (both happen before the list-1 empty check). Each dispatch sets r31 to the RE'd jal-site
// constant (0x8007A930 list-1 / 0x8007A964 list-2) and carries `next` in r16 across the call so
// a substrate-routed handler's own ra/s0 spill byte-matches core B. No MarginRenderer::flush —
// gen_func_8007A904 never calls it (that's a render-overlay addition, deferred — see the
// unconditional call still in walkAll() above for the pc_skip=true path).
void ObjectList::walkAllFaithful() {
  Core* c = core;
  uint32_t node = c->mem_r32(T2_OBJLIST_HEAD_1);
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);

  while (node) {
    c->r[16] = c->mem_r32(node + T2OBJ_NEXT);   // s0 = next — live value for a callee's own spill
    c->r[31] = 0x8007A930u;                     // jal-site ra (list-1 loop)
    c->mem_w8(node + T2OBJ_RENDER_FLAG, 0);
    call_handler(c, node);
    node = c->r[16];
  }

  node = c->mem_r32(T2_OBJLIST_HEAD_2);
  while (node) {
    c->r[16] = c->mem_r32(node + T2OBJ_NEXT);
    c->r[31] = 0x8007A964u;                     // jal-site ra (list-2 loop)
    c->mem_w8(node + T2OBJ_RENDER_FLAG, 0);
    call_handler(c, node);
    node = c->r[16];
  }

  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] = sp + 24;
}

void ObjectList::walkList2() {
  Core* c = core;
  long nodes = 0;
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_2), &nodes);
  // NB: no MarginRenderer::flush here — that belongs to walkAll (walkList2 is a distinct call site
  // from Sop::fieldUpdate, not a replacement of the whole entity walk).

  if (mDbg < 0) mDbg = cfg_dbg("engine") ? 1 : 0;
  if (mDbg && (mWalksL2 % 300) == 0)
    fprintf(stderr, "[engine] objwalk_l2 #%ld: %ld nodes\n", mWalksL2, nodes);
  mWalksL2++;
}

void ObjectList::walkAux() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80069B28u, walkAuxFaithful()); return; }
  // FUN_80069B28: does NOT clear the render flag; dispatches per handler ptr via the shared path.
  for (uint32_t n = c->mem_r32(AUX_LIST_HEAD); n; ) {
    uint32_t h    = c->mem_r32(n + 0x1Cu);
    uint32_t next = c->mem_r32(n + 0x24u);
    eng(c).behaviors.dispatchObj(n, h);
    n = next;
  }
}

// pc_faithful mirror of gen_func_80069B28 (guest FUN_80069B28). Guest frame (sp-24, ra@+20,
// s0(r16)@+16) spilled UNCONDITIONALLY before the loop (matches gen — the spill happens even
// when the aux list is empty). v0 (r2) is ALSO clobbered unconditionally by gen's LUI half of the
// AUX_LIST_HEAD address load, even on the empty-list fast path — reproduced explicitly below,
// else v0 is left stale from whatever computation preceded this call (caught by MV_CHECK: native
// left a stale v0 while the substrate always shows 0x800F0000 on an empty aux list). Each dispatch
// is issued with r31 set to the RE'd jal site (0x80069B50) so a substrate-routed handler's own ra
// spill byte-matches core B, and `next` is carried in r16 across the call (not a host local) so a
// substrate handler's s0 spill also byte-matches. Does NOT clear the render flag (only
// walkAll/walkList2 do that).
void ObjectList::walkAuxFaithful() {
  Core* c = core;
  c->r[2] = (uint32_t)32783u << 16;    // gen's LUI half of the AUX_LIST_HEAD address load — clobbers
                                        // v0 unconditionally (even on the empty-list fast path), so it
                                        // must be reproduced here or v0 is left stale from an earlier
                                        // computation instead of 0x800F0000.
  uint32_t node = c->mem_r32(AUX_LIST_HEAD);
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);
  while (node != 0) {
    uint32_t h = c->mem_r32(node + 0x1Cu);
    c->r[16] = c->mem_r32(node + 0x24u);   // next — carried in s0 across the call, matches gen
    c->r[31] = 0x80069B50u;                // jal-site ra — matches gen's spill target for callees
    eng(c).behaviors.dispatchObj(node, h);
    node = c->r[16];
  }
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}
