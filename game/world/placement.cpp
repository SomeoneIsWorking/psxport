// game/world/placement.cpp — PC-native field OBJECT-PLACEMENT subsystem. See placement.h.
#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "placement.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
#include "spawn.h"     // class Spawn (eng(c).spawn.dispatch)
void rec_super_call(Core*, uint32_t);

// ================================================================================================
// FUN_80072A78 — the field OBJECT-PLACEMENT DRIVER (top-down: the "object spawn handler").
//
// This is the function the GAME-stage area machine calls (from GAME.BIN @0x80106bf4 / 0x801072a8 /
// 0x801077f0 / 0x80108e14) when a field/area becomes active. It selects the area's PLACEMENT TABLE
// from (area id 0x800BF870, sub-state 0x800BF871), then walks the table's fixed 0x14-byte records,
// spawning one object per record via the owned per-type dispatch FUN_8007A980 and stamping the new
// node's identity / position / facing / behavior-handler from the record. It is THE entry point that
// populates a field with its NPCs/items/scenery. Resident MAIN.EXE leaf (no yield) → ownable by a
// plain override; the only call it makes is the spawn dispatch (owned). It WRITES guest object state
// the still-recomp content reads → content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B).
//
// PLACEMENT RECORD (0x14 bytes, table terminated by a record whose byte[0]==0xff):
//   +0x00 u8   type   (a0 to spawn = type & 0x7f; full byte also stamped to node+0x28)
//   +0x01 u8   class  (a1 to spawn; if class==3 -> a2/list=1, else list=0)
//   +0x02 u16  X      -> node+0x2e        +0x04 u16  Y -> node+0x32     +0x06 u16  Z -> node+0x36
//   +0x08 u8          -> node+0x02        +0x09 u8     -> node+0x03
//   +0x0a s16  facingA(deg) -> node+0x56 (deg->PSX 0..0xfff)   +0x0c s16 facingB(deg) -> node+0x58
//   +0x0e s16  cond   (1 = gated by per-area collected-bitmask 0x800BFE56 bit[area];
//                      2 = gated by the global enable byte 0x800BF873)
//   +0x10 u32  handler fn ptr -> node+0x1c (the per-object update/render routine; content)
//
// TABLE SELECT (special-cased per area; default = PTR table 0x800A4C28[area]):
//   area5,sub1..3 -> 0x8013C1A4 ; area1,sub>=15 -> 0x80134918 ;
//   area6: sub<6 0x801437AC / sub<9 0x80143ACC / else 0x80143AE0 ;
//   area8: sub<9 0x8014304C / sub<16 0x801432B8 / sub<21 0x80143470 / else 0x80143614 ;
//   area0x15: sub 0..4 -> 0x80115004/18/F4/180/1F8 / else 0x80115310 ;
//   default: if (u16@0x800BF870 == 0x704) none; else 0x800A4C28[area] (0 -> none).
static uint32_t place_select_table(Core* c) {
  uint8_t area = c->mem_r8(0x800BF870u);
  uint8_t sub  = c->mem_r8(0x800BF871u);
  if (area == 5  && (uint8_t)(sub - 1) < 3) return 0x8013C1A4u;
  if (area == 1  && sub >= 15)              return 0x80134918u;
  if (area == 6) { if (sub < 6) return 0x801437ACu; if (sub < 9) return 0x80143ACCu; return 0x80143AE0u; }
  if (area == 8) { if (sub < 9) return 0x8014304Cu; if (sub < 16) return 0x801432B8u; if (sub < 21) return 0x80143470u; return 0x80143614u; }
  if (area == 0x15) {
    switch (sub) {
      case 0: return 0x80115004u; case 1: return 0x80115018u; case 2: return 0x801150F4u;
      case 3: return 0x80115180u; case 4: return 0x801151F8u; default: return 0x80115310u;
    }
  }
  if (c->mem_r16(0x800BF870u) == 0x704u) return 0;   // area4/sub7 -> no placement
  return c->mem_r32(0x800A4C28u + (uint32_t)area * 4);
}

// deg->PSX angle (0..0xfff): exact replica of the signed div-by-360 reciprocal at 0x80072d50.
static uint16_t place_deg2ang(int16_t deg) {
  int32_t  v1   = (int32_t)((uint32_t)(int32_t)deg << 12);
  int64_t  prod = (int64_t)v1 * (int64_t)(int32_t)0xB60B60B7u;   // MIPS signed mult
  int32_t  hi   = (int32_t)((uint64_t)prod >> 32);               // mfhi
  int32_t  v0   = (int32_t)((uint32_t)hi + (uint32_t)v1) >> 8;   // addu; sra 8
  uint32_t r    = (uint32_t)v0 - (uint32_t)(v1 >> 31);           // subu (sign correction)
  return (uint16_t)(r & 0xfffu);
}

static void place_objects(Core* c) {
  uint32_t rec = place_select_table(c);
  if (rec == 0 || c->mem_r8(rec) == 0xff) return;
  uint8_t area = c->mem_r8(0x800BF870u);
  do {
    int16_t cond = c->mem_r16s(rec + 0x0e);
    bool skip = false;
    if (cond == 1) {
      uint32_t bits = c->mem_r16(0x800BFE56u);          // u16, zero-extended
      if ((bits >> (area & 0x1f)) & 1u) skip = true;     // already-collected gate
    } else if (cond == 2) {
      if (c->mem_r8(0x800BF873u) != 0) skip = true;      // global-enable gate
    }
    if (!skip) {
      uint8_t cls = c->mem_r8(rec + 1);
      uint32_t node = eng(c).spawn.dispatch(/*cls=*/(uint32_t)(c->mem_r8(rec) & 0x7fu),
        /*type=*/(cls == 3) ? 3u : cls,
        /*list=*/(cls == 3) ? 1u : 0u);
      if (node != 0) {
        c->mem_w8 (node + 0x28, c->mem_r8 (rec));
        c->mem_w32(node + 0x1c, c->mem_r32(rec + 0x10));
        c->mem_w8 (node + 0x02, c->mem_r8 (rec + 8));
        c->mem_w8 (node + 0x03, c->mem_r8 (rec + 9));
        c->mem_w16(node + 0x2e, c->mem_r16(rec + 2));
        c->mem_w16(node + 0x32, c->mem_r16(rec + 4));
        c->mem_w16(node + 0x54, 0);
        c->mem_w16(node + 0x36, c->mem_r16(rec + 6));
        c->mem_w16(node + 0x56, place_deg2ang(c->mem_r16s(rec + 0x0a)));
        c->mem_w16(node + 0x58, place_deg2ang(c->mem_r16s(rec + 0x0c)));
      }
    }
    rec += 0x14;
  } while (c->mem_r8(rec) != 0xff);
}

void Placement::placeAreaObjects() { Core* c = core;
  int s_v = c->game->verify.on("placeverify");
  if (!s_v) { place_objects(c); return; }
  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t area = c->mem_r8(0x800BF870u), sub = c->mem_r8(0x800BF871u);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  place_objects(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80072A78u);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("placeverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[placeverify] MISMATCH area=%u sub=%u ram@%x spad@%x sp=%x\n", area, sub, ro, so, sp);
  } else fprintf(stderr, "[placeverify] match #%ld (area=%u sub=%u)\n", ++ng, area, sub);
}

// FUN_80072DDC — single-object SPAWN-WITH-PARENT helper (the 2nd dominant field-spawn caller found by
// spawntrace). Spawns one object via the owned dispatch FUN_8007A980, then links it to a PARENT object and
// stamps a flag. Resident leaf, no yield. RE'd 1:1 from disas 0x80072ddc.
//   node* spawn_parent(a0=parent, a1=type, a2=class, a3=flag):
//     node = FUN_8007A980(type & 0x7f, (class==3)?3:class&0xff, (class==3)?1:0);
//     if (node) { node[0x28]=type(full byte); node[0x10]=parent; node[2]=flag; }
//     return node;
static uint32_t spawn_with_parent(Core* c) {
  uint32_t parent = c->r[4], type = c->r[5], cls = c->r[6] & 0xffu, flag = c->r[7];
  uint32_t node = eng(c).spawn.dispatch(/*cls=*/type & 0x7fu,
    /*type=*/(cls == 3) ? 3u : cls,
    /*list=*/(cls == 3) ? 1u : 0u);
  if (node != 0) {
    c->mem_w8 (node + 0x28, type & 0xffu);   // full type byte (s0 = original a1, unmasked)
    c->mem_w32(node + 0x10, parent);
    c->mem_w8 (node + 0x02, flag & 0xffu);
  }
  return node;
}
void Placement::spawnWithParent() { Core* c = core;
  int s_v = c->game->verify.on("spawnparentverify");
  if (!s_v) { c->r[2] = spawn_with_parent(c); return; }
  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = spawn_with_parent(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80072DDCu);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("spawnparentverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[spawnparentverify] MISMATCH v0 n=%x o=%x ram@%x spad@%x\n", v0_n, v0_o, ro, so);
  } else if (++ng % 20 == 0) fprintf(stderr, "[spawnparentverify] %ld matches\n", ng);
}
