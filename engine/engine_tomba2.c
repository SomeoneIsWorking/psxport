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
// A/B TOGGLE: native walk is the DEFAULT (it IS the engine now); set PSXPORT_RECOMP_OBJWALK=1 to fall
// back to the recomp body (the oracle) for diffing. Verified native==recomp: VRAM bit-identical at
// frames 4000 and 4720 of real gameplay (1 MB cmp PASS each) — see docs/journal.md.
#include "r3000.h"
#include "cfg.h"
#include "tomba2_types.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern uint32_t mem_r32(uint32_t);
extern void     mem_w8(uint32_t, uint8_t);
extern void     rec_dispatch(R3000* c, uint32_t addr);   // hybrid call: recomp body if emitted, else interp
extern uint32_t g_current_object;                        // wide60: tag the object a handler's geometry belongs to
void            gen_func_8007A904(R3000*);               // the recomp body (oracle / super-call)

static int  s_dbg   = -1;
static long s_walks = 0;

// Call one node's handler exactly as the recomp does: a0 = node, jalr *(node+0x1c). Tag g_current_object
// = node around the WHOLE handler so wide60 attributes ALL geometry it submits to this entity (the
// interpolation identity), independent of whether projection happens inside the cull subtree.
static inline void call_handler(R3000* c, uint32_t node) {
  uint32_t prev = g_current_object;
  g_current_object = node;
  c->r[4] = node;                                  // $a0
  rec_dispatch(c, mem_r32(node + T2OBJ_HANDLER));
  g_current_object = prev;
}

static void walk_list(R3000* c, uint32_t head, long* count) {
  for (uint32_t n = head; n; ) {
    uint32_t next = mem_r32(n + T2OBJ_NEXT);       // read next BEFORE the handler may unlink the node
    mem_w8(n + T2OBJ_RENDER_FLAG, 0);              // engine clears the per-frame render flag
    call_handler(c, n);
    n = next; (*count)++;
  }
}

// Native FUN_8007a904. Second list head is re-read fresh after list 1 (handlers in list 1 may mutate
// it) — matches the recomp body, which reloads DAT_800f2624 across the first loop.
static void ov_objwalk(R3000* c) {
  long nodes = 0;
  walk_list(c, mem_r32(T2_OBJLIST_HEAD_1), &nodes);
  walk_list(c, mem_r32(T2_OBJLIST_HEAD_2), &nodes);

  if (s_dbg < 0) s_dbg = cfg_dbg("engine") ? 1 : 0;
  if (s_dbg && (s_walks % 300) == 0)
    fprintf(stderr, "[engine] objwalk #%ld: %ld nodes\n", s_walks, nodes);
  s_walks++;
}

void engine_tomba2_init(void) {
  if (cfg_on("PSXPORT_RECOMP_OBJWALK")) return;    // oracle fallback: keep the recomp FUN_8007a904
  rec_set_override(T2_OBJWALK_FN, ov_objwalk);     // 0x8007A904 — native engine owns the object walk
  if (cfg_dbg("engine"))
    fprintf(stderr, "[engine] native object-list walk active (FUN_8007a904)\n");
}
