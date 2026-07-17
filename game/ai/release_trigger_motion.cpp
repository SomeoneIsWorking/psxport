// game/ai/release_trigger_motion.cpp — see release_trigger_motion.h for the ownership summary.
//
// RE'd via Ghidra headless (tools/decomp.sh all) on scratch/ram/band12.bin (a free-roam field dump,
// PSXPORT_AUTO_SKIP=1), then cross-checked against tools/disas.py --ram for: the FUN_80123E9C jump
// table (0x80109B18, resolves the lui/addiu pair the decompile hid behind a raw case switch), the
// FUN_80077B38 model-pointer immediate (0x8014C808 — lui 0x8015/addiu -0x37F8), and the
// 0x800BF9CC bitmask width in leaderFollowSync (a byte load — `lbu`, not a 32-bit word; the
// decompiler's `(int)(uint)DAT_800bf9cc` notation was easy to misread as a word).
//
// A recurring Ghidra artifact across this cluster: the SAME memory operand shows as `*(int *)` at a
// COMPARE site and `*(short *)` at a nearby VALUE-USE site (e.g. arcSwoopMotion / doubleArcMotion's
// `anchor+0x30`/`anchor+0x34` fields). This is consistent with a 32-bit `lw` feeding both an int
// compare AND a 16-bit `sh` store (the decompiler back-propagates the store's 2-byte type onto the
// read). Since truncating a 32-bit sum to 16 bits is identical to truncating a 16-bit sum (addition
// mod 2^16 doesn't care about the extra high bits), the VALUE USES are width-safe either way; the
// COMPARE sites below use the WORD read (mem_r32/mem_r16... as Ghidra typed at that exact site) since
// that is where the extra high bits could change the branch outcome.
#include "release_trigger_motion.h"
#include "game_ctx.h"
#include "core.h"
#include "game.h"
#include "core/engine.h"
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C)
#include "world/spawn.h"      // Spawn::spawnAndInit (FUN_8003116C)
#include "world/graphics_bind.h"  // GraphicsBind::setGeom (FUN_80077B38)
#include "audio/sfx.h"        // Sfx::trigger (FUN_80074590)
#include "math/rng.h"         // Rng::next (FUN_8009A450)
#include "override_registry.h"   // overrides::install — the one native-override registry
#include <cstdint>

extern "C" void rec_dispatch(Core*, uint32_t);

// ---------------------------------------------------------------------------------------------
// FUN_80123E9C — hoverBobCycle: node[5] 5-state timer-driven Y-bob. Called from
// beh_jumptable_release_trigger's jt case 6 sub-case 1, and internally by leaderFollowSync.
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::hoverBobCycle(uint32_t obj) {
  Core* c = core;
  uint8_t st = c->mem_r8(obj + 5);
  switch (st) {
    case 0:
      c->mem_w8(obj + 5, (uint8_t)(st + 1));
      c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 3;
      eng(c).graphicsBind.setGeom();                      // FUN_80077B38
      c->mem_w16(obj + 0x40, 0x5a);
      c->mem_w16(obj + 0x48, 0x10);
      c->mem_w16(obj + 0x4e, 0xfffe);
      c->mem_w16(obj + 0x64, 0);
      c->mem_w16(obj + 0x68, 0);
      [[fallthrough]];
    case 1:
    case 3: {
      int16_t v40 = c->mem_r16s(obj + 0x40);
      c->mem_w16(obj + 0x40, (uint16_t)(v40 - 1));
      if (v40 == 1) {
        uint8_t cur = c->mem_r8(obj + 5);
        c->mem_w8(obj + 5, (uint8_t)(cur + 1));
      }
      break;
    }
    case 2: {
      c->mem_w16(obj + 0x64, (uint16_t)(c->mem_r16(obj + 0x64) + c->mem_r16(obj + 0x48)));
      c->mem_w16(obj + 0x48, (uint16_t)(c->mem_r16(obj + 0x48) + c->mem_r16(obj + 0x4e)));
      int16_t ph = (int16_t)(c->mem_r16(obj + 0x56) + 0x80);
      c->mem_w16(obj + 0x56, (uint16_t)ph);
      if (ph != 0x800) break;
      c->mem_w16(obj + 0x40, 0x5a);
      c->mem_w16(obj + 0x48, 0xfff0);
      uint8_t cur = c->mem_r8(obj + 5);
      c->mem_w16(obj + 0x4e, 2);
      c->mem_w8(obj + 5, (uint8_t)(cur + 1));
      break;
    }
    case 4: {
      c->mem_w16(obj + 0x64, (uint16_t)(c->mem_r16(obj + 0x64) + c->mem_r16(obj + 0x48)));
      int16_t v56 = c->mem_r16s(obj + 0x56);
      c->mem_w16(obj + 0x48, (uint16_t)(c->mem_r16(obj + 0x48) + c->mem_r16(obj + 0x4e)));
      c->mem_w16(obj + 0x56, (uint16_t)(v56 - 0x80));
      if (v56 == 0x80) {
        c->mem_w8(obj + 5, 1);
        c->mem_w16(obj + 0x40, 0x5a);
        c->mem_w16(obj + 0x48, 0x10);
        c->mem_w16(obj + 0x4e, 0xfffe);
      }
      break;
    }
    default: break;
  }
  c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);   // FUN_80077B5C — still-substrate position commit
}

// ---------------------------------------------------------------------------------------------
// FUN_801241BC — leaderFollowSync: node[0x5e]==0 free-runs or snaps into follow-mode off a leader
// node reached via node[0x10]; node[0x5e]==1 hands off to still-substrate FUN_8012400C.
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::leaderFollowSync(uint32_t obj) {
  Core* c = core;
  if (c->mem_r8(obj + 0x5e) == 0) {
    uint32_t leader = c->mem_r32(obj + 0x10);
    uint8_t typeByte = c->mem_r8(leader + 3);
    uint32_t bitmaskByte = c->mem_r8(0x800BF9CCu);        // lbu — NOT a 32-bit word (disas-verified)
    if (((bitmaskByte >> (typeByte & 0x1f)) & 1u) == 0) {
      uint8_t cVar1 = c->mem_r8(leader + 1);
      c->mem_w8(obj + 1, cVar1);
      if (cVar1 != 0) {
        c->r[4] = obj;
        eng(c).cull.enqueueQueueA(obj);                // FUN_80077E7C (return ignored)
        c->r[4] = obj; rec_dispatch(c, 0x80123C94u);       // FUN_80123C94 — still-substrate predicate
        if (c->r[2] == 0) {
          int32_t shifted = (int32_t)((uint32_t)c->mem_r16(leader + 0x84) << 16);
          int16_t half = (int16_t)(((shifted >> 16) - (shifted >> 31)) >> 1);
          c->mem_w16(obj + 0x66, (uint16_t)(-half));
          hoverBobCycle(obj);                              // FUN_80123E9C
          c->r[4] = c->mem_r32(leader + 0xc0);
          c->r[5] = obj + 100;                             // +0x64
          c->r[6] = 0x1F8000C0u;
          rec_dispatch(c, 0x80051D90u);                    // still-substrate scratchpad fill
          uint16_t r0 = c->mem_r16(0x1F8000C0u);
          c->mem_w16(obj + 0x2e, r0);
          uint16_t r2 = c->mem_r16(0x1F8000C2u);
          c->mem_w16(obj + 0x32, r2);
          uint16_t r4 = c->mem_r16(0x1F8000C4u);
          c->mem_w16(obj + 0x36, r4);
          c->mem_w32(obj + 0x30, c->mem_r32(obj + 0x30) +
                                  (uint32_t)(c->mem_r16s(obj + 0x6c) * 0x100));
        }
      }
    } else {
      c->mem_w16(obj + 0x2e, c->mem_r16(leader + 0x2e));
      c->mem_w16(obj + 0x32, c->mem_r16(leader + 0x32));
      uint16_t uVar2 = c->mem_r16(leader + 0x36);
      c->mem_w8(obj + 0x5e, 1);
      c->mem_w8(obj + 5, 0);
      c->mem_w8(obj + 6, 0);
      c->mem_w8(obj + 0, 1);
      c->mem_w16(obj + 0x36, uVar2);
    }
  } else if (c->mem_r8(obj + 0x5e) == 1) {
    c->r[4] = obj; rec_dispatch(c, 0x8012400Cu);           // FUN_8012400C — still-substrate
  }
}

// ---------------------------------------------------------------------------------------------
// FUN_801244E8 — driftReposition(obj, variant): node[5] 2-state re-seed/idle drift. `variant`
// selects the state-0 spawn-point source (0 = camera-relative default, else per-type table).
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::driftReposition(uint32_t obj, uint32_t variant) {
  Core* c = core;
  static constexpr uint32_t TBL = 0x801498B0u;     // 3-entry (X,Y,Z) i16 table, stride 6B
  uint32_t anchor = c->mem_r32(obj + 0x10);
  if (c->mem_r8(obj + 5) == 0) {
    c->mem_w8(obj + 5, 1);
    if (variant == 0) {
      c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(anchor + 0x2c) - 0x14));
      c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(anchor + 0x30) - 200));
      c->mem_w16(obj + 0x36, c->mem_r16(anchor + 0x34));
    } else {
      int32_t off = (c->mem_r16s(obj + 0x60) - 2) * 6;
      c->mem_w16(obj + 0x2e, c->mem_r16(TBL + (uint32_t)off));
      c->mem_w16(obj + 0x32, c->mem_r16(TBL + (uint32_t)off + 2));
      c->mem_w16(obj + 0x36, c->mem_r16(TBL + (uint32_t)off + 4));
    }
    c->mem_w8(obj + 0xb, 0x13);
    c->mem_w16(obj + 0x54, 0x100);
    c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 3;
    eng(c).graphicsBind.setGeom();                       // FUN_80077B38
  } else if (c->mem_r8(obj + 5) == 1) {
    if (Actor(c, obj).boundsCull() != 0) {                  // FUN_8007778C
      uint32_t rec98 = obj + 0x98;
      c->r[4] = rec98; rec_dispatch(c, 0x80051794u);        // still-substrate
      c->r[4] = obj + 0x54; c->r[5] = rec98; rec_dispatch(c, 0x800847F0u);   // still-substrate
      c->r[4] = 0x1F8000F8u; c->r[5] = rec98; rec_dispatch(c, 0x80084360u);  // still-substrate
      c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);          // still-substrate
    }
    if (c->mem_r8(0x800BF9DDu) == 0xe) {
      uint32_t uVar4 = (uint32_t)rngOf(c).next();             // FUN_8009A450 (draw 1)
      if ((uVar4 & 0x3f) == 0) {
        eng(c).spawn.spawnAndInit(0x107, obj + 0x2c, 0xFFFFFFF6u);   // FUN_8003116C
      }
      uint32_t uVar2 = (uint32_t)rngOf(c).next();              // FUN_8009A450 (draw 2)
      c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(anchor + 0x2c) + (int32_t)((uVar2 & 3) - 1) * 0x28));
      c->mem_w16(obj + 0x32, c->mem_r16(anchor + 0x30));
      uint16_t uVar1 = c->mem_r16(anchor + 0x34);
      c->mem_w8(obj + 0x5e, 1);
      c->mem_w8(obj + 5, 0);
      c->mem_w8(obj + 6, 0);
      c->mem_w16(obj + 0x36, uVar1);
    } else if (c->mem_r8(0x800BF9DDu) < 0xe) {
      c->r[4] = obj; rec_dispatch(c, 0x80124328u);           // FUN_80124328 — still-substrate
    }
  }
}

// ---------------------------------------------------------------------------------------------
// FUN_801246B4 — arcSwoopMotion: node[5] 4-state swoop (arm -> resume-wait -> velocity ramp x2).
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::arcSwoopMotion(uint32_t obj) {
  Core* c = core;
  static constexpr uint32_t TBL = 0x80109B44u;    // 5-entry i16 table, index = node[0x60] (no -2)
  uint32_t anchor = c->mem_r32(obj + 0x10);
  if (c->mem_r8(anchor + 1) == 0) return;
  uint8_t st = c->mem_r8(obj + 5);

  if (st == 1) goto resume;
  if (st < 2) {
    if (st != 0) return;
    c->mem_w8(obj + 5, 1);
    c->mem_w8(obj + 0, 1);
    c->mem_w8(obj + 0xb, 0x10);
    c->mem_w16(obj + 0x5a, 0);
    c->mem_w8(obj + 0x47, 0);
    c->mem_w8(obj + 0x46, (uint8_t)(c->mem_r8(obj + 0x60) & 3));
    eng(c).spawn.spawnAndInit(0x107, obj + 0x2c, 0xFFFFFFF6u);      // FUN_8003116C
    {
      int32_t idx = c->mem_r16s(obj + 0x60);
      int16_t tv  = c->mem_r16s(TBL + (uint32_t)idx * 2);
      c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(anchor + 0x2c) + tv));
    }
    c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(anchor + 0x30) - 0x20));
    c->mem_w16(obj + 0x36, c->mem_r16(anchor + 0x34));
    c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 4;
    eng(c).graphicsBind.setGeom();                                  // FUN_80077B38
    goto resume;
  }
  if (st == 2) {
    uint8_t sub = (uint8_t)(c->mem_r8(obj + 0x46) & 3);
    c->mem_w8(obj + 5, 3);
    if (sub == 0 || sub == 2) {
      c->mem_w16(obj + 0x4a, 0xe800);
      c->mem_w16(obj + 0x50, 0x180);
      c->mem_w16(obj + 0x4c, 0);
    } else {
      uint16_t v50 = 0xfc00;
      if (sub == 1) v50 = 0x400;
      c->mem_w16(obj + 0x4c, v50);
      c->mem_w16(obj + 0x4a, 0xf000);
      c->mem_w16(obj + 0x50, 0x200);
    }
  } else if (st != 3) {
    return;
  }
  {
    uint32_t vis = Actor(c, obj).boundsCull();                          // FUN_8007778C
    if (vis == 0) return;
    c->mem_w32(obj + 0x34, c->mem_r32(obj + 0x34) + (uint32_t)(c->mem_r16s(obj + 0x4c) * 0x100));
    int32_t leaderZ = (int32_t)c->mem_r32(anchor + 0x34);   // compare needs the word (disas-verified)
    int32_t cur36 = c->mem_r16s(obj + 0x36);
    if (cur36 < leaderZ - 0x40) {
      c->mem_w16(obj + 0x36, (uint16_t)(leaderZ - 0x40));
    } else if (cur36 <= leaderZ + 0x40) {
      goto tail;
    } else {
      c->mem_w16(obj + 0x36, (uint16_t)(leaderZ + 0x40));
    }
  }
tail:
  c->mem_w32(obj + 0x30, c->mem_r32(obj + 0x30) + (uint32_t)(c->mem_r16s(obj + 0x4a) * 0x100));
  c->mem_w16(obj + 0x4a, (uint16_t)(c->mem_r16s(obj + 0x4a) + c->mem_r16s(obj + 0x50)));
  c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);
  if (c->mem_r8(obj + 0x29) == 0) return;
  c->mem_w8(obj + 5, 2);
  c->mem_w8(obj + 0x47, (uint8_t)(1 - c->mem_r8(obj + 0x47)));
  c->mem_w8(obj + 0x46, (uint8_t)(c->mem_r8(obj + 0x46) + 1));
  c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(obj + 0x32) - 8));
  eng(c).sfx.trigger(0x8f, 0, 0);
  return;

resume:
  if (c->mem_r8(0x800BF9DDu) < 0xf) {
    if ((int32_t)c->mem_r32(anchor + 0x30) <= c->mem_r16s(obj + 0x32)) {
      uint32_t r = (uint32_t)rngOf(c).next();
      if ((r & 0xf) == 0) eng(c).spawn.spawnAndInit(8, obj + 0x2c, 0xFFFFFFB0u);
      int32_t idx = c->mem_r16s(obj + 0x60);
      int16_t tv  = c->mem_r16s(TBL + (uint32_t)idx * 2);
      c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(anchor + 0x2c) + tv));
      c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(anchor + 0x30) - 0x20));
      c->mem_w16(obj + 0x36, c->mem_r16(anchor + 0x34));
    }
    c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);
    Actor(c, obj).boundsCull();                                          // FUN_8007778C, return ignored
  } else {
    c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
  }
  return;
}

// ---------------------------------------------------------------------------------------------
// FUN_801249D4 — doubleArcMotion: node[5] 9-state (0..8) two-pass X impulse + velocity ramp.
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::doubleArcMotion(uint32_t obj) {
  Core* c = core;
  static constexpr uint32_t TBL = 0x80109B50u;    // 3-entry i16 table, index = node[0x60]-2
  uint32_t anchor = c->mem_r32(obj + 0x10);
  uint8_t st = c->mem_r8(obj + 5);

  switch (st) {
    case 0:
      if ((int32_t)c->mem_r32(anchor + 0x30) <= c->mem_r16s(obj + 0x32)) {
        int32_t idx = c->mem_r16s(obj + 0x60) - 2;
        uint16_t tv = c->mem_r16(TBL + (uint32_t)idx * 2);
        c->mem_w16(obj + 0x5a, 0);
        c->mem_w8(obj + 0x47, 0);
        c->mem_w8(obj + 5, (uint8_t)(st + 1));
        c->mem_w8(obj + 0, 1);
        c->mem_w8(obj + 0xb, 0x10);
        c->mem_w8(obj + 0x46, (uint8_t)(c->mem_r8(obj + 0x60) & 1));
        c->mem_w16(obj + 0x40, tv);
        c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 4;
        eng(c).graphicsBind.setGeom();                                // FUN_80077B38
      }
      break;
    case 1:
    case 5: {
      c->mem_w16(obj + 0x4a, 0xe800);
      uint8_t cur = c->mem_r8(obj + 5);
      c->mem_w16(obj + 0x50, 0x400);
      c->mem_w8(obj + 5, (uint8_t)(cur + 1));
      break;
    }
    case 2:
    case 4:
    case 6: {
      c->mem_w32(obj + 0x30, c->mem_r32(obj + 0x30) + (uint32_t)(c->mem_r16s(obj + 0x4a) * 0x100));
      c->mem_w16(obj + 0x4a, (uint16_t)(c->mem_r16s(obj + 0x4a) + c->mem_r16s(obj + 0x50)));
      int32_t impulse = (c->mem_r8(obj + 0x46) == 0) ? -0x80000 : 0x80000;
      c->mem_w32(obj + 0x2c, (uint32_t)((int32_t)c->mem_r32(obj + 0x2c) + impulse));
      int32_t leaderY = (int32_t)c->mem_r32(anchor + 0x30);
      if (leaderY <= c->mem_r16s(obj + 0x32)) {
        int16_t v40 = c->mem_r16s(obj + 0x40);
        c->mem_w32(obj + 0x30, (uint32_t)(leaderY << 16));
        c->mem_w16(obj + 0x40, (uint16_t)(v40 - 1));
        uint8_t cur2 = c->mem_r8(obj + 5);
        if (v40 == 1) {
          c->mem_w16(obj + 0x40, 1);
          c->mem_w8(obj + 5, (uint8_t)(cur2 + 1));
        } else {
          c->mem_w8(obj + 5, (uint8_t)(cur2 - 1));
        }
        c->mem_w8(obj + 0x47, (uint8_t)(1 - c->mem_r8(obj + 0x47)));
        eng(c).sfx.trigger(0x8f, 0, 0);
      }
      break;
    }
    case 3:
    case 7: {
      c->mem_w16(obj + 0x4a, 0xe000);
      c->mem_w16(obj + 0x50, 0x200);
      uint8_t cur = c->mem_r8(obj + 5);
      c->mem_w8(obj + 0, 2);
      c->mem_w8(obj + 5, (uint8_t)(cur + 1));
      break;
    }
    case 8: {
      int32_t impulse = (c->mem_r8(obj + 0x46) == 0) ? -0x80000 : 0x80000;
      c->mem_w32(obj + 0x2c, (uint32_t)((int32_t)c->mem_r32(obj + 0x2c) + impulse));
      c->mem_w32(obj + 0x30, c->mem_r32(obj + 0x30) + (uint32_t)(c->mem_r16s(obj + 0x4a) * 0x100));
      c->mem_w16(obj + 0x4a, (uint16_t)(c->mem_r16s(obj + 0x4a) + c->mem_r16s(obj + 0x50)));
      break;
    }
    default: break;
  }

  uint32_t uVar5 = (uint32_t)rngOf(c).next();               // FUN_8009A450
  if ((uVar5 & 0x3f) == 0) {
    eng(c).spawn.spawnAndInit(8, obj + 0x2c, 0xFFFFFFB0u);     // FUN_8003116C
  }
  Actor(c, obj).boundsCull();                              // FUN_8007778C, return ignored
  c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);
  if (c->mem_r8(obj + 5) == 8 && c->mem_r8(obj + 1) == 0) {
    c->mem_w8(obj + 0x5e, 2);
    c->mem_w8(obj + 5, 0);
    c->mem_w8(obj + 6, 0);
  }
}

// ---------------------------------------------------------------------------------------------
// FUN_80124C6C — circleOrbitMotion: node[5] 3-state orbit toward a per-type (X,Y,Z) table entry.
// ---------------------------------------------------------------------------------------------
void ReleaseTriggerMotion::circleOrbitMotion(uint32_t obj) {
  Core* c = core;
  static constexpr uint32_t TBL = 0x80109B7Cu;   // 3-entry-per-type (X,Y,Z) i16 table, stride 6B
  uint8_t bVar6 = (uint8_t)(c->mem_r8(obj + 0x60) - 2);
  uint32_t entryOff = (uint32_t)bVar6 * 6u;
  int16_t tblX = c->mem_r16s(TBL + entryOff);
  int16_t tblY = c->mem_r16s(TBL + entryOff + 2);
  int16_t tblZ = c->mem_r16s(TBL + entryOff + 4);
  uint8_t st = c->mem_r8(obj + 5);

  if (st == 1) {
    c->mem_w8(obj + 5, 2);
    uint8_t n46 = c->mem_r8(obj + 0x46);
    c->mem_w16(obj + 0x48, (uint16_t)((int32_t)n46 * -0x400));
    c->mem_w16(obj + 0x4a, (uint16_t)((int32_t)n46 * 0x800 - 0x1800));
    c->mem_w16(obj + 0x50, (uint16_t)((int32_t)n46 * 0x20 + 0x180));
  } else {
    if (st < 2) {
      if (st != 0) return;
      c->mem_w8(obj + 5, 1);
      c->mem_w8(obj + 0, 1);
      c->mem_w8(obj + 0xb, 0x10);
      c->mem_w16(obj + 0x5a, 0);
      c->mem_w8(obj + 0x46, bVar6);
      c->mem_w8(obj + 0x47, bVar6);
      c->mem_w16(obj + 0x2e, (uint16_t)tblX);
      c->mem_w16(obj + 0x32, (uint16_t)tblY);
      c->mem_w16(obj + 0x36, (uint16_t)tblZ);
      c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 4;
      eng(c).graphicsBind.setGeom();                                 // FUN_80077B38
      return;
    }
    if (st != 2) return;
  }

  if (Actor(c, obj).boundsCull() != 0) {                                // FUN_8007778C
    c->mem_w32(obj + 0x2c, c->mem_r32(obj + 0x2c) + (uint32_t)(c->mem_r16s(obj + 0x48) * 0x100));
    int16_t cur2e = c->mem_r16s(obj + 0x2e);
    if (cur2e < tblX - 0x40) {
      c->mem_w16(obj + 0x2e, (uint16_t)(tblX - 0x40));
    } else if (tblX + 0x40 < cur2e) {
      c->mem_w16(obj + 0x2e, (uint16_t)(tblX + 0x40));
    }
    c->mem_w32(obj + 0x30, c->mem_r32(obj + 0x30) + (uint32_t)(c->mem_r16s(obj + 0x4a) * 0x100));
    c->mem_w16(obj + 0x4a, (uint16_t)(c->mem_r16s(obj + 0x4a) + c->mem_r16s(obj + 0x50)));
    c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);
    if (tblY <= c->mem_r16s(obj + 0x32)) {
      c->mem_w8(obj + 5, 1);
      uint8_t cur46 = c->mem_r8(obj + 0x46);
      c->mem_w8(obj + 0x46, (uint8_t)(1 - cur46));
      c->mem_w8(obj + 0x47, (uint8_t)(1 - cur46));
      c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(obj + 0x32) - 8));
      eng(c).sfx.trigger(0x8f, 0, 0);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Override registry wiring — guest ABI trampolines (a0 in r4, a1 in r5), same idiom as
// PcScheduler::registerOverrides (game/core/pc_scheduler.cpp).
// ---------------------------------------------------------------------------------------------
static void eov_hoverBobCycle(Core* c)    { eng(c).releaseTriggerMotion.hoverBobCycle(c->r[4]); }
static void eov_leaderFollowSync(Core* c) { eng(c).releaseTriggerMotion.leaderFollowSync(c->r[4]); }
static void eov_driftReposition(Core* c)  { eng(c).releaseTriggerMotion.driftReposition(c->r[4], c->r[5]); }
static void eov_arcSwoopMotion(Core* c)   { eng(c).releaseTriggerMotion.arcSwoopMotion(c->r[4]); }
static void eov_doubleArcMotion(Core* c)  { eng(c).releaseTriggerMotion.doubleArcMotion(c->r[4]); }
static void eov_circleOrbitMotion(Core* c){ eng(c).releaseTriggerMotion.circleOrbitMotion(c->r[4]); }

extern void ov_a00_gen_80123E9C(Core*);
extern void ov_a00_gen_801241BC(Core*);
extern void ov_a00_gen_801244E8(Core*);
extern void ov_a00_gen_801246B4(Core*);
extern void ov_a00_gen_801249D4(Core*);
extern void ov_a00_gen_80124C6C(Core*);

void ReleaseTriggerMotion::registerOverrides() {
  using overrides::install;   // rec_dispatch-only (A00 overlay leaves) — setter omitted
  install(0x80123E9Cu, "ReleaseTriggerMotion::hoverBobCycle",    eov_hoverBobCycle,     ov_a00_gen_80123E9C);
  install(0x801241BCu, "ReleaseTriggerMotion::leaderFollowSync", eov_leaderFollowSync,  ov_a00_gen_801241BC);
  install(0x801244E8u, "ReleaseTriggerMotion::driftReposition",  eov_driftReposition,   ov_a00_gen_801244E8);
  install(0x801246B4u, "ReleaseTriggerMotion::arcSwoopMotion",   eov_arcSwoopMotion,    ov_a00_gen_801246B4);
  install(0x801249D4u, "ReleaseTriggerMotion::doubleArcMotion",  eov_doubleArcMotion,   ov_a00_gen_801249D4);
  install(0x80124C6Cu, "ReleaseTriggerMotion::circleOrbitMotion",eov_circleOrbitMotion, ov_a00_gen_80124C6C);
}
