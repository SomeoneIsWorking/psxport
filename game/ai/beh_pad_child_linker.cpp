// game/ai/beh_pad_child_linker.cpp — PC-native per-object BEHAVIOR handler FUN_8006F2D0.
//
// A RESIDENT MAIN.EXE per-object behavior routine (prologue 0x8006F2D0; `jr ra` at 0x8006F9D0). The
// hottest still-PSX resident handler (~x777/field-frame, ~450 instrs). Same ownership model as the
// siblings (the FUN_8004ce14 handler / the FUN_80071a3c handler / …): a state machine on the node's state byte node[4].
//
//   STATE 2 : node[4] = 3.                STATE 3 : FUN_8007A624(node).      default(>3): nothing.
//   STATE 0 : init record-list — node[8]=2, node[1]=0, node[0x2a]=0; if (*(s16*)0x800ED098 < 2)
//             node[4]=3; else node[9]=2, node[4]++, node[0xb]=15, node[0xd]=0, node[0x54/56/58]=0,
//             s4=*(u32*)0x800ECF5C, then for i in 0..node[8]: rec = FUN_8007AAE8(); node[0xC0+i*4]=rec;
//             rec[6]=0xFFFF; rec[0..4]=table 0x800A4BA8[i].{0,2,4}; rec[8/10/12]=0; idx=table[i].6;
//             rec[0x40]=s4 + *(u32*)(s4+4+idx*4).  Then node[0x10/0x14]=0, node[0x46]=255.
//   STATE 1 : if (0x1F800137!=0 || 0x800BF80D!=0) kill-link node[0x14] & node[0x10] (if pointed
//             state<2 set it 3, clear ptr), FUN_8006F04C(node); else read input area block at
//             0x800E7E80, copy fields to node, branch on pad 0x1F80018E / 0x1F8001A8 to set node[1]
//             and the rec[0xC0]/[0xC4] anim words (calls FUN_8004766C / FUN_80047B5C), run
//             FUN_8006F138 + FUN_8006EFF4/FUN_8007E038/FUN_8006F02C spawn/despawn of the linked
//             node[0x14]/node[0x10] children, gated by area flags at 0x800BF840 (==71 / &0x40 / &0xF).
//   EVERY exit clears byte 0x800BF840 = 0 (epilogue).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node/global memory WRITES owned
// native; every sub-behavior CALL stays reachable by address via rec_dispatch (leaf, no recursion).
// NO GTE, NO render packets. Transcribed 1:1 as a register machine (locals = guest regs, goto labels =
// guest addresses) so delay-slot subtleties (e.g. the f570 `v1=512` clobber that makes the f574/f5b8
// 0x100/0x20 sub-branches dead) are preserved exactly; the byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. a0/a1 are written into c->r only where the guest writes them, so
// c->r evolves identically to the recomp across the leaf rec_dispatch calls. v0 is NOT reproduced (the
// per-object dispatcher ignores the handler return; the gate compares only RAM+scratchpad).
//
// FUN_8006F04C and FUN_8007E038 (this file's only two callers of each) are now OWNED NATIVE too:
//   Bit::processLinkRequest()               — game/math/mathlib.{h,cpp} — was rec_dispatch(0x8006F04Cu)
//   Spawn::spawnOverlayVariant(id, variant)  — game/world/spawn.{h,cpp} — was rec_dispatch(0x8007E038u)
// PC calls PC directly for both (no rec_dispatch indirection) now that they're native.

#include "core.h"
#include "game_ctx.h"
#include "mathlib.h"    // class Bit (eng(c).bit.testFE48 / setFE34)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "graphics_bind.h"   // ov_record_alloc_g (FUN_8007AAE8)
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8006F2D0u;

}  // namespace

void beh_pad_child_linker(Core* c) {
  uint32_t obj = c->r[4];                       // s2
  uint32_t v0, v1, p;
  uint32_t s0 = 0, s1 = 0, s4 = 0, s5 = 0;      // s-registers (callee-saved → persist as locals)
  int s3 = 0;
  int16_t idx;

  uint8_t st = c->mem_r8(obj + 4);              // node[4] = state
  if (st == 1) goto L460;
  if (st < 2) { if (st == 0) goto L334; goto L9ac; }
  if (st == 2) { c->mem_w8(obj + 4, 3); goto L9ac; }   // STATE 2
  if (st == 3) goto L9a4;                              // STATE 3
  goto L9ac;                                           // default (state > 3)

 L9a4:                                          // STATE 3 — FUN_8007A624(node)
  eng(c).spawn.despawn(obj);
  goto L9ac;

 L334:                                          // STATE 0 — build the record list
  c->mem_w8(obj + 8, 2);
  c->mem_w8(obj + 1, 0);
  c->mem_w8(obj + 0x2a, 0);
  if (!(c->mem_r16s(0x800ED098u) < 2)) goto L360;
  c->mem_w8(obj + 4, 3);
  goto L9ac;
 L360:
  c->mem_w8(obj + 9, 2);
  s4 = c->mem_r32(0x800ECF5Cu);
  c->mem_w8(obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));
  c->mem_w8(obj + 0xb, 15);
  c->mem_w8(obj + 0xd, 0);
  c->mem_w16(obj + 0x54, 0);
  c->mem_w16(obj + 0x56, 0);
  c->mem_w16(obj + 0x58, 0);
  s3 = 0;
  if (c->mem_r8(obj + 8) == 0) goto L44c;
  s5 = s4 + 4;
  s1 = 0x800A4BA8u;
  s0 = obj;
 L3b0:
  eng(c).graphicsBind.recordAlloc();                 // a0 left as-is (matches the guest, which never reloads it)
  s3 += 1;
  v0 = c->r[2];                                 // returned record ptr
  c->mem_w32(s0 + 0xC0, v0);
  c->mem_w16(v0 + 6, 0xFFFF);
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 0, c->mem_r16(s1 + 0));
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 2, c->mem_r16(s1 + 2));
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 4, c->mem_r16(s1 + 4));
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 8, 0);
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 10, 0);
  p = c->mem_r32(s0 + 0xC0); c->mem_w16(p + 12, 0);
  idx = c->mem_r16s(s1 + 6);
  s1 += 8;
  v0 = c->mem_r32(s5 + ((uint32_t)(int32_t)idx << 2));
  p = c->mem_r32(s0 + 0xC0);
  c->mem_w32(p + 0x40, s4 + v0);
  s0 += 4;
  if (s3 < (int)c->mem_r8(obj + 8)) goto L3b0;
 L44c:
  c->mem_w32(obj + 0x10, 0);
  c->mem_w32(obj + 0x14, 0);
  c->mem_w8(obj + 0x46, 255);
  goto L9ac;

 L460:                                          // STATE 1
  if (c->mem_r8(0x1F800137u) != 0) goto L484;
  if (c->mem_r8(0x800BF80Du) == 0) goto L4ec;
  goto L484;
 L484:                                          // frozen/paused: kill the two linked children
  v0 = c->mem_r32(obj + 0x14);
  if (v0 != 0) { if (c->mem_r8(v0 + 4) < 2) c->mem_w8(v0 + 4, 3); c->mem_w32(obj + 0x14, 0); }
  v0 = c->mem_r32(obj + 0x10);
  if (v0 != 0) { if (c->mem_r8(v0 + 4) < 2) c->mem_w8(v0 + 4, 3); c->mem_w32(obj + 0x10, 0); }
  eng(c).bit.processLinkRequest();           // FUN_8006F04C (native)
  goto L9ac;
 L4ec:
  s1 = 0x800E7E80u;                             // input/area block base
  if (c->mem_r8(s1 + 41) == 0) goto L510;
  if (c->mem_r8(s1 + 362) == 0) goto L520;
 L510:
  if (c->mem_r8(s1 + 385) == 0) goto L848;
  goto L520;
 L520:
  c->mem_w16(obj + 0x2e, c->mem_r16(s1 + 46));
  c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16(s1 + 50) - 300));
  c->mem_w16(obj + 0x36, c->mem_r16(s1 + 54));
  c->mem_w8(obj + 0x2a, c->mem_r8(s1 + 42));
  v1 = c->mem_r16(0x1F80018Eu);                 // pad
  if ((v1 & 0x1030) == 0) goto L618;
  if ((v1 & 0x1000) == 0) { v1 = 512; goto L5b8; }
  v1 = 512;                                     // f570 delay clobbers the pad value
  v0 = v1 & 0x100;                              // f574 — always 0 (v1==512): the body below is dead
  p = c->mem_r32(obj + 0xC4);                   // f57c delay slot (always executes)
  if (v0 == 0) goto L590;
  c->mem_w16(p + 12, v1);
  v0 = 2;
  goto L5a0;
 L590:
  v0 = c->mem_r32(obj + 0xC4);
  v1 = 3584;
  c->mem_w16(v0 + 12, v1);
  v0 = 1;
 L5a0:
  c->mem_w8(obj + 1, (uint8_t)v0);
  v1 = c->mem_r32(obj + 0xC4);
  v0 = c->mem_r16(0x1F800194u);
  c->mem_w16(v1 + 10, v0);
  goto L6cc;
 L5b8:
  v0 = v1 & 0x20;                               // v1==512 → 0: body dead, falls to L5ec
  if (v0 == 0) goto L5ec;
  v0 = 2;
  c->mem_w8(obj + 1, (uint8_t)v0);
  v1 = c->mem_r32(obj + 0xC4);
  v0 = c->mem_r16(0x1F80018Cu);
  c->mem_w16(v1 + 10, v0);
  v1 = c->mem_r32(obj + 0xC4);
  v0 = 512;
  c->mem_w16(v1 + 12, v0);
  goto L6cc;
 L5ec:
  v0 = 1;
  c->mem_w8(obj + 1, (uint8_t)v0);
  v0 = c->mem_r16(0x1F80018Cu);
  v1 = c->mem_r32(obj + 0xC4);
  v0 = v0 + 2048;
  c->mem_w16(v1 + 10, v0);
  v1 = c->mem_r32(obj + 0xC4);
  v0 = 3584;
  c->mem_w16(v1 + 12, v0);
  goto L6cc;
 L618:
  v0 = (uint16_t)c->mem_r16s(s1 + 364);
  v1 = 0xC000;
  v0 = v0 & 0xC000;
  if (v0 != v1) goto L6cc;
  c->r[4] = obj; rec_dispatch(c, 0x8004766Cu);
  v1 = c->mem_r16(0x1F8001A8u);                 // pad2
  v0 = v1 & 0x0002;
  if (v0 == 0) goto L684;
  c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x80047B5Cu);
  v0 = c->r[2];
  if (v0 == 0) goto L6c8;
  v1 = c->mem_r32(obj + 0xC4); c->mem_w16(v1 + 10, c->mem_r16(0x1F8001A2u));
  v1 = c->mem_r32(obj + 0xC4); c->mem_w16(v1 + 12, 512);
  c->mem_w8(obj + 1, 2);
  goto L6cc;
 L684:
  v0 = v1 & 0x0004;
  if (v0 == 0) goto L6c8;
  c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x80047B5Cu);
  v0 = c->r[2];
  if (v0 == 0) goto L6c8;
  v1 = c->mem_r32(obj + 0xC4); c->mem_w16(v1 + 10, c->mem_r16(0x1F8001A2u));
  v1 = c->mem_r32(obj + 0xC4); c->mem_w16(v1 + 12, 3584);
  c->mem_w8(obj + 1, 1);
  goto L6cc;
 L6c8:
  c->mem_w8(obj + 1, 0);
 L6cc:
  if (c->mem_r8(obj + 1) == 0) goto L81c;
  v0 = c->mem_r8(s1 + 327);
  if (v0 != 0) goto L708;
  v0 = 3584;                                    // f6e8 delay
  v1 = c->mem_r32(obj + 0xC0); c->mem_w16(v1 + 8, 512);
  v1 = c->mem_r32(obj + 0xC0); c->mem_w16(v1 + 10, c->mem_r16(s1 + 320));
  goto L724;
 L708:
  v1 = c->mem_r32(obj + 0xC0); c->mem_w16(v1 + 8, v0);   // v0 = 3584
  v0 = c->mem_r16(s1 + 320);
  v1 = c->mem_r32(obj + 0xC0);
  v0 = v0 + 2048;
  c->mem_w16(v1 + 10, v0);
 L724:
  v0 = c->mem_r32(obj + 0xC4);
  v1 = (uint16_t)c->mem_r16s(v0 + 12);
  {
    bool ne = (v1 != 3584);
    v1 = 0x800A0000u;                           // f738 delay (always executes)
    if (ne) goto L760;
  }
  v1 = v1 + 19368;                              // 0x800A4BA8
  { uint32_t a = c->mem_r32(obj + 0xC0); c->mem_w16(a + 2, c->mem_r16(v1 + 2)); }
  { uint32_t a = c->mem_r32(obj + 0xC4); c->mem_w16(a + 2, c->mem_r16(v1 + 10)); }
  goto L784;
 L760:
  v1 = v1 + 19368;                              // 0x800A4BA8 (v1 == 0x800A0000 here)
  { uint32_t a = c->mem_r32(obj + 0xC0); c->mem_w16(a + 2, c->mem_r16(v1 + 10)); }
  { uint32_t a = c->mem_r32(obj + 0xC4); c->mem_w16(a + 2, c->mem_r16(v1 + 2)); }
 L784:
  c->r[4] = obj; rec_dispatch(c, 0x8006F138u);
  v0 = eng(c).bit.testFE48((int32_t)c->mem_r8(obj + 1) - 1);   // FUN_8006EFF4 (native)
  if (v0 == 0) goto L7cc;
  v0 = c->mem_r32(obj + 0x14);
  if (v0 == 0) goto L87c;
  if (c->mem_r8(v0 + 4) < 2) c->mem_w8(v0 + 4, 3);
  goto L814;
 L7cc:
  v0 = c->mem_r32(obj + 0x14);
  if (v0 != 0) goto L800;
  v0 = eng(c).spawn.spawnOverlayVariant(
      (uint16_t)(uint32_t)((int32_t)c->mem_r8(obj + 1) - 1), 0);   // FUN_8007E038 (native)
  c->mem_w32(obj + 0x14, v0);
  eng(c).bit.setFE34((int32_t)c->mem_r8(obj + 1) - 1);         // FUN_8006F02C (native)
  goto L87c;
 L800:
  if (c->mem_r8(v0 + 4) < 2) goto L87c;
 L814:
  c->mem_w32(obj + 0x14, 0);
  goto L87c;
 L81c:
  v0 = c->mem_r32(obj + 0x14);
  if (v0 == 0) goto L87c;
  if (c->mem_r8(v0 + 4) < 2) c->mem_w8(v0 + 4, 3);
  goto L814;
 L848:
  v0 = c->mem_r32(obj + 0x14);
  if (v0 == 0) goto L878;
  if (c->mem_r8(v0 + 4) < 2) c->mem_w8(v0 + 4, 3);
  c->mem_w32(obj + 0x14, 0);
 L878:
  c->mem_w8(obj + 1, 0);
 L87c:
  eng(c).bit.processLinkRequest();           // FUN_8006F04C (native)
  v0 = c->mem_r8(s1 + 41);
  if (v0 != 0) { v1 = 1; goto L8ac; }
  v1 = c->mem_r8(0x800BF840u);
  if (v1 != 71) { v1 = 0; goto L8ac; }
  v1 = 1;
 L8ac:
  if (c->mem_r8(obj + 1) != 0) goto L970;
  if (v1 == 0) goto L970;
  if (c->mem_r8(s1 + 362) != 0) goto L970;
  { uint32_t a = c->mem_r8(0x800BF840u);
    if ((a & 0x40) == 0) goto L970;
    s0 = a & 0x0F; }
  v0 = eng(c).bit.testFE48((int32_t)s0);                        // FUN_8006EFF4 (native)
  if (v0 != 0) goto L970;
  if ((c->mem_r8(s1 + 97) & 1) != 0) goto L970;
  v0 = c->mem_r32(obj + 0x10);
  if (v0 != 0) goto L93c;
  v0 = eng(c).spawn.spawnOverlayVariant((uint16_t)s0, 0);   // FUN_8007E038 (native)
  c->mem_w32(obj + 0x10, v0);
  eng(c).bit.setFE34((int32_t)s0);                              // FUN_8006F02C (native)
  c->mem_w8(obj + 0x46, (uint8_t)s0);
  goto L9ac;
 L93c:
  v1 = v0;                                      // node[0x10] child ptr
  if (c->mem_r8(v1 + 4) < 2) goto L958;
 L950:
  c->mem_w32(obj + 0x10, 0);
  goto L9ac;
 L958:
  if (c->mem_r8(obj + 0x46) == s0) goto L9ac;   // same child id → keep it
  c->mem_w8(v1 + 4, 3);
  goto L950;
 L970:
  v0 = c->mem_r32(obj + 0x10);
  if (v0 == 0) goto L9ac;
  v1 = v0;
  if (c->mem_r8(v1 + 4) < 2) c->mem_w8(v1 + 4, 3);
  goto L950;

 L9ac:
  c->mem_w8(0x800BF840u, 0);                    // epilogue: every exit clears this area flag
}
