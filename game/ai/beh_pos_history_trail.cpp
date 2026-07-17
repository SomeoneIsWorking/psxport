// game/ai/beh_pos_history_trail.cpp — PC-native per-object BEHAVIOR handler FUN_80029B40.
//
// Resident handler (~167-line state machine). Ported from the Ghidra decompile of FUN_80029B40
// (scratch/decomp/ram_f1000_all.c). A "trailing position history" actor: it follows a TARGET object
// (pointer at node[0x10], here `tgt`) and maintains a 6-slot ring of recorded positions at node+0x38
// (each slot = 8 bytes: u16 x,y,z,w). Each active frame it shifts the ring forward by one slot and seeds
// the head (slot 0) from the target's current position (tgt[0x2e]/[0x32]/[0x36]). Outer state machine on
// node[4]:
//   STATE 0 : INIT — node[0x2C]=4, node[4]=1, node[5]=node[6]=node[7]=node[1]=0, node[0x2E]=0xA0, zero the
//             ring slots 0..4 (node+0x38..0x60), then node[0x30]=tgt[0x2E], node[0x32]=tgt[0x32],
//             node[0x36]=0, node[0x34]=tgt[0x36]. return.
//   STATE 1 : ADVANCE — branch on n5=node[5]:
//               n5==0 : if tgt[0]==2 -> node[5]=1, fall into the n5==1 logic; else just commit tail.
//               n5==1 : (Lc70) if tgt[0]==1 && tgt[0x5E]==1 -> node[5]++ then (Lca0) record tgt pos into
//                       node[0x30..0x36] (node[0x36]=tgt[0x6A]&0xFF0) and run the node[3]-routed delay/
//                       advance logic (node[6] delay counter; node[5]++ on expiry via Ld98/Lda8). else
//                       commit tail unchanged.
//               n5==2 : (Lca0) same record+route block.
//               n5==3 : (Lda8) saturating node[0x36]&7 counter (<3 ? ++ : node[5]++).
//               n5 other : node[1]=1, go to commit tail (Lde0).
//             COMMIT TAIL (Lde0): if tgt[4]>1 || (DAT_800E7FC6&4) || DAT_800E7FC6==0 -> node[0x2D]=
//             node[0x2C], node[4]=2. Then shift the ring forward and seed slot 0 (node[0x38/0x3A/0x3C])
//             from tgt[0x2E]/[0x32]/[0x36].
//   STATE 2 : RETIRE-WALK — shift ring forward, clear slot-0 first three shorts, node[1]=1. If node[5]>1:
//             advance the cursor node[0x2C] (0..4 wrap), clear the indexed slot's 4th short, bump the
//             saturating node[0x36]&7 counter, and if cursor wrapped back to node[0x2D] -> node[4]=3. Else
//             (node[5]<=1) if the slot-4 int node[0x58]==0 -> node[4]=3.
//   STATE 3 : FUN_8007A624(node) (despawn-family leaf).
//
// CONTROL FLOW + every direct node WRITE owned native at the SAME offset/width as the decompile (undefined
// = byte, undefined2/short/ushort = 16-bit, the node[0x58] read = 32-bit int, the ring copy = two 32-bit
// words per slot). The single sub-behavior CALL (FUN_8007A624) stays a pure-PSX leaf via rec_dispatch. The
// target object (tgt = node[0x10]) is only READ here, never written — so it can't corrupt the still-recomp
// owner. The original goto structure (Lc70/Lca0/Ld90/Ld98/Lda8/Lddc/Lde0) is preserved exactly. The
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80029B40u;

// Shift the 6-slot ring at node+0x38 forward by one slot: slot[i] <- slot[i-1] for i=5..1 (each slot is
// two 32-bit words, copied high-to-low exactly as the decompile's iVar7=5..1 loop).
static inline void ring_shift_forward(Core* c, uint32_t nd) {
  for (int i = 5; i >= 1; i--) {
    uint32_t d = nd + 0x38u + 8u * (uint32_t)i;
    c->mem_w32(d, c->mem_r32(d - 8));
    c->mem_w32(d + 4, c->mem_r32(d - 4));
  }
}

}  // namespace

void beh_pos_history_trail(Core* c) {
  uint32_t nd = c->r[4];
  uint32_t tgt = c->mem_r32(nd + 0x10);          // pcVar8 = *(char**)(node+0x10)
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st < 2) {
      if (st != 0) return;
      // ---- STATE 0 : INIT ----
      c->mem_w8(nd + 0x2c, 4);
      c->mem_w8(nd + 4, 1);
      c->mem_w8(nd + 5, 0);
      c->mem_w8(nd + 6, 0);
      c->mem_w8(nd + 7, 0);
      c->mem_w8(nd + 1, 0);
      c->mem_w8(nd + 0x2e, 0xa0);
      for (int i = 0; i < 5; i++) {              // zero ring slots 0..4 (node+0x38..0x60)
        uint32_t d = nd + 0x38u + 8u * (uint32_t)i;
        c->mem_w32(d, 0);
        c->mem_w32(d + 4, 0);
      }
      c->mem_w16(nd + 0x30, c->mem_r16(tgt + 0x2e));
      c->mem_w16(nd + 0x32, c->mem_r16(tgt + 0x32));
      uint16_t t36 = c->mem_r16(tgt + 0x36);
      c->mem_w16(nd + 0x36, 0);
      c->mem_w16(nd + 0x34, t36);
      return;
    }
    if (st != 2) {
      if (st != 3) return;
      // ---- STATE 3 ----
      eng(c).spawn.despawn(nd);                 // FUN_8007A624 (still-PSX leaf)
      return;
    }
    // ---- STATE 2 : RETIRE-WALK ----
    ring_shift_forward(c, nd);
    c->mem_w16(nd + 0x38, 0);                     // *puVar10 = 0
    c->mem_w16(nd + 0x3a, 0);
    c->mem_w16(nd + 0x3c, 0);
    c->mem_w8(nd + 1, 1);
    if (c->mem_r8(nd + 5) > 1) {                  // (byte)node[5] > 1
      int8_t cv = c->mem_r8s(nd + 0x2c);
      int8_t nv = (int8_t)(cv + 1);
      if (nv > 4) nv = 0;                         // 0..4 wrap (matches '\x04' < (char)(cv+1))
      c->mem_w8(nd + 0x2c, (uint8_t)nv);
      c->mem_w16(nd + 0x38u + 2u * ((uint32_t)(int32_t)nv * 4u + 3u), 0); // puVar10[nv*4+3] = 0
      uint16_t m36 = c->mem_r16(nd + 0x36);
      if ((m36 & 7) < 3) c->mem_w16(nd + 0x36, (uint16_t)(m36 + 1));
      if (c->mem_r8s(nd + 0x2c) != c->mem_r8s(nd + 0x2d)) return;
      c->mem_w8(nd + 4, 3);
      return;
    }
    if (c->mem_r32(nd + 0x58) != 0) return;       // slot-4 int
    c->mem_w8(nd + 4, 3);
    return;
  }

  // ======== STATE 1 : ADVANCE ========
  uint8_t n5 = c->mem_r8(nd + 5);
  uint8_t n3 = 0;
  if (n5 == 1) {
    goto Lc70;
  } else if (n5 < 2) {                            // n5 == 0
    if (c->mem_r8s(tgt + 0) == 2) {
      c->mem_w8(nd + 5, 1);
      goto Lc70;
    }
    goto Lddc;
  } else {
    if (n5 == 2) goto Lca0;
    if (n5 != 3) { c->mem_w8(nd + 1, 1); goto Lde0; }
    goto Lda8;                                    // n5 == 3
  }

 Lc70:
  if (!(c->mem_r8s(tgt + 0) == 1 && c->mem_r8s(tgt + 0x5e) == 1)) goto Lddc;
  c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
 Lca0:
  c->mem_w16(nd + 0x30, c->mem_r16(tgt + 0x2e));
  c->mem_w16(nd + 0x32, c->mem_r16(tgt + 0x32));
  c->mem_w16(nd + 0x34, c->mem_r16(tgt + 0x36));
  c->mem_w16(nd + 0x36, (uint16_t)(c->mem_r16(tgt + 0x6a) & 0xff0));
  n3 = c->mem_r8(nd + 3);
  if (n3 == 0) {
    if (c->mem_r8s(tgt + 0) != 1) goto Ld98;
  } else {
    if (n3 > 2) {
      if (n3 < 6) {                               // node[3] in {3,4,5}
        if (c->mem_r8s(tgt + 0x2b) != 2) {
          uint8_t n6 = c->mem_r8(nd + 6);
          c->mem_w8(nd + 6, (uint8_t)(n6 + 1));
          if (n6 < 4) goto Lddc;
        }
      } else if (c->mem_r8s(tgt + 0x2b) != 3) {  // node[3] >= 6
        uint8_t n6 = c->mem_r8(nd + 6);
        c->mem_w8(nd + 6, (uint8_t)(n6 + 1));
        if (n6 < 3) goto Lddc;
      }
      goto Ld98;
    }
    // node[3] in {1,2}
    if (c->mem_r8s(tgt + 0) != 1 || c->mem_r8s(tgt + 0x29) != 0) goto Ld98;
  }
  c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
  goto Lddc;

 Ld98:
  c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
 Lda8:
  {
    uint16_t m36 = c->mem_r16(nd + 0x36);
    if ((m36 & 7) < 3) c->mem_w16(nd + 0x36, (uint16_t)(m36 + 1));
    else c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
  }
 Lddc:
  c->mem_w8(nd + 1, 1);
 Lde0:
  {
    uint8_t dat = c->mem_r8(0x800e7fc6u);         // DAT_800E7FC6 (byte global)
    if (((uint8_t)c->mem_r8(tgt + 4) > 1) || ((dat & 4) != 0) || (dat == 0)) {
      c->mem_w8(nd + 0x2d, c->mem_r8(nd + 0x2c));
      c->mem_w8(nd + 4, 2);
    }
    ring_shift_forward(c, nd);                    // iVar7 == 5 in every path that reaches here
    c->mem_w16(nd + 0x38, c->mem_r16(tgt + 0x2e));
    c->mem_w16(nd + 0x3a, c->mem_r16(tgt + 0x32));
    c->mem_w16(nd + 0x3c, c->mem_r16(tgt + 0x36));
  }
}
