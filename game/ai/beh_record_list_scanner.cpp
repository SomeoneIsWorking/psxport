// game/ai/beh_record_list_scanner.cpp — PC-native per-object BEHAVIOR handler FUN_8004CE14.
//
// A RESIDENT MAIN.EXE per-object behavior routine (prologue 0x8004CE14; `jr ra` at 0x8004D0CC).
// Same SHAPE as the sibling owned behaviors (the FUN_80071a3c handler / the FUN_739ac handler / …): a state machine on the
// node's state byte node[4]. RE'd 1:1 from disas 0x8004CE14:
//
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   default(>=4): nothing.
//   STATE 0 : if (area-flag byte 0x800BF873 != 0) { node[4]=3; return; }
//             else: node[4]=1, node[0]=1, node[0x74]=0, FUN_8004B354(node,0), node[0x6c] = table
//             0x800A3F00[node[3]].  Then FALL THROUGH into the shared scan block (same as state 1).
//   STATE 1 : the shared scan block directly.
//
//   Shared scan block: decide run-vs-early-return from the area byte 0x800BF870 + node[3] + the camera
//   scroll byte 0x1F800207, then walk the 16-byte record list at node[0x6c] until a 0xFF terminator.
//   Per record, a short-circuit visibility test (record flag bit7 selects FUN_8004D7EC vs FUN_8004D868;
//   the persistent mask node[0x74] gates) decides whether to ACT: act = FUN_80111CCC(record[12]) when
//   area==1 && 0x800BF871>=15, else FUN_80077ACC(node, record[4], record[6], record[8]); a nonzero result
//   ORs bit s3 into node[0x70]. At the terminator: node[0x6a]=count, node[11]=31, FUN_80077EFC(node),
//   node[1]=1.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node/global memory WRITES owned native;
// every sub-behavior CALL stays reachable by address via rec_dispatch (leaf, no recursion). NO GTE, NO
// render packets. Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) like every owned behavior.
// v0 is NOT reproduced: the per-object dispatcher (call_handler) ignores the return; the gate compares
// only RAM+scratchpad — matching the sibling objbeh gates.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::cullWrap77acc / installSceneRecord
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "mathlib.h"  // Bit::test7EC / test868 (FUN_8004D7EC / FUN_8004D868)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8004CE14u;

inline uint32_t lh(Core* c, uint32_t a) { return (uint32_t)c->mem_r16s(a); }

}  // namespace

void beh_record_list_scanner(Core* c) {
  uint32_t obj = c->r[4];
  uint32_t s5  = obj + 0x60;
  uint8_t state = c->mem_r8(obj + 4);

  if (state == 2) return;                                       // no-op
  if (state == 3) { eng(c).spawn.despawn(obj); return; }
  if (state > 3) return;                                        // default

  if (state == 0) {
    if (c->mem_r8(0x800BF873u) != 0) { c->mem_w8(obj + 4, 3); return; }
    uint32_t d3 = c->mem_r8(obj + 3);
    c->mem_w8(obj + 4, 1);
    c->mem_w8(obj + 0, 1);
    uint32_t tv = c->mem_r32(0x800A3F00u + d3 * 4u);
    c->mem_w32(s5 + 20, 0);                                     // node[0x74] = 0
    c->mem_w8(obj + 0x18, 0);           // FUN_8004B354(obj, 0) inlined — arg1==0 path: single-byte reset
    c->mem_w32(s5 + 12, tv);                                    // node[0x6c] = table value
  }

  // shared scan block (state 0 fall-through + state 1): early-return gate
  uint32_t area = c->mem_r8(0x800BF870u);
  uint32_t d3   = c->mem_r8(obj + 3);
  if (area == 0) {
    if (d3 == 1) {
      if (!(c->mem_r8(0x1F800207u) < 28)) return;
    } else if (d3 == 2) {
      if (c->mem_r8(0x1F800207u) < 28) return;
    }
  } else if (area == 6) {
    if (c->mem_r8(0x800BF871u) == 19) return;
  }

  // record-list scan
  uint32_t s4 = c->mem_r32(s5 + 12);                            // node[0x6c]
  c->mem_w32(s5 + 16, 0);                                       // node[0x70] = 0
  int s3 = 0;
  for (;;) {
    if (c->mem_r8(s4) == 0xFFu) {                               // terminator
      c->mem_w16(s5 + 10, (uint16_t)s3);                        // node[0x6a] = count
      c->mem_w8(obj + 11, 31);
      eng(c).cull.enqueueQueueC(obj);                    // FUN_80077EFC (native; return ignored)
      c->mem_w8(obj + 1, 1);
      return;
    }
    uint32_t s2 = c->mem_r8(s4 + 2) & 0x80u;
    uint32_t s1 = 1u << (s3 & 31);
    int verdict = -2;                                           // -2 = undecided, 0 = skip, 1 = act
    if (s2 == 0) {
      if (eng(c).bit.test7EC((int32_t)lh(c, s4 + 10), 0) != 0) verdict = 0;
    }
    if (verdict == -2 && (c->mem_r32(obj + 0x74) & s1) != 0) verdict = 0;
    if (verdict == -2 && s2 != 0) {
      if (eng(c).bit.test868((int32_t)lh(c, s4 + 10)) != 0) verdict = 0;
    }
    if (verdict == -2) verdict = ((c->mem_r32(obj + 0x74) & s1) != 0) ? 0 : 1;

    if (verdict == 1) {
      uint32_t ret;
      if (area == 1 && c->mem_r8(0x800BF871u) >= 15) {
        c->r[4] = c->mem_r8(s4 + 12); rec_dispatch(c, 0x80111CCCu);
        ret = c->r[2];
      } else {
        c->r[4] = obj; c->r[5] = lh(c, s4 + 4); c->r[6] = lh(c, s4 + 6); c->r[7] = lh(c, s4 + 8);
        eng(c).cull.cullWrap77acc();          // FUN_80077ACC (native)
        ret = c->r[2];
      }
      if (ret != 0) c->mem_w32(s5 + 16, c->mem_r32(s5 + 16) | s1);
    }
    s3 += 1;
    s4 += 16;
  }
}
