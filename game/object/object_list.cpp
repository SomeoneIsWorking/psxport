// ObjectList::walkAll / walkAux — see object_list.h.
//
// Faithful ports of guest FUN_8007A904 / FUN_80069B28. Both use the shared `dispatch_obj_method`
// helper (game/object/engine_tomba2.cpp), the same native-or-substrate handler dispatch used by
// TransitionState3::walkOnce. The `behhist` diagnostic (from the pre-restructure call_handler)
// is preserved in the main walk since it feeds top-down ownership decisions.
#include "object_list.h"
#include "core.h"
#include "cfg.h"
#include "game.h"                 // Fps60State::current_object
#include "tomba2_types.h"         // T2_OBJLIST_HEAD_1/2, T2OBJ_HANDLER/NEXT/RENDER_FLAG
#include "margin_render.hpp"      // margin_render_flush (native widescreen margin pass)
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core* c, uint32_t addr);
bool dispatch_native_behavior(Core* c, uint32_t h);   // native-behavior table lookup (engine_tomba2.cpp)

namespace {

// Per-node handler dispatch (was static call_handler + walk_list helpers). Local to this TU.
inline void call_handler(Core* c, uint32_t node) {
  uint32_t prev = c->game->fps60.current_object;
  c->game->fps60.current_object = node;
  c->r[4] = node;
  uint32_t h = c->mem_r32(node + T2OBJ_HANDLER);
  if (cfg_dbg("behhist")) {
    static uint32_t addr[64]; static long cnt[64]; static int nh=0; static long w=0;
    int i=0; for(; i<nh; i++) if(addr[i]==h) break;
    if(i==nh && nh<64){ addr[nh]=h; cnt[nh]=0; nh++; }
    if(i<64) cnt[i]++;
    if((++w % 300)==0){ fprintf(stderr,"[behhist] distinct=%d handlers:\n", nh);
      for(int j=0;j<nh;j++) fprintf(stderr,"   %08X  x%ld\n", addr[j], cnt[j]); }
  }
  if (!dispatch_native_behavior(c, h)) rec_dispatch(c, h);
  c->game->fps60.current_object = prev;
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
  long nodes = 0;
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_1), &nodes);
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_2), &nodes);
  margin_render_flush(c);

  static int  s_dbg   = -1;
  static long s_walks = 0;
  if (s_dbg < 0) s_dbg = cfg_dbg("engine") ? 1 : 0;
  if (s_dbg && (s_walks % 300) == 0)
    fprintf(stderr, "[engine] objwalk #%ld: %ld nodes\n", s_walks, nodes);
  s_walks++;
}

void ObjectList::walkAux() {
  Core* c = core;
  // FUN_80069B28: does NOT clear the render flag; dispatches per handler ptr via the shared path.
  void dispatch_obj_method(Core* c, uint32_t obj, uint32_t h);
  for (uint32_t n = c->mem_r32(AUX_LIST_HEAD); n; ) {
    uint32_t h    = c->mem_r32(n + 0x1Cu);
    uint32_t next = c->mem_r32(n + 0x24u);
    dispatch_obj_method(c, n, h);
    n = next;
  }
}
