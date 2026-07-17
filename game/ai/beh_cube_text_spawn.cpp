// game/ai/beh_cube_text_spawn.cpp — PC-native per-object BEHAVIOR handler FUN_8003AD48.
//
// A RESIDENT MAIN.EXE per-object behavior (range 0x8003AD48..0x8003B04C; `jr ra` at 0x8003b04c). The
// "cube letters" text actor (~x142/field-frame on seaside): measures a string and spawns one record per
// glyph. node[4] = outer state, node[3] = variant.
//
//   STATE 0 (init @0x8003ada8): pick the string — node[3]==2 -> "Clear" (PTR @0x800a3a8c), else the
//     table entry mem32(0x800a33cc + node[0x60]*12). Measure it with FUN_80073750 -> node[8] (len, +1 for
//     Clear). uVar7 = 0x16 (Clear) / 0xf (table). Table branch: if (len&0xff)>=33 -> overflow (two
//     FUN_8009a730 logs, node[4]=2, return). Then if mem16(0x800ed098) < node[8] -> node[4]=2, return;
//     else node[9]=node[8], node[0xb]=4, node[0xd]=0, node[4]++ (->1), node[0x3c]=*PTR(0x800ecf58), and
//     spawn node[8] glyph records: each FUN_8007aae8() -> node[s0+0xc0], zero rec[0..0xc], rec[6]=0xffff,
//     FUN_80051b04(rec,1,uVar7), rec[0x3e]=0, rec[0x3f]=index. Seed node[0x2e]/[0x32]/[0x36]/[0x54..0x58];
//     node[0x5e] + node[0x36] adjust by node[3]==2.
//   STATE 1 (tick @0x8003af88): node[3]: 0 -> FUN_8003a790, 1 -> FUN_8003a9a0, 2 -> FUN_8003abe4 (other:
//     none); then node[1]=1; FUN_800517f8(node).
//   STATE 2 (@0x8003b000): node[4]=3; mem8(0x800bf849)--; mem8(0x800ed06c)--.
//   STATE 3 (@0x8003b030): FUN_8007a624(node).
//
// CONTROL FLOW + every node/global WRITE owned native; every sub-behavior CALL stays a pure-PSX leaf via
// rec_dispatch. RE'd 1:1 from disas 0x8003AD48 (Ghidra decomp scratch/decomp/field2/8003ad48.c
// cross-checked). GOTCHA (record-alloc loop): FUN_8007aae8 carries a0 implicitly — its a0/a1 is the
// leftover from the prior rec_dispatch (FUN_80073750 on the first iter, FUN_80051b04 thereafter, which
// leaves a0=rec); we therefore must NOT write c->r[4] before each FUN_8007aae8, exactly as the recomp
// doesn't. Byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::cullWrap77acc / installSceneRecord
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "ui/font.h"   // Font::measureLineWidth (FUN_80073750)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8003AD48u;

// string-table entry pointer: mem32(0x800a33c8 + (node[0x60]*3 << 2) + 4)   (node[0x60] is signed lh)
static inline uint32_t tbl_strp(Core* c, uint32_t nd) {
  int32_t i = c->mem_r16s(nd + 0x60);
  uint32_t off = (uint32_t)(i * 3) << 2;
  return c->mem_r32(0x800A33C8u + off + 4);
}

}  // namespace

void beh_cube_text_spawn(Core* c) {
  const uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    // ---------- STATE 1 (tick) ----------
    uint8_t type = c->mem_r8(nd + 3);
    if (type == 1)      { c->r[4] = nd; rec_dispatch(c, 0x8003A9A0u); }
    else if (type == 0) { c->r[4] = nd; rec_dispatch(c, 0x8003A790u); }
    else if (type == 2) { c->r[4] = nd; rec_dispatch(c, 0x8003ABE4u); }
    c->mem_w8(nd + 1, 1);
    c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                 // FUN_800517f8
    return;
  }
  if (st >= 2) {
    if (st == 2) {                                              // STATE 2
      c->mem_w8(nd + 4, 3);
      c->mem_w8(0x800BF849u, (uint8_t)(c->mem_r8(0x800BF849u) - 1));
      c->mem_w8(0x800ED06Cu, (uint8_t)(c->mem_r8(0x800ED06Cu) - 1));
      return;
    }
    if (st == 3) { eng(c).spawn.despawn(nd); }  // STATE 3
    return;
  }
  if (st != 0) return;

  // ---------- STATE 0 (init) ----------
  uint32_t uVar7;
  uint8_t type = c->mem_r8(nd + 3);
  if (type == 2) {
    // "Clear" branch
    uint32_t len = (uint32_t)Font::measureLineWidth(c, c->mem_r32(0x800A3A8Cu));  // FUN_80073750(PTR_s_Clear)
    uVar7 = 0x16;
    c->mem_w8(nd + 8, (uint8_t)(len + 1));
  } else {
    // table branch
    uVar7 = 0xf;
    uint32_t len = (uint32_t)Font::measureLineWidth(c, tbl_strp(c, nd));         // FUN_80073750(table str)
    c->mem_w8(nd + 8, (uint8_t)len);
    if ((len & 0xff) >= 33) {
      // overflow: log + bail to state 2
      c->r[4] = 0x80014A54u; rec_dispatch(c, 0x8009A730u);             // s_cube_moji_over_flow
      c->r[4] = 0x80014A6Cu; c->r[5] = tbl_strp(c, nd); rec_dispatch(c, 0x8009A730u);
      c->mem_w8(nd + 4, 2);
      return;
    }
  }

  // common @0x8003ae44
  if (c->mem_r16s(0x800ED098u) < (int)(uint8_t)c->mem_r8(nd + 8)) {
    c->mem_w8(nd + 4, 2);
    return;
  }
  c->mem_w8(nd + 9, c->mem_r8(nd + 8));
  c->mem_w8(nd + 0x0b, 4);
  c->mem_w8(nd + 0x0d, 0);
  c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));          // node[4] 0 -> 1
  c->mem_w32(nd + 0x3c, c->mem_r32(0x800ECF58u));

  // ---- glyph record-alloc loop ----
  uint8_t n = c->mem_r8(nd + 8);
  if (n != 0) {
    uint32_t s0 = nd; int i6 = 0;
    do {
      eng(c).graphicsBind.recordAlloc();                            // FUN_8007aae8() — DO NOT set a0 (leftover)
      uint32_t rec = c->r[2];
      c->mem_w32(s0 + 0xc0, rec);
      c->mem_w16(rec + 6, 0xffff);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 0, 0);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 2, 0);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 4, 0);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 8, 0);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 0xa, 0);
      c->mem_w16(c->mem_r32(s0 + 0xc0) + 0xc, 0);
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(s0 + 0xc0), 1, uVar7);   // FUN_80051B04 (native)
      c->mem_w8(c->mem_r32(s0 + 0xc0) + 0x3e, 0);
      c->mem_w8(c->mem_r32(s0 + 0xc0) + 0x3f, (uint8_t)i6);
      i6++;
      s0 += 4;
    } while (i6 < (int)(uint8_t)c->mem_r8(nd + 8));
  }

  // ---- tail @0x8003af30 ----
  uint16_t sVar1 = c->mem_r16(0x801003F8u);
  c->mem_w16(nd + 0x2e, 0);
  c->mem_w16(nd + 0x32, 0xffc0);
  c->mem_w16(nd + 0x54, 0);
  c->mem_w16(nd + 0x56, 0);
  c->mem_w16(nd + 0x58, 0);
  c->mem_w16(nd + 0x36, (uint16_t)(sVar1 + 8));
  if (c->mem_r8(nd + 3) != 2) {
    c->mem_w8(nd + 0x5e, 0);
    c->mem_w16(nd + 0x36, (uint16_t)(sVar1 + c->mem_r16(nd + 0x36) + 8));
    return;
  }
  c->mem_w8(nd + 0x5e, 1);
}
