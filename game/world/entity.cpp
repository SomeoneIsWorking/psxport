// game/world/entity.cpp — PC-native per-object ENTITY STATE-MACHINE subsystem.
// The per-object behavior cluster that drives each entity's logic: the child-node spawn / sub-object
// builder (FUN_80040410), the per-object dispatcher loop over the 0x800ec188 table (FUN_80026C88), the
// state-machine head (FUN_80040558), and the oscillate / frame-toggle sub-behavior (FUN_8003FD10).
// Control flow + object memory owned native; the per-state sub-behaviors stay reachable by address via
// rec_dispatch (each honors its own override identically). NO GTE, NO render packets. Extracted verbatim
// from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code structure.
// Diagnostic A/B gates (child40410/disp26c88/sm40558/fd10) are REPL channels, unchanged.
#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entity.h"
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update
#include "rng.h"       // class Rng (via rngOf(c).next())
#include "render/cull.h"     // class Cull (eng(c).cull.enqueueQueueA — FUN_80077E7C)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// FUN_80040410 — per-object CHILD-NODE SPAWN / sub-object builder (a callee of the per-object state
// machine FUN_80040558's state-0 handler). a0 = obj, a1 = group index (low byte). NO GTE, NO render
// packets; pure control flow + object/child-node memory writes, with 2 dispatched callees.
//   obj[8] = 2 (child count, set unconditionally on entry).
//   if ((int16)*0x800ed098 < 2): obj[4] = 3; return 0  (global gate — not ready yet).
//   else:
//     obj[9]=2, obj[13]=0, obj[11]=0, sh obj[84]=obj[86]=obj[88]=0.
//     count = obj[8] (the 2 just written); s0 (per-child base) = obj; for i in [0,count):
//       node = jal 0x8007aae8()          (child-node allocator, dispatched -> v0 = node ptr)
//       s0[0xC0] = node                  (store the child ptr at obj+0xC0 + 4*i)
//       node[6] = (i - 1) as s16         (0xFFFF on the first child)
//       node[0] = u16 tblA[6*i + 0],  node[2] = u16 tblA[6*i + 2],  node[4] = u16 tblA[6*i + 4]
//                                        (tblA = 0x800a3b1c, stride 6)
//       node[8] = node[0xA] = node[0xC] = 0
//       a2 = lh tblB[2*((a1&0xff) + i)]  (tblB = 0x800a3b28, stride 2, base index = a1&0xff)
//       jal 0x80051b04(a0=node, a1=1, a2)  (transform/geom setup -> writes node[0x40], dispatched)
//     return 1.
// CONTROL FLOW + every memory write owned native; the allocator 0x8007aae8 and the setup 0x80051b04 stay
// PSX via rec_dispatch (each honors its own override identically in the super-call path). GOTCHA: the
// child count read (v1=obj[8]) is the value just stored (2) — re-read from memory; the loop counter
// increments BEFORE the tblA stores complete but AFTER node[6]=s2-1 is stored (delay-slot ordering),
// so node[6] uses the PRE-increment index. `child40410` gate = full RAM+scratchpad A/B vs rec_super_call.
static uint32_t child_spawn_40410(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t a1  = c->r[5] & 0xffu;
  c->mem_w8(obj + 8, 2);
  if (c->mem_r16s(0x800ed098u) < 2) { c->mem_w8(obj + 4, 3); return 0; }
  c->mem_w8(obj + 9, 2);
  c->mem_w8(obj + 13, 0);
  c->mem_w8(obj + 11, 0);
  c->mem_w16(obj + 84, 0);
  c->mem_w16(obj + 86, 0);
  c->mem_w16(obj + 88, 0);
  uint32_t count = c->mem_r8(obj + 8);
  uint32_t s0 = obj;                 // per-child base for obj[0xC0 + 4*i]
  uint32_t s1 = 0x800a3b1cu;         // tblA cursor (stride 6)
  uint32_t s3 = a1 << 2;             // tblB byte offset = (a1&0xff)*4, +2 per iter
  const uint32_t s5 = 0x800a3b28u;   // tblB base
  for (uint32_t i = 0; i < count; i++) {
    c->r[4] = 0; eng(c).graphicsBind.recordAlloc();     // allocate child node
    uint32_t node = c->r[2];
    c->mem_w32(s0 + 0xC0, node);
    c->mem_w16(node + 6, (uint16_t)(i - 1));        // node[6] = (i-1) as s16
    c->mem_w16(node + 0, c->mem_r16(s1 + 0));
    c->mem_w16(node + 2, c->mem_r16(s1 + 2));
    c->mem_w16(node + 4, c->mem_r16(s1 + 4));
    c->mem_w16(node + 8, 0);
    c->mem_w16(node + 0xA, 0);
    c->mem_w16(node + 0xC, 0);
    uint32_t a2 = (uint32_t)c->mem_r16s(s5 + s3);
    eng(c).graphicsBind.installSceneRecord(node, 1, a2);   // FUN_80051B04 (native)
    s1 += 6; s3 += 2; s0 += 4;
  }
  return 1;
}

// FUN_80026C88 — per-object DISPATCHER LOOP over the 40-entry, 64-byte-stride object table at 0x800ec188.
// args: none. void return. Pure control flow — the loop body reads only obj[0] (active byte) and obj[1]
// (handler index), loads a fn-ptr from the table at 0x800ad52c (stride 4), and tail-calls it with a0=obj.
// The dispatcher writes NOTHING to memory itself (the recomp body only saves/restores s0/s1/s2/ra in its
// own stack frame, which the native body never touches); ALL side effects live inside the dispatched
// handlers, which stay PSX via rec_dispatch (each honors its own owned override identically in this path).
// NO GTE, NO render-packet writes in the dispatcher. RE from gen_func_80026C88 / disas 0x80026C88:
//   s2 = 0x800ad52c (handler fn-ptr table, stride 4); s0 = 0x800ec188 (object table, stride 64); i = 0
//   for i in [0,40): v0 = lbu obj[0]; if v0 != 0 { idx = lbu obj[1]; fn = *(s2 + idx*4); a0 = obj; (*fn)(); }
//                    i++; obj += 64
// `disp26c88` gate = full RAM+scratchpad A/B vs rec_super_call (native run → snapshot+rollback → super_call
// → diff). Same family rationale as the other dispatcher-loop gates (sm40558/grid): the dispatched handlers
// run in BOTH passes and leave transient residue in their own stack frames below entry sp (no native frame
// there) + this fn's own 32-byte frame is dead below sp on return → exclude [sp-0x800, sp) (sp ~0x1FExxx,
// RAM end 0x200000 — far above ALL game data; a real divergence would alter persistent state).
// (disp_26c88 / ov_disp_26c88 moved to ObjectTable::dispatch — game/world/object_table.cpp.)

// FUN_80040558 — per-object STATE-MACHINE HEAD (the dispatcher whose state-0 handler calls the just-owned
// child-spawn FUN_80040410; owning the head advances the whole behavior family). a0 = obj. Pure control
// flow + object byte/halfword writes + global/scratchpad reads; NO GTE, NO render packets. Every `jal` is
// a sub-behavior kept PSX via rec_dispatch (each honors its own override identically in the super-call
// path). Dispatch on the state byte obj[4]: 0 / 1 / 2 / 3 / else.
//   STATE 3 (@a40): jal 0x8007a624(obj); return.
//   STATE 0 (@5ac): sub-dispatch on obj[5]:
//     obj[5]==0 (@5cc): v0 = jal 0x80040410(obj, a1=obj[3]); if v0!=0 -> obj[5]++; then ALWAYS:
//        sh obj[128]=64, sh obj[130]=128, sb obj[41]=0, sb obj[43]=0, sb obj[95]=0,
//        sh obj[132]=150, sh obj[134]=150, sb obj[70]=0; return.  (the +64 in obj[128] comes from the
//        delay-slot constant whether or not obj[5] was bumped).
//     obj[5]==1 (@620): v1=obj[94]; if v1<8 -> jump table 0x800152e0[v1], else v0=1 (@6c0).
//        jt[0]@650 jal 0x8003fbc4; v0=1.   jt[1]@660 jal 0x8003fc00; v0=1.
//        jt[2]@670 jal 0x801286f4; v0=1.   jt[3]@6c0 v0=1.   jt[4]@680 jal 0x8003fc78; v0=1.
//        jt[5]@690 jal 0x80120188; v0=1.   jt[6]@6a0 jal 0x8003fc8c; v0=ret. jt[7]@6b0 jal 0x801146e8; v0=ret.
//        @6b8: if v0==0 return; @6c4: sb obj[4]=v0, sb obj[5]=0, sb obj[0]=v0, sb obj[41]=0; return.
//     else: return.
//   STATE 1 (@6d8): g870=*0x800bf870; if g870==18 { if *0x800bfa59==0 return; } else if g870==19 { if
//      *0x800bf871==19 return; }. Then @720: v1=obj[5]; if v1<6 -> jt 0x80015300[v1] (jal one of
//      0x8003fd10/fed8/ffcc/4022c/40390/0x80114934), else fall to @7a8.
//      @7a8: v1=obj[94]; if v1<8 -> jt 0x80015318[v1], else @8c8. jt: [0,1,3,4,6]->@7e0, [2]->@888,
//      [5]->@8c0, [7]->@7d8 (=@7e0 prefixed by jal 0x8012b118).
//        @7e0: if *0x800bf816!=0 && *0x800bf817==obj[106](s16) && (obj[40]&0x80) { sb obj[1]=1;
//              jal 0x80077e7c(obj); goto @878 } else goto @834.
//        @834: if (obj[40]&0x80) goto @8c8; else if *0x800bf870==8 v0=jal 0x8012e168(obj) else
//              v0=jal 0x8007778c(obj); if v0==0 goto @8c8; @878 jal 0x800517f8(obj); sb obj[41]=0; return.
//        @888: v0=*(*obj[16] + 1)(u8); sb obj[1]=v0; if (v0&0xff)==0 goto @8c8; else jal 0x8012866c(obj),
//              jal 0x80077e7c(obj); sb obj[41]=0; return.
//        @8c0: jal 0x801201e0(obj); (fall to @8c8).   @8c8: sb obj[41]=0; return.
//   STATE 2 (@8d4): v1=obj[5]; if v1<5 -> jt 0x80015338[v1], else @964.
//      jt[0]/jt[4]=@964, jt[1]=@904, jt[2]=@94c (jal 0x8003fe00), jt[3]=@95c (jal 0x8003fed8 -> @964).
//        @904: if obj[3]==0 && *0x800bfad1==0 -> jal 0x80040b48(a0=56); @92c: if obj[94]==2 { v1b=*obj[16];
//              sb *(v1b+94)=1; } goto @964.
//        @964: if obj[94]==2 { v0=*(*obj[16]+1)(u8); sb obj[1]=v0; jal 0x8012866c(obj); jal 0x80077e7c(obj);
//              return } else @99c (mirrors state-1's @7e0..tail with the same global checks +
//              jal 0x8012e168/0x8007778c + jal 0x800517f8, sb obj[1]=1 path).
// `sm40558` gate = full RAM+scratchpad A/B vs rec_super_call (same family rationale as child40410: every
// dispatched callee runs in BOTH passes leaving residue below entry sp, and this fn's own 24-byte frame is
// dead there on return -> exclude [sp-0x800, sp), far above all game data).
static void sm40558(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t G = 0x800BF870u;                 // global block base (0x800bf870 = mode byte)
  uint32_t st = c->mem_r8(obj + 4);

  if (st == 3) { eng(c).spawn.despawn(obj); return; }

  if (st == 0) {
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 == 0) {
      c->r[4] = obj; c->r[5] = c->mem_r8(obj + 3);
      c->r[2] = child_spawn_40410(c);
      if (c->r[2] != 0) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      c->mem_w16(obj + 128, 64);
      c->mem_w16(obj + 130, 128);
      c->mem_w8 (obj + 41, 0);
      c->mem_w8 (obj + 43, 0);
      c->mem_w8 (obj + 95, 0);
      c->mem_w16(obj + 132, 150);
      c->mem_w16(obj + 134, 150);
      c->mem_w8 (obj + 70, 0);
      return;
    }
    if (s5 == 1) {
      uint32_t v94 = c->mem_r8(obj + 94);
      uint32_t v0;
      switch (v94) {
        case 0: c->r[4]=obj; rec_dispatch(c, 0x8003FBC4u); v0=1; break;
        case 1: c->r[4]=obj; rec_dispatch(c, 0x8003FC00u); v0=1; break;
        case 2: c->r[4]=obj; rec_dispatch(c, 0x801286F4u); v0=1; break;
        case 3: v0=1; break;
        case 4: c->r[4]=obj; rec_dispatch(c, 0x8003FC78u); v0=1; break;
        case 5: c->r[4]=obj; rec_dispatch(c, 0x80120188u); v0=1; break;
        case 6: c->r[4]=obj; rec_dispatch(c, 0x8003FC8Cu); v0=c->r[2]; break;
        case 7: c->r[4]=obj; rec_dispatch(c, 0x801146E8u); v0=c->r[2]; break;
        default: v0=1; break;                    // v94>=8 -> @6c0 (v0=1)
      }
      if (v94 == 6 || v94 == 7) { if (v0 == 0) return; }   // @6b8 (only the ret-valued blocks gate here)
      c->mem_w8(obj + 4, (uint8_t)v0);
      c->mem_w8(obj + 5, 0);
      c->mem_w8(obj + 0, (uint8_t)v0);
      c->mem_w8(obj + 41, 0);
      return;
    }
    return;                                       // obj[5] other -> @a48
  }

  if (st == 1) {
    uint32_t g870 = c->mem_r8(G + 0);
    if (g870 == 18) { if (c->mem_r8(0x800BFA59u) == 0) return; }
    else if (g870 == 19) { if (c->mem_r8(G + 1) == 19) return; }
    // @720: obj[5] sub-dispatch (jt 0x80015300, 6 entries)
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 < 6) {
      static const uint32_t JT1[6] = { 0x8003FD10u, 0x8003FED8u, 0x8003FFCCu, 0x8004022Cu, 0x80040390u, 0x80114934u };
      c->r[4] = obj; rec_dispatch(c, JT1[s5]);
    }
    // @7a8: obj[94] sub-dispatch (jt 0x80015318, 8 entries)
    uint32_t v94 = c->mem_r8(obj + 94);
    int go888 = 0, go8c0 = 0;                      // selected tail block
    if (v94 < 8) {
      if (v94 == 7) { c->r[4] = obj; rec_dispatch(c, 0x8012B118u); }   // @7d8 prefix
      if (v94 == 2) go888 = 1;
      else if (v94 == 5) go8c0 = 1;
      // else (0,1,3,4,6,7) -> the @7e0 common block
    } else {
      // v94 >= 8 -> @8c8
      c->mem_w8(obj + 41, 0);
      return;
    }
    if (go888) {
      // @888
      uint32_t p = c->mem_r32(obj + 16);
      uint32_t v0 = c->mem_r8(p + 1);
      c->mem_w8(obj + 1, (uint8_t)v0);
      if ((v0 & 0xff) == 0) { c->mem_w8(obj + 41, 0); return; }        // @8c8
      c->r[4]=obj; rec_dispatch(c, 0x8012866Cu);
      eng(c).cull.enqueueQueueA(obj);              // FUN_80077E7C (native)
      c->mem_w8(obj + 41, 0);
      return;
    }
    if (go8c0) {
      // @8c0
      c->r[4]=obj; rec_dispatch(c, 0x801201E0u);
      c->mem_w8(obj + 41, 0);                       // fall to @8c8
      return;
    }
    // @7e0 common block
    if (c->mem_r8(0x800BF816u) != 0
        && c->mem_r8(0x800BF817u) == (uint32_t)(uint16_t)c->mem_r16s(obj + 106)) {
      if (c->mem_r8(obj + 40) & 0x80) {
        c->mem_w8(obj + 1, 1);
        eng(c).cull.enqueueQueueA(obj);              // FUN_80077E7C (native)
        // @878
        c->r[4]=obj; eng(c).graphicsBind.renderUpdate();
        c->mem_w8(obj + 41, 0);
        return;
      }
      // (obj[40]&0x80)==0 -> @8c8
      c->mem_w8(obj + 41, 0);
      return;
    }
    // @834 (g816==0, or obj[817]!=obj[106])
    if (c->mem_r8(obj + 40) & 0x80) { c->mem_w8(obj + 41, 0); return; }   // @8c8
    {
      uint32_t v0;
      if (c->mem_r8(G + 0) == 8) { c->r[4]=obj; rec_dispatch(c, 0x8012E168u); v0=c->r[2]; }
      else                        { v0 = Actor(c, obj).boundsCull(); }  // FUN_8007778C (native)
      if (v0 == 0) { c->mem_w8(obj + 41, 0); return; }                    // @8c8
      // @878
      c->r[4]=obj; eng(c).graphicsBind.renderUpdate();
      c->mem_w8(obj + 41, 0);
      return;
    }
  }

  if (st == 2) {
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 < 5) {
      // jt 0x80015338: [0]/[4]=@964, [1]=@904, [2]=@94c, [3]=@95c
      if (s5 == 1) {
        // @904
        if (c->mem_r8(obj + 3) == 0 && c->mem_r8(0x800BFAD1u) == 0) eng(c).sceneEvents.arm(56);   // FUN_80040B48 (native)
        // @92c
        if (c->mem_r8(obj + 94) == 2) {
          uint32_t v1b = c->mem_r32(obj + 16);
          c->mem_w8(v1b + 94, 1);
        }
        // fall to @964
      } else if (s5 == 2) {
        c->r[4]=obj; rec_dispatch(c, 0x8003FE00u);    // @94c -> @964
      } else if (s5 == 3) {
        c->r[4]=obj; rec_dispatch(c, 0x8003FED8u);    // @95c -> @964
      }
      // s5==0 or 4 -> @964 directly
    }
    // @964
    if (c->mem_r8(obj + 94) == 2) {
      uint32_t p = c->mem_r32(obj + 16);
      uint32_t v0 = c->mem_r8(p + 1);
      c->mem_w8(obj + 1, (uint8_t)v0);
      c->r[4]=obj; rec_dispatch(c, 0x8012866Cu);
      eng(c).cull.enqueueQueueA(obj);              // FUN_80077E7C (native)
      return;
    }
    // @99c: mirror of state-1 @7e0..tail (global checks + cull/transform), with obj fields
    if (c->mem_r8(0x800BF816u) != 0
        && c->mem_r8(0x800BF817u) == (uint32_t)(uint16_t)c->mem_r16s(obj + 106)) {
      if (c->mem_r8(obj + 40) & 0x80) {
        c->mem_w8(obj + 1, 1);
        eng(c).cull.enqueueQueueA(obj);              // FUN_80077E7C (native)
        // @a30
        c->r[4]=obj; eng(c).graphicsBind.renderUpdate();
        return;
      }
      return;                                         // (obj[40]&0x80)==0 -> @a48
    }
    // @9ec
    if (c->mem_r8(obj + 40) & 0x80) return;           // @a48
    {
      uint32_t v0;
      if (c->mem_r8(G + 0) == 8) { c->r[4]=obj; rec_dispatch(c, 0x8012E168u); v0=c->r[2]; }
      else                        { v0 = Actor(c, obj).boundsCull(); }  // FUN_8007778C (native)
      if (v0 == 0) return;                            // @a48
      // @a30
      c->r[4]=obj; eng(c).graphicsBind.renderUpdate();
      return;
    }
  }

  // st other (>3) -> @a48
}

// FUN_8003FD10 — per-object OSCILLATE / FRAME-TOGGLE sub-behavior (one of sm40558 STATE-1's obj[5] jump-table
// handlers JT1[0], reached ~thousands×/run from the hot active-behavior path). a0 = obj. NO GTE, NO render
// packets — pure object/scratchpad memory ops + ONE dispatched callee (0x8009A450 = ov_rand, owned). A
// 3-way micro state-machine on the phase byte obj[6]:
//   obj[6]==0 (@fd40): if obj[43]==0 return; else obj[6]=1, obj[43]=0, obj[64](sh)=16, return.
//   obj[6]==1 (@fd64): if obj[43]!=0 { obj[43]=0; obj[64](sh)=16; }  @fd7c: v0=obj[64](lhu); v0--; obj[64]=v0;
//     if (int16)v0 == -1 obj[6] += -1 (i.e. obj[6]--);  @fdb0: r = ((u16*)0x1F80017C & 1); node=*(obj+0xC0);
//     node[2](sh) = r*6; rr = ov_rand(); node[0](sh) = ((rr&3)-2)*6; return.
//   obj[6] other (@fdf0): return.
// GOTCHAs: (1) the `sh v1,2(node)` at 0x8003fdd0 is in the ov_rand jal DELAY SLOT — node and v1 (=r*6) are
//   computed BEFORE the call, the store happens with the pre-call values (node loaded @0x8003fdc4). (2) the
//   obj[6]-- at @fdac uses v1=-1 added to obj[6] (only on the v0==-1 branch). (3) node[2]/[0] are halfword
//   stores of v0*6 == (v0*3)<<1. `fd10` gate = full RAM+scratchpad A/B vs rec_super_call (same family
//   rationale as sm40558: the dispatched ov_rand runs in BOTH passes + this fn's 24-byte frame is dead below
//   entry sp on return -> exclude [sp-0x800, sp)).
static void osc_fd10(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t p6 = c->mem_r8(obj + 6);
  if (p6 == 0) {                                  // @fd40
    if (c->mem_r8(obj + 43) == 0) return;         // @fdf0
    c->mem_w8 (obj + 6, 1);
    c->mem_w16(obj + 64, 16);
    c->mem_w8 (obj + 43, 0);
    return;
  }
  if (p6 != 1) return;                            // @fdf0
  // @fd64
  if (c->mem_r8(obj + 43) != 0) {
    c->mem_w8 (obj + 43, 0);
    c->mem_w16(obj + 64, 16);
  }
  // @fd7c
  uint16_t cnt = c->mem_r16(obj + 64);
  cnt = (uint16_t)(cnt - 1);
  c->mem_w16(obj + 64, cnt);
  if ((int16_t)cnt == -1) {                       // @fda4 (obj[6] += -1)
    c->mem_w8(obj + 6, (uint8_t)(c->mem_r8(obj + 6) - 1));
  }
  // @fdb0
  uint32_t r = c->mem_r16(0x1F80017Cu) & 1u;      // scratchpad halfword & 1
  uint32_t node = c->mem_r32(obj + 0xC0);
  c->mem_w16(node + 2, (uint16_t)(r * 6u));       // sh in the ov_rand delay slot (pre-call node/value)
  uint32_t rr = (uint32_t)rngOf(c).next() & 3u;   // FUN_8009A450 -> native class Rng
  uint32_t v0 = (uint32_t)((int32_t)rr - 2);
  c->mem_w16(node + 0, (uint16_t)(v0 * 6u));
}

// FUN_8007a904 — the engine's PER-FRAME ENTITY-LIST WALK (the native entity manager / object driver).
// Each frame the engine walks the two live object lists and runs every node's handler. This is the
// top of the per-object spine: the driver that touches every active game object, so owning it puts the
// engine — not the recompiled body — in charge of iterating the world's objects (the foundation for
// PC-owned per-object render classification, issue #4). Pure list traversal; the per-type handlers stay
// reachable by address via rec_dispatch (each honors its own owned override identically).
//
// RE'd verbatim from disas 0x8007a904 (two identical loops, list0 head 0x800FB168 then list1 head
// 0x800F2624 — the SAME (head) vars the spawn primitive links into, spawn.cpp LIST_HEAD[0]/[1]):
//   for (n = *head; n != 0; n = next) {
//     next        = u32[n + 0x24];   // NEXT captured BEFORE the handler runs (a handler may relink/free n;
//                                    //   `next` is a callee-saved reg in the recomp body, so it survives)
//     handler     = u32[n + 0x1C];   // per-type update/render fn pointer
//     u8[n + 1]   = 0;               // clear the per-frame render flag (jalr delay slot — before the call)
//     handler(n);                    // a0 = n
//   }
// NB only TWO lists are walked here (list0 then list1); the third pool/list 0x800F2738 is not driven by
// this function. `walkverify` gate = full main-RAM + scratchpad A/B vs rec_super_call(0x8007a904).
static void entity_walk_7a904(Core* c) {
  static const uint32_t HEAD[2] = { 0x800FB168u, 0x800F2624u };
  for (int L = 0; L < 2; L++) {
    uint32_t n = c->mem_r32(HEAD[L]);
    while (n) {
      uint32_t next    = c->mem_r32(n + 0x24);   // capture next FIRST (handler may unlink/free n)
      uint32_t handler = c->mem_r32(n + 0x1C);
      c->mem_w8(n + 1, 0);                        // clear per-frame render flag (delay slot of the call)
      c->r[4] = n;                                // a0 = node
      rec_dispatch(c, handler);                   // run the per-type handler (stays PSX / owned override)
      n = next;
    }
  }
}
