// game/ai/actor_zoned_attacker.cpp — PC-native bodies for the "zoned attacker" per-object
// sub-behavior cluster: FUN_8014047C / FUN_80140544 / FUN_801409C0 / FUN_80143A00 / FUN_80144928 /
// FUN_80144B50. All six are SUB-BEHAVIOR callees reached exclusively via `rec_dispatch(c, addr)`
// from the already-native FUN_80145230 dispatcher (game/ai/beh_id_compare_motion_dispatch.cpp,
// guest 0x8014xxxx OVERLAY area) — see that file's header comment for the call graph:
//   state 0 (init)      -> FUN_80140544                      -> ActorZonedAttacker::typeInit
//   state 1 tick gate   -> FUN_8014047c (×2 call sites)      -> ActorZonedAttacker::gateCheck
//   state 1 node[3]==1..5 (motion) -> FUN_80144928, then, if it signals "arrived", the heading-
//     bucket -> FUN_801409c0                                  -> approachAndFace, pickAttackByRange
//   state 1 node[3]==0/default     -> FUN_80143a00             -> defaultSubStateMachine
//   state 2 (idle)      -> FUN_80144b50                        -> idleTick
//
// RE SOURCE: Ghidra headless decompile (tools/decomp.sh, project scratch/ghidra/A00,
// scratch/decomp/cluster1.c) for all six bodies; every other FUN_xxxx / func_0x-address referenced
// below stays an un-owned PSX leaf, reached uniformly via `rec_dispatch(c, addr)` (guest ABI: args
// in c->r[4..7], return in c->r[2]) — same discipline as game/object/actor_sm_reward.cpp. Two
// leaves (FUN_800777FC / FUN_800518FC) already have a native PC class (Cull::cullWrapperFlag2 /
// Engine::objMatrixCompose) but are NOT installed in the override registry (only their sole other
// caller invokes them directly) — dispatching to their guest address still reaches the exact same
// byte-identical substrate body, so this file keeps everything uniform via rec_dispatch rather than
// special-casing those two.
//
// Field/global widths below are taken directly from Ghidra's explicit casts in the decompiled C
// (undefined1/char/byte = 1 byte, undefined2/short/ushort = 2 bytes, undefined4/int/uint = 4 bytes);
// several globals (DAT_800e7eaa, DAT_1f800137/800bf89c/800bf809, DAT_1f800160/164, DAT_800e7ffe,
// DAT_8014bf5e) are cross-checked against their already-native reads in
// game/ai/beh_id_compare_motion_dispatch.cpp for width agreement. Field/global MEANING (not just
// width) is not independently understood beyond what the control flow implies — this is a faithful
// structural transcription, not a semantic rebuild (matches the "not independently RE'd" leaves
// convention already used throughout game/ai/ and game/object/).
#include "core.h"
#include "game_ctx.h"
#include "actor_zoned_attacker.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "game.h"
#include "guest_abi.h"  // GuestFrame — guest-stack frame discipline for the override trampolines
#include "spawn.h"   // Spawn::spawnAndInit (FUN_8003116C, already native)
#include <cstdint>

void rec_dispatch(Core*, uint32_t);   // hybrid call: override if wired, else substrate

namespace {

enum { R_A0 = 4, R_A1 = 5, R_A2 = 6, R_A3 = 7, R_V0 = 2 };

// Callee leaf addresses (all un-owned PSX leaves reached via rec_dispatch).
constexpr uint32_t FN_80145C78 = 0x80145C78u;
constexpr uint32_t FN_8009A450 = 0x8009A450u;   // RNG read (same leaf the sibling caller reaches via rngOf(c).next())
constexpr uint32_t FN_800781E0 = 0x800781E0u;   // 2D distance
constexpr uint32_t FN_80078240 = 0x80078240u;   // 3D distance
constexpr uint32_t FN_801402B8 = 0x801402B8u;   // anim/state-cue setter
constexpr uint32_t FN_801406E4 = 0x801406E4u;   // per-tick motion/anim step
constexpr uint32_t FN_80076D68 = 0x80076D68u;   // Animation::step body (reached via leaf dispatch)
constexpr uint32_t FN_800519E0 = 0x800519E0u;   // per-type table install (scratch/decomp/f800519E0.c)
constexpr uint32_t FN_8004766C = 0x8004766Cu;   // grid resolve-in-place
constexpr uint32_t FN_80049674 = 0x80049674u;
constexpr uint32_t FN_800782B0 = 0x800782B0u;
constexpr uint32_t FN_80142788 = 0x80142788u;
constexpr uint32_t FN_801425F0 = 0x801425F0u;
constexpr uint32_t FN_80141AC4 = 0x80141AC4u;
constexpr uint32_t FN_801422B4 = 0x801422B4u;
constexpr uint32_t FN_8014213C = 0x8014213Cu;
constexpr uint32_t FN_80141C20 = 0x80141C20u;
constexpr uint32_t FN_80140AF4 = 0x80140AF4u;
constexpr uint32_t FN_8014243C = 0x8014243Cu;
constexpr uint32_t FN_801436C4 = 0x801436C4u;
constexpr uint32_t FN_801431C4 = 0x801431C4u;
constexpr uint32_t FN_801408AC = 0x801408ACu;
constexpr uint32_t FN_8014103C = 0x8014103Cu;
constexpr uint32_t FN_80141438 = 0x80141438u;
constexpr uint32_t FN_8014181C = 0x8014181Cu;
constexpr uint32_t FN_80142A94 = 0x80142A94u;
constexpr uint32_t FN_80142CF4 = 0x80142CF4u;
constexpr uint32_t FN_80026100 = 0x80026100u;
constexpr uint32_t FN_80074590 = 0x80074590u;   // SFX trigger leaf (not independently RE'd here)
constexpr uint32_t FN_80077E20 = 0x80077E20u;   // palette/color side-effect leaf
constexpr uint32_t FN_80080750 = 0x80080750u;
constexpr uint32_t FN_801280E8 = 0x801280E8u;
constexpr uint32_t FN_80077768 = 0x80077768u;
constexpr uint32_t FN_800777FC = 0x800777FCu;   // Cull::cullWrapperFlag2 body (reached via leaf dispatch)
constexpr uint32_t FN_800518FC = 0x800518FCu;   // Engine::objMatrixCompose body (reached via leaf dispatch)
constexpr uint32_t FN_800495DC = 0x800495DCu;
constexpr uint32_t FN_800315D4 = 0x800315D4u;
constexpr uint32_t FN_8014047C = 0x8014047Cu;   // self: ActorZonedAttacker::gateCheck (via override table)
constexpr uint32_t FN_801409C0 = 0x801409C0u;   // self: ActorZonedAttacker::pickAttackByRange

// Cross-checked-width globals (see file header).
constexpr uint32_t G_800E7EAA = 0x800E7EAAu;    // u8
constexpr uint32_t G_800E7EAC = 0x800E7EACu;    // address only (passed as ptr arg)
constexpr uint32_t G_800ED098 = 0x800ED098u;    // i16
constexpr uint32_t G_800ECFB0 = 0x800ECFB0u;    // u32 (raw register-value copy; see f800519E0.c)
constexpr uint32_t G_8014BE14 = 0x8014BE14u;    // address only (passed as ptr arg)
constexpr uint32_t G_800ECFB4 = 0x800ECFB4u;    // u32 (raw copy into node+0x3c)
constexpr uint32_t G_1F8001A0 = 0x1F8001A0u;    // u16
constexpr uint32_t G_1F8001A2 = 0x1F8001A2u;    // u16
constexpr uint32_t G_1F800160 = 0x1F800160u;    // i16
constexpr uint32_t G_1F800162 = 0x1F800162u;    // i16
constexpr uint32_t G_1F800164 = 0x1F800164u;    // i16
constexpr uint32_t G_800E7FFE = 0x800E7FFEu;    // u16
constexpr uint32_t G_8014BEE4 = 0x8014BEE4u;    // byte table[16]
constexpr uint32_t G_8014BED4 = 0x8014BED4u;    // byte table[16]
constexpr uint32_t G_8014BEF4 = 0x8014BEF4u;    // byte table[16]
constexpr uint32_t G_1F800137 = 0x1F800137u;    // u8
constexpr uint32_t G_800BF89C = 0x800BF89Cu;    // u8
constexpr uint32_t G_800BF809 = 0x800BF809u;    // u8
constexpr uint32_t G_8014BF5E = 0x8014BF5Eu;    // u8 countdown (shared with the caller's own tail)
constexpr uint32_t G_800E7E80 = 0x800E7E80u;    // u8

inline void call2(Core* c, uint32_t node, uint32_t addr, uint32_t a1, uint32_t a2) {
  c->r[R_A0] = node; c->r[R_A1] = a1; c->r[R_A2] = a2;
  rec_dispatch(c, addr);
}
inline void call1(Core* c, uint32_t node, uint32_t addr) {
  c->r[R_A0] = node;
  rec_dispatch(c, addr);
}

// Guest-ABI frame contracts for the three override trampolines that allocate a guest stack frame
// (from `python3 tools/abi_extract.py <addr> --contract`). Each trampoline MUST mirror its substrate
// gen_func's guest-stack frame (alloc + callee-save spills) — MIRROR_VERIFY compares the full guest
// RAM+regs. An earlier draft had bare trampolines (no frame), leaving whatever stale bytes sat in the
// spill slots; fixed by wrapping the body in GuestFrame (2026-07-11, the f389 diverge root-cause
// family — same fix as game/world/spawn.cpp's eov_* trampolines).
static constexpr GuestFrameSpill kSpills_80140544[4] = { {16, 16}, {17, 20}, {18, 24}, {31, 28} };   // frame=32
static constexpr GuestFrameSpill kSpills_8014047C[2] = { {16, 16}, {31, 20} };                       // frame=24
static constexpr GuestFrameSpill kSpills_80144928[4] = { {16, 16}, {17, 20}, {18, 24}, {31, 28} };   // frame=32

}  // namespace

// ActorZonedAttacker::gateCheck(c) — FUN_8014047c(node) -> bool v0. A tick/despawn-gate predicate:
// node[0x66]==0x81 -> "grace window" compare against DAT_800e7eaa; node[0x66]==0x80 -> delegate to
// FUN_80145c78(DAT_800e7eaa, &DAT_800e7eac); else compare a per-type table value (node[0x14]-> +0x48)
// against FUN_80145c78(node[0x2a], node+0x2c), short-circuiting true once DAT_800e7eaa clears two
// thresholds (2 then 0xb).
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::gateCheck(Core* c) {
  GuestFrame<24, 2> frame(c, kSpills_8014047C);
  const uint32_t node = c->r[R_A0];
  const int8_t n66 = (int8_t)c->mem_r8(node + 0x66);
  bool result;
  if (n66 == (int8_t)0x81) {
    int32_t v = (int32_t)c->mem_r8(G_800E7EAA) - 0xc;
    result = (0x10 < v);
  } else if (n66 == (int8_t)0x80) {
    c->r[R_A0] = c->mem_r8(G_800E7EAA);
    c->r[R_A1] = G_800E7EAC;
    rec_dispatch(c, FN_80145C78);
    result = (c->r[R_V0] != 0);
  } else {
    const uint32_t typePtr = c->mem_r32(node + 0x14);
    int32_t iVar2 = (int32_t)c->mem_r32(typePtr + 0x48);
    if (iVar2 == -1) {
      iVar2 = 0;
      if ((int32_t)c->mem_r8(G_800E7EAA) > 2) {
        iVar2 = 1;
        if ((int32_t)c->mem_r8(G_800E7EAA) > 0xb) {
          c->r[R_V0] = 1;
          return;
        }
      }
    }
    c->r[R_A0] = c->mem_r8(node + 0x2a);
    c->r[R_A1] = node + 0x2c;
    rec_dispatch(c, FN_80145C78);
    const int32_t iVar3 = (int32_t)c->r[R_V0];
    result = (iVar2 != iVar3);
  }
  c->r[R_V0] = result ? 1u : 0u;
}

// ActorZonedAttacker::typeInit(c) — FUN_80140544(node). One-shot per-type installer: bails to
// state 3 if DAT_800ed098 hasn't reached readiness (0x12); else installs a per-type table
// (FN_800519E0), seeds a grid-resolve + facing/parity flags, resets counters, and sets default
// range/scroll constants (matches the "per-type init; then -> epilogue" comment in the caller).
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::typeInit(Core* c) {
  GuestFrame<32, 4> frame(c, kSpills_80140544);
  const uint32_t node = c->r[R_A0];
  if (c->mem_r16s(G_800ED098) < 0x12) {
    c->mem_w8(node + 9, 0);
    c->mem_w8(node + 4, 3);
    return;
  }
  c->mem_w8(node + 0xd, 0);
  c->mem_w8(node + 9, 0x12);
  // FN_800519E0 takes 4 args (node, 0x12, a copy of DAT_800ecfb0's raw bits, &DAT_8014be14) —
  // wider than the call1/call2 helpers, so issue it directly.
  c->r[R_A0] = node; c->r[R_A1] = 0x12u; c->r[R_A2] = c->mem_r32(G_800ECFB0); c->r[R_A3] = G_8014BE14;
  rec_dispatch(c, FN_800519E0);
  c->mem_w32(node + 0x3c, c->mem_r32(G_800ECFB4));
  call1(c, node, FN_8004766C);
  if (c->r[R_V0] == 0) {
    c->mem_w8(node + 4, 3);
    return;
  }
  call1(c, node, FN_80049674);
  const uint16_t v1a2 = c->mem_r16(G_1F8001A2);
  const uint16_t v1a0 = c->mem_r16(G_1F8001A0);
  c->mem_w16(node + 0x54, 0);
  c->mem_w16(node + 0x58, v1a2);
  c->mem_w16(node + 0x56, v1a0);
  c->mem_w16(node + 0x60, v1a0);
  c->mem_w8(node + 100, 0);
  const int32_t ix = c->mem_r16s(G_1F800160);
  const int32_t iy = c->mem_r16s(G_1F800164);
  c->mem_w16(node + 0x62, 0);
  const int32_t s60 = c->mem_r16s(node + 0x60);
  c->r[R_A0] = node + 0x2c; c->r[R_A1] = (uint32_t)ix; c->r[R_A2] = (uint32_t)iy;
  rec_dispatch(c, FN_800782B0);
  const int32_t s4 = (int32_t)(int16_t)c->r[R_V0];
  uint16_t v62 = c->mem_r16(node + 0x62);
  const uint32_t val = (uint32_t)(s60 - s4 + 0x400) & 0xfffu;
  v62 = (val < 0x801u) ? (uint16_t)(v62 & 0xfffe) : (uint16_t)(v62 | 1);
  c->mem_w16(node + 0x62, v62);
  c->mem_w8(node + 0x5f, 0);
  c->mem_w8(node + 0x1b, (uint8_t)(c->mem_r8(node + 0x1b) & 0xbf));
  c->mem_w8(node + 0x2b, 0);
  c->mem_w16(node + 4, 1);
  c->mem_w8(node + 0x66, c->mem_r8(node + 3));
  c->mem_w8(node + 0, 1);
  call2(c, node, FN_801402B8, 1u, 0u);
  c->mem_w16(node + 0xbc, 0x1000);
  c->mem_w16(node + 0xba, 0x1000);
  c->mem_w16(node + 0xb8, 0x1000);
  c->mem_w16(node + 0x80, 0x38);
  c->mem_w16(node + 0x82, 0x70);
  c->mem_w16(node + 0x84, 0x8c);
  c->mem_w16(node + 0x4e, 0);
  c->mem_w16(node + 0x50, 0);
  c->mem_w16(node + 0x86, 0xf0);
  c->mem_w16(node + 0x7c, 0);
  c->mem_w16(node + 0x7e, 0);
}

// ActorZonedAttacker::pickAttackByRange(c) — FUN_801409c0(node[, unused a1]) -> byte v0. Rolls the
// RNG, bails to 0 if a "busy" flag (DAT_800e7ffe & 0x8200) is set, buckets the 2D distance-to-target
// into a zone (0=far/1=mid/2=near/-1=busy-or-too-far), then indexes one of three 16-entry byte
// tables by the RNG roll. The caller's own precomputed zone arg is NOT read by this body — it
// recomputes the same zone internally (confirmed: Ghidra's signature for this function has no
// live-in second parameter), so callers passing a stale/rough zone estimate is harmless.
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::pickAttackByRange(Core* c) {
  const uint32_t node = c->r[R_A0];
  rec_dispatch(c, FN_8009A450);
  const uint32_t rngv = c->r[R_V0];
  if (c->mem_r16(G_800E7FFE) & 0x8200) { c->r[R_V0] = 0; return; }
  const int32_t nx = c->mem_r16s(node + 0x2e);
  const int32_t ny = c->mem_r16s(node + 0x36);
  const int32_t tx = c->mem_r16s(G_1F800160);
  const int32_t ty = c->mem_r16s(G_1F800164);
  c->r[R_A0] = (uint32_t)(int32_t)(int16_t)(nx - tx);
  c->r[R_A1] = (uint32_t)(int32_t)(int16_t)(ny - ty);
  rec_dispatch(c, FN_800781E0);
  const int32_t dist = (int32_t)(int16_t)c->r[R_V0];
  int32_t zone;
  if ((c->mem_r16(G_800E7FFE) & 0x8200) == 0 && dist < 0x641) {
    if (dist < 0x44d) { zone = 2; if (dist > 600) zone = 1; }
    else zone = 0;
  } else {
    zone = -1;
  }
  uint32_t table;
  if (zone == 1) {
    table = G_8014BEE4;
  } else if (zone < 2) {
    if (zone != 0) { c->r[R_V0] = 0; return; }
    table = G_8014BED4;
  } else {
    if (zone != 2) { c->r[R_V0] = 0; return; }
    table = G_8014BEF4;
  }
  c->r[R_V0] = c->mem_r8(table + (rngv & 0xf));
}

// ActorZonedAttacker::approachAndFace(c) — FUN_80144928(node) -> v0. A short "close the distance,
// then hold" sub-SM keyed on node[7]: 0/1 seed a per-type anim cue then probe 3D distance
// (FN_80078240) to flip a "close enough" bit; 2 re-probes; 3/4 run a ~40-frame settle countdown
// (node+0x40) after seeding a different anim cue; 5/6 run a ~30-frame countdown. Ends by advancing
// node[0x32] (heading) by 0x10 and stepping FN_801406e4 every call.
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::approachAndFace(Core* c) {
  GuestFrame<32, 4> frame(c, kSpills_80144928);
  const uint32_t node = c->r[R_A0];
  uint32_t uVar4 = 0;
  const uint8_t st = c->mem_r8(node + 7);

  goto label_dispatch;

label_ac8:
  c->mem_w8(node + 7, (uint8_t)(c->mem_r8(node + 7) + 1));
  goto label_caseD_7;

label_caseD_2: {
  const int32_t nx = c->mem_r16s(node + 0x2e);
  const int32_t ny = c->mem_r16s(node + 0x32);
  const int32_t nz = c->mem_r16s(node + 0x36);
  const int32_t tx = c->mem_r16s(G_1F800160);
  const int32_t ty = c->mem_r16s(G_1F800162);
  const int32_t tz = c->mem_r16s(G_1F800164);
  c->r[R_A0] = (uint32_t)(int32_t)(int16_t)(tx - nx);
  c->r[R_A1] = (uint32_t)(int32_t)(int16_t)(ty - ny);
  c->r[R_A2] = (uint32_t)(int32_t)(int16_t)(tz - nz);
  rec_dispatch(c, FN_80078240);
  if (c->r[R_V0] < 0x3c0u) goto label_ac8;
  goto label_caseD_7;
}

label_caseD_7:
  c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
  call1(c, node, FN_801406E4);
  c->r[R_V0] = uVar4;
  return;

label_dispatch:
  switch (st) {
    case 0:
    case 1: {
      uint32_t uVar3;
      switch (c->mem_r8(node + 3)) {
        default: uVar3 = 0x12; break;
        case 2:  uVar3 = 7;    break;
        case 3:
        case 4:  uVar3 = 0x1b; break;
        case 5:  uVar3 = 6;    break;
      }
      call2(c, node, FN_801402B8, uVar3, 0u);
      c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
      call1(c, node, FN_801406E4);
      c->mem_w8(node + 7, 2);
      goto label_caseD_2;
    }
    case 2:
      goto label_caseD_2;
    case 3: {
      uint32_t uVar3;
      switch (c->mem_r8(node + 3)) {
        default: uVar3 = 0x13; break;
        case 2:  uVar3 = 7; uVar4 = 1; break;
        case 3:
        case 4:  uVar3 = 0x1d; break;
        case 5:  uVar3 = 0x34; break;
      }
      call2(c, node, FN_801402B8, uVar3, 8u);
      c->mem_w16(node + 0x40, 0x28);
      c->mem_w8(node + 7, (uint8_t)(c->mem_r8(node + 7) + 1));
      [[fallthrough]];
    }
    case 4: {
      const uint16_t v = (uint16_t)(c->mem_r16(node + 0x40) - 1);
      c->mem_w16(node + 0x40, v);
      if ((int16_t)v > 0) goto label_caseD_7;
      goto label_ac8;
    }
    case 5:
      call2(c, node, FN_801402B8, 7u, 8u);
      c->mem_w16(node + 0x40, 0x1e);
      c->mem_w8(node + 7, (uint8_t)(c->mem_r8(node + 7) + 1));
      [[fallthrough]];
    case 6: {
      const uint16_t v = (uint16_t)(c->mem_r16(node + 0x40) - 1);
      c->mem_w16(node + 0x40, v);
      if ((int16_t)v < 1) uVar4 = 1;
      goto label_caseD_7;
    }
    default:
      goto label_caseD_7;
  }
}

// ActorZonedAttacker::defaultSubStateMachine(c) — FUN_80143a00(node). The node[3]==0/default
// motion sub-behavior: a ~14-way switch on node[5] (attack/idle "move id") that cycles anim cues
// (FN_801402b8), advances heading (node[0x32]), and eventually re-packs node[4..7] with a new
// (state, move-id) pair once a countdown/trigger fires. Every attack-cycle sub-call
// (FUN_8014xxxx below) stays an un-RE'd PSX leaf — only the CONTROL FLOW + node/global writes are
// owned here, matching the "not independently RE'd" convention.
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::defaultSubStateMachine(Core* c) {
  const uint32_t node = c->r[R_A0];
  uint32_t uVar6 = 0;
  uint32_t uVar7 = 0;
  int32_t iVar5 = 0;
  uint8_t sharedBVar2 = 0;   // holds case9/case10's node[6] across the shared LAB_801448a8 jump

  const uint8_t state5 = c->mem_r8(node + 5);
  switch (state5) {
    case 0: {
      uint8_t n6 = c->mem_r8(node + 6);
      if (n6 == 0) { c->mem_w16(node + 6, 1); }
      else if (n6 != 1) { return; }

      call1(c, node, FN_80142788);
      {
        const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
        if (sVar4 == 2) {
          uVar7 = 0x501u;
        } else if (sVar4 < 3) {
          if (sVar4 != 1) return;
          const uint8_t bVar2 = (uint8_t)(c->mem_r8(node + 100) - 1);
          c->mem_w8(node + 100, bVar2);
          uVar7 = 0x201u;
          if ((int32_t)((uint32_t)bVar2 << 24) < 1) {
            c->mem_w32(node + 4, 0x301u);
            rec_dispatch(c, FN_8009A450);
            c->mem_w8(node + 100, (uint8_t)(c->r[R_V0] & 3));
            return;
          }
        } else if (sVar4 == 3) {
          uVar7 = 0x401u;
        } else {
          if (sVar4 != 4) return;
          const int32_t nx = c->mem_r16s(node + 0x2e);
          const int32_t tx = c->mem_r16s(G_1F800160);
          const int32_t nz = c->mem_r16s(node + 0x36);
          const int32_t tz = c->mem_r16s(G_1F800164);
          c->r[R_A0] = (uint32_t)(int32_t)(int16_t)(nx - tx);
          c->r[R_A1] = (uint32_t)(int32_t)(int16_t)(nz - tz);
          rec_dispatch(c, FN_800781E0);
          const int32_t d = (int32_t)(int16_t)c->r[R_V0];
          int32_t zone;
          if ((c->mem_r16(G_800E7FFE) & 0x8200) == 0 && d < 0x641) {
            if (d < 0x44d) zone = (d < 0x259) ? 2 : 1;
            else zone = 0;
          } else zone = -1;
          c->r[R_A0] = node; c->r[R_A1] = (uint32_t)zone;
          rec_dispatch(c, FN_801409C0);
          const int32_t r = (int32_t)(int16_t)c->r[R_V0];
          uVar7 = 0x301u;
          if (r != 0) {
            c->mem_w8(node + 5, (uint8_t)r);
            c->mem_w8(node + 6, 0);
            return;
          }
        }
      }
      goto LAB_80144908;
    }
    case 1: {
      // (case 0 falls through into case 1 in the recompiled body — see case 0 above; this arm
      // is reached both directly on entry with node[5]==1 and via fallthrough from case 0.)
      uint8_t n6 = c->mem_r8(node + 6);
      if (n6 == 0) { c->mem_w16(node + 6, 1); }
      else if (n6 != 1) { return; }
      call1(c, node, FN_801425F0);
      uVar6 = (uint32_t)c->r[R_V0] << 16;
      break;
    }
    case 2: {
      uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 != 1) {
        if (1 < bVar2) {
          uVar6 = 0;
          if (bVar2 != 2) return;
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 0x1au, 8u);
            c->mem_w16(node + 0x4e, 0);
            c->mem_w8(node + 7, 1);
            goto LAB_80143ea0_a;
          } else {
            if (c->mem_r8(node + 7) == 1) goto LAB_80143ea0_a;
            if (c->mem_r8(node + 7) < 0x14) {
              const int8_t cVar3 = (int8_t)(c->mem_r8(node + 7) + 1);
              c->mem_w8(node + 7, (uint8_t)cVar3);
              goto LAB_80143ea0_tail_skip;
            }
            uVar6 = 1;
            goto LAB_80143ea0_tail_skip;
          }
LAB_80143ea0_a: {
            const uint32_t p38 = c->mem_r32(node + 0x38);
            if (c->mem_r16s(p38 + 4) != 0) {
              c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) ^ 1));
              c->mem_w16(node + 0x56, (uint16_t)(c->mem_r16(node + 0x56) + 0x800));
              call2(c, node, FN_801402B8, 7u, 0u);
              c->mem_w8(node + 7, 2);
            }
          }
LAB_80143ea0_tail_skip:
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          break;
        }
        if (bVar2 != 0) return;
        c->mem_w16(node + 6, 1);
      }
      if (c->mem_r8(node + 7) == 0) {
        c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
        call2(c, node, FN_801402B8, 5u, 8u);
        c->mem_w8(node + 7, 0x2d);
        c->mem_w16(node + 0x4e, 0);
      }
      {
        const uint8_t bv = c->mem_r8(node + 7);
        if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
        c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
        call1(c, node, FN_801406E4);
        if (1 < bv) return;
        rec_dispatch(c, FN_8009A450);
        uVar6 = c->r[R_V0];
        uVar7 = 0x101u;
        if ((uVar6 & 0x8000) == 0) goto LAB_801448e8;
        goto LAB_80144908;
      }
    }
    case 3: {
      uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 != 1) {
        if (1 < bVar2) {
          uVar6 = 0;
          if (bVar2 != 2) return;
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 0x1au, 8u);
            c->mem_w16(node + 0x4e, 0);
            c->mem_w8(node + 7, 1);
            goto LAB_80143d00_a;
          } else {
            if (c->mem_r8(node + 7) == 1) goto LAB_80143d00_a;
            if (c->mem_r8(node + 7) < 0x14) {
              const int8_t cVar3 = (int8_t)(c->mem_r8(node + 7) + 1);
              c->mem_w8(node + 7, (uint8_t)cVar3);
              goto LAB_80143d00_tail_skip;
            }
            uVar6 = 1;
            goto LAB_80143d00_tail_skip;
          }
LAB_80143d00_a: {
            const uint32_t p38 = c->mem_r32(node + 0x38);
            if (c->mem_r16s(p38 + 4) != 0) {
              c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) ^ 1));
              c->mem_w16(node + 0x56, (uint16_t)(c->mem_r16(node + 0x56) + 0x800));
              call2(c, node, FN_801402B8, 7u, 0u);
              c->mem_w8(node + 7, 2);
            }
          }
LAB_80143d00_tail_skip:
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          break;
        }
        if (bVar2 != 0) return;
        c->mem_w16(node + 6, 1);
      }
      goto LAB_80143c48;
    }
    case 4: {
      uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 != 1) {
        if (1 < bVar2) {
          if (bVar2 == 2) {
            call1(c, node, FN_8014213C);
            iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
            goto LAB_80144550;
          }
          if (bVar2 != 3) return;
          call1(c, node, FN_801422B4);
          uVar6 = (uint32_t)c->r[R_V0] << 16;
          break;
        }
        if (bVar2 != 0) return;
        c->mem_w16(node + 6, 1);
      }
      goto LAB_80143c48;
    }
    case 5: {
      // (case 4's "goto LAB_80143c48" lands in case 5's own fallback block below.)
LAB_80143c48:
      call1(c, node, FN_801425F0);
      iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
      goto LAB_801448e0;
    }
    case 6: {
      const uint8_t sub6 = c->mem_r8(node + 6);
      switch (sub6) {
        case 0:
        case 1:
          call1(c, node, FN_801408AC);
          return;
        case 2:
          call2(c, node, FN_80141AC4, 0x20u, 0x500u);
          iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
          goto LAB_80144550;
        case 3: {
          c->r[R_A0] = node; c->r[R_A1] = 0; c->r[R_A2] = 0x1900u;
          rec_dispatch(c, FN_80141AC4);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          if (sVar4 != 0) c->mem_w16(node + 6, 3);
          bool spawn5f;
          const uint8_t n5f = c->mem_r8(node + 0x5f);
          if (n5f == 0) spawn5f = false;
          else if (n5f == 3) spawn5f = (c->mem_r16(node + 0x62) & 1) != 0;
          else spawn5f = (c->mem_r16(node + 0x62) & 1) == 0;
          if (!spawn5f) return;
          c->mem_w32(node + 4, 0xa01u);
          return;
        }
        case 4: {
          call1(c, node, FN_80141C20);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          if (sVar4 != 0) c->mem_w16(node + 6, 4);
          bool spawn5f;
          const uint8_t n5f = c->mem_r8(node + 0x5f);
          if (n5f == 0) spawn5f = false;
          else if (n5f == 3) spawn5f = (c->mem_r16(node + 0x62) & 1) != 0;
          else spawn5f = (c->mem_r16(node + 0x62) & 1) == 0;
          if (!spawn5f) return;
          c->mem_w32(node + 4, 0xa01u);
          return;
        }
        case 5: {
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 10u, 8u);
            c->mem_w8(node + 7, 0x5a);
            c->mem_w16(node + 0x4e, 0);
          }
          uint8_t bv = c->mem_r8(node + 7);
          if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
          uVar6 = (uint32_t)(1 >= bv);
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          break;
        }
        default: return;
      }
      break;
    }
    case 7: {
      const uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 == 2) {
        call1(c, node, FN_8014181C);
        iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
        goto LAB_80144550;
      }
      if (bVar2 < 3) { call1(c, node, FN_801408AC); return; }
      if (bVar2 != 3) return;
      if (c->mem_r8(node + 7) == 0) {
        c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
        call2(c, node, FN_801402B8, 5u, 8u);
        c->mem_w8(node + 7, 0x2d);
        c->mem_w16(node + 0x4e, 0);
      }
      uint8_t bv = c->mem_r8(node + 7);
      if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
      uVar6 = (uint32_t)(1 >= bv);
      c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
      call1(c, node, FN_801406E4);
      break;
    }
    case 8: {
      const uint8_t sub8 = c->mem_r8(node + 6);
      switch (sub8) {
        case 0:
        case 1:
          call1(c, node, FN_801408AC);
          return;
        case 2:
          call1(c, node, FN_8014103C);
          iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
          goto LAB_80144550;
        case 3: {
          call2(c, node, FN_80141AC4, 0x20u, 0x500u);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          if (sVar4 == 0) return;
          c->mem_w16(node + 6, 4);
          return;
        }
        case 4: {
          call1(c, node, FN_80141438);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          if (sVar4 != 0) c->mem_w16(node + 6, 5);
          bool spawn5f;
          const uint8_t n5f = c->mem_r8(node + 0x5f);
          if (n5f == 0) spawn5f = false;
          else if (n5f == 3) spawn5f = (c->mem_r16(node + 0x62) & 1) != 0;
          else spawn5f = (c->mem_r16(node + 0x62) & 1) == 0;
          if (!spawn5f) return;
          c->mem_w32(node + 4, 0xa01u);
          return;
        }
        case 5: {
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 0x1fu, 8u);
            c->mem_w8(node + 7, 0x32);
            c->mem_w16(node + 0x4e, 0);
          }
          uint8_t bv = c->mem_r8(node + 7);
          if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          if (1 < bv) return;
          c->mem_w16(node + 6, 6);
          return;
        }
        case 6: {
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 5u, 8u);
            c->mem_w8(node + 7, 0x2d);
            c->mem_w16(node + 0x4e, 0);
          }
          uint8_t bv = c->mem_r8(node + 7);
          if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
          uVar6 = (uint32_t)(1 >= bv);
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          break;
        }
        default: return;
      }
      break;
    }
    case 9: {
      const uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 != 1) {
        if (1 < bVar2) { sharedBVar2 = bVar2; goto LAB_801448a8; }
        if (bVar2 != 0) return;
        c->mem_w16(node + 6, 1);
      }
      call1(c, node, FN_80140AF4);
      iVar5 = (int32_t)((uint32_t)c->r[R_V0] << 16);
      goto LAB_801448e0;
    }
    case 10: {
      const uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 != 1) {
        if (1 < bVar2) { sharedBVar2 = bVar2; goto LAB_801448a8; }
        if (bVar2 != 0) return;
        c->mem_w16(node + 6, 1);
      }
      call1(c, node, FN_80140AF4);
      {
        const int32_t v0copy = (int32_t)(int16_t)c->r[R_V0];
        if (c->mem_r8(node + 7) < 3) c->mem_w16(node + 0x58, 0);
        iVar5 = v0copy << 16;
      }
      goto LAB_801448e0;
    }
    case 0xb: {
      const uint8_t sub_b = c->mem_r8(node + 6);
      switch (sub_b) {
        case 0:
        case 1:
          call1(c, node, FN_801408AC);
          return;
        case 2: {
          call2(c, node, FN_80142A94, 0x300u, 0x1e00u);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          uVar7 = 0xa01u;
          if (sVar4 != -1) {
            if (sVar4 == 0) {
              bool spawn5f;
              const uint8_t n5f = c->mem_r8(node + 0x5f);
              if (n5f == 0) spawn5f = false;
              else if (n5f == 3) spawn5f = (c->mem_r16(node + 0x62) & 1) != 0;
              else spawn5f = (c->mem_r16(node + 0x62) & 1) == 0;
              if (!spawn5f) return;
              c->mem_w32(node + 4, 0xa01u);
              return;
            }
            c->mem_w16(node + 6, 4);
            return;
          }
          break;
        }
        case 3: {
          call1(c, node, FN_80142CF4);
          const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
          uVar7 = 0xa01u;
          if (sVar4 != -1) {
            if (sVar4 == 0) {
              bool spawn5f;
              const uint8_t n5f = c->mem_r8(node + 0x5f);
              if (n5f == 0) spawn5f = false;
              else if (n5f == 3) spawn5f = (c->mem_r16(node + 0x62) & 1) != 0;
              else spawn5f = (c->mem_r16(node + 0x62) & 1) == 0;
              if (!spawn5f) return;
              c->mem_w32(node + 4, 0xa01u);
              return;
            }
            c->mem_w16(node + 6, 3);
            return;
          }
          break;
        }
        case 4: {
          if (c->mem_r8(node + 7) == 0) {
            c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
            call2(c, node, FN_801402B8, 10u, 8u);
            c->mem_w8(node + 7, 0x3c);
            c->mem_w16(node + 0x4e, 0);
          }
          uint8_t bv = c->mem_r8(node + 7);
          if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
          uVar6 = (uint32_t)(1 >= bv);
          c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
          call1(c, node, FN_801406E4);
          goto LAB_801448fc;
        }
        default: return;   // switchD_80143a48_caseD_10 (final return)
      }
      goto LAB_80144908;
    }
    case 0xc: {
      const uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 == 2) {
        const int32_t nx = c->mem_r16s(node + 0x2e);
        const int32_t tx = c->mem_r16s(G_1F800160);
        const int32_t nz = c->mem_r16s(node + 0x36);
        const int32_t tz = c->mem_r16s(G_1F800164);
        c->r[R_A0] = (uint32_t)(int32_t)(int16_t)(nx - tx);
        c->r[R_A1] = (uint32_t)(int32_t)(int16_t)(nz - tz);
        rec_dispatch(c, FN_800781E0);
        iVar5 = (int32_t)(int16_t)c->r[R_V0];
        if (iVar5 < 800) goto LAB_80144550;
        call2(c, node, FN_80142A94, 800u, 0x1e00u);
        const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
        uVar7 = 0xa01u;
        if (sVar4 != -1) {
          if (sVar4 == 0) return;
          c->mem_w16(node + 6, 3);
          return;
        }
        goto LAB_80144908;
      }
      if (bVar2 < 3) { call1(c, node, FN_801408AC); return; }
      if (bVar2 != 3) return;
      call1(c, node, FN_801436C4);
      {
        const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
        uVar7 = 0xa01u;
        if (sVar4 == -1) goto LAB_80144908;
        if (sVar4 == 0) return;
        if (c->mem_r8(G_800E7E80) & 2) {
          c->mem_w32(node + 4, 0xf01u);
          return;
        }
        goto LAB_80144904;
      }
    }
    case 0xd: {
      const uint8_t bVar2 = c->mem_r8(node + 6);
      if (bVar2 == 2) {
        call1(c, node, FN_801431C4);
        const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
        uVar7 = 0xa01u;
        if (sVar4 != -1) {
          if (sVar4 == 0) return;
          c->mem_w16(node + 6, 3);
          return;
        }
        goto LAB_80144908;
      }
      if (bVar2 < 3) { call1(c, node, FN_801408AC); return; }
      if (bVar2 != 3) return;
      if (c->mem_r8(node + 7) == 0) {
        c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
        call2(c, node, FN_801402B8, 5u, 8u);
        c->mem_w8(node + 7, 0x2d);
        c->mem_w16(node + 0x4e, 0);
      }
      uint8_t bv = c->mem_r8(node + 7);
      if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
      uVar6 = (uint32_t)(1 >= bv);
      c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
      call1(c, node, FN_801406E4);
      break;
    }
    case 0xe: {
      uint8_t n6 = c->mem_r8(node + 6);
      if (n6 == 0) { c->mem_w16(node + 6, 1); }
      else if (n6 != 1) { return; }
      if (c->mem_r8(node + 7) == 0) {
        c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
        call2(c, node, FN_801402B8, 0x2fu, 8u);
        c->mem_w8(node + 7, 0x1e);
        c->mem_w16(node + 0x4e, 0);
      }
      uint8_t bv = c->mem_r8(node + 7);
      if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
      uVar6 = (uint32_t)(1 >= bv);
      c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
      call1(c, node, FN_801406E4);
      break;
    }
    case 0xf: {
      uint8_t n6 = c->mem_r8(node + 6);
      if (n6 == 0) { c->mem_w16(node + 6, 1); }
      else if (n6 != 1) { return; }
      if (c->mem_r8(node + 7) == 0) {
        c->mem_w16(node + 0x62, (uint16_t)(c->mem_r16(node + 0x62) & 0xfffb));
        call2(c, node, FN_801402B8, 0x30u, 8u);
        c->mem_w8(node + 7, 0x1e);
        c->mem_w16(node + 0x4e, 0);
      }
      uint8_t bv = c->mem_r8(node + 7);
      if (1 < bv) c->mem_w8(node + 7, (uint8_t)(bv - 1));
      uVar6 = (uint32_t)(1 >= bv);
      c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
      call1(c, node, FN_801406E4);
      break;
    }
    default:
      return;   // switchD_80143a48_caseD_10
  }
  goto LAB_801448fc;

LAB_801448a8:
  if (sharedBVar2 != 2) return;
  call1(c, node, FN_8014243C);
  uVar6 = (uint32_t)c->r[R_V0] << 16;

LAB_801448fc:
  if (uVar6 != 0) {
LAB_80144904:
    uVar7 = 0x101u;
LAB_80144908:
    c->mem_w32(node + 4, uVar7);
  }
  return;   // switchD_80143a48_caseD_10

LAB_80144550:
  if (iVar5 == 0) return;
  // LAB_80144558
  c->mem_w16(node + 6, 3);
  return;

LAB_801448e0:
  if (iVar5 == 0) return;
LAB_801448e8:
  c->mem_w16(node + 6, 2);
  return;
}

// ActorZonedAttacker::idleTick(c) — FUN_80144b50(node). The "idle" state (2) handler: cases 1/4/5
// run brief scripted sub-states (item pickup / carry, each gated on a packed node[4..7] word);
// case 2 is the MAIN per-frame idle body: a despawn re-arm check (mirrors gateCheck's own DAT_800e7eaa
// window), then — while not paused/dialog-locked — a big node[6] sub-state machine (approach-anim,
// countdown-driven attack-cue spawn via Spawn::spawnAndInit, color-lerp-toward-neutral on death,
// etc.); cases 7/8/10/0xb bracket palette flashes around a shared "movement" tail
// (switchD_80144b94_caseD_2) that either seeds a walk cycle or integrates a velocity into node[0x2c/
// 0x34] (position) each frame. Ends by re-running gateCheck + the cull/render/flag-fold tail — the
// EXACT same tail shape as the caller's own second_cull block (both close over FN_8014047c ->
// FN_800777FC -> FN_800518FC -> node[0xb] fold).
// ----------------------------------------------------------------------------------------------
void ActorZonedAttacker::idleTick(Core* c) {
  const uint32_t node = c->r[R_A0];
  const uint8_t state5 = c->mem_r8(node + 5);

  switch (state5) {
    case 0:
    case 6:
      goto switchD_caseD_0;
    case 1:
      if (c->mem_r8(node + 6) == 0) {
        c->r[R_A0] = 4; rec_dispatch(c, FN_80026100);
        call2(c, node, FN_801402B8, 2u, 4u);
        c->mem_w16(node + 0x84, 0x14);
        c->mem_w16(node + 0x86, 100);
        c->r[R_A0] = 0x89u; c->r[R_A1] = 0; c->r[R_A2] = 0; rec_dispatch(c, FN_80074590);
        c->mem_w8(node + 0x1b, (uint8_t)(c->mem_r8(node + 0x1b) & 0xbf));
        c->mem_w8(node + 0xd, (uint8_t)(c->mem_r8(node + 0xd) & 0xfd));
        c->mem_w8(node + 6, 1);
      } else if (c->mem_r8(node + 6) != 1) {
        goto switchD_caseD_3;
      }
      goto LAB_801451a0;
    case 2:
      break;
    default:
      goto switchD_caseD_3;
    case 4:
      if ((c->mem_r32(node + 4) & 0xffff00u) == 0x400u) {
        c->r[R_A0] = node; c->r[R_A1] = 0x20u; c->r[R_A2] = 0x30u; c->r[R_A3] = 0xffu;
        rec_dispatch(c, FN_80077E20);
        goto LAB_80144bd0;
      }
      goto LAB_80144bd0;
    case 5:
LAB_80144bd0:
      if ((c->mem_r32(node + 4) & 0xffff00u) == 0x500u) {
        c->r[R_A0] = node; c->r[R_A1] = 0xffu; c->r[R_A2] = 0x30u; c->r[R_A3] = 0x30u;
        rec_dispatch(c, FN_80077E20);
      }
switchD_caseD_0:
      {
        bool bVar1;
        if (c->mem_r16s(node + 0x32) < 1) {
          if ((int8_t)c->mem_r8(node + 0x66) == (int8_t)0x81) {
            bVar1 = c->mem_r16s(node + 0x32) < -0x81;
          } else {
            bVar1 = c->mem_r8(G_800E7EAA) < 0xc;
          }
          bVar1 = !bVar1;
        } else {
          bVar1 = true;
        }
        if (bVar1) {
          c->mem_w8(node + 4, 3);
          goto switchD_caseD_3;
        }
        if ((c->mem_r8(G_1F800137) == 0 || c->mem_r8(G_800BF89C) == 2) && c->mem_r8(G_800BF809) == 0) {
          uint8_t bVar6 = c->mem_r8(node + 6);
          if (bVar6 == 1) {
            goto LAB_80144d20;
          } else if (bVar6 < 2) {
            if (bVar6 == 0) {
              const uint32_t lhs = (((uint32_t)c->mem_r8(node + 0x2b) * 0x10 - 0x800) & 0xfffu);
              const uint32_t val = (uint32_t)((int32_t)lhs - c->mem_r16s(node + 0x60) + 0x400) & 0xfffu;
              uint16_t v62 = c->mem_r16(node + 0x62);
              v62 = (val < 0x801u) ? (uint16_t)(v62 | 1) : (uint16_t)(v62 & 0xfffe);
              c->mem_w16(node + 0x62, v62);
              c->r[R_A0] = 0x88u; c->r[R_A1] = 0; c->r[R_A2] = 0; rec_dispatch(c, FN_80074590);
              c->mem_w16(node + 6, 1);
              goto LAB_80144d20;
            }
          } else if (bVar6 == 2) {
            c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + 0x10));
            call1(c, node, FN_801406E4);
            uint16_t v42 = c->mem_r16(node + 0x42);
            c->mem_w16(node + 0x42, (uint16_t)(v42 - 1));
            if ((int32_t)((int16_t)v42) < 1) c->mem_w16(node + 6, 3);
          } else if (bVar6 == 3) {
            if (c->mem_r8(node + 0xd) & 2) {
              auto lerp = [&](uint32_t off) {
                const uint8_t v = c->mem_r8(node + off);
                const int32_t diff = (int32_t)(0x80 - (uint32_t)v);
                const int8_t inc = (int8_t)(diff >> 3);
                c->mem_w8(node + off, (uint8_t)(v + (uint8_t)inc));
              };
              lerp(0x18); lerp(0x19); lerp(0x1a);
            }
            call1(c, node, FN_8014243C);
            const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
            call1(c, node, FN_80076D68);
            if (sVar4 != 0) {
              c->mem_w8(node + 0, 1);
              c->mem_w8(node + 0xd, (uint8_t)(c->mem_r8(node + 0xd) & 0xfd));
              c->mem_w8(node + 0x1b, (uint8_t)(c->mem_r8(node + 0x1b) & 0xbf));
              c->mem_w8(node + 0x2b, 0);
              c->mem_w8(node + 3, 0);
              c->mem_w32(node + 4, 1u);
            }
          }
          goto idle_after_substate;
LAB_80144d20:
          call1(c, node, FN_80140AF4);
          {
            const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
            if (c->mem_r8(node + 5) == 4) {
              if (sVar4 != 0) {
                c->mem_w16(node + 6, 2);
                c->mem_w16(node + 0x42, 0x5a);
              }
            } else {
              call1(c, node, FN_80076D68);
              if (sVar4 != 0) c->mem_w16(node + 6, 3);
            }
          }
          {
            const uint8_t cd = c->mem_r8(G_8014BF5E);
            c->mem_w8(G_8014BF5E, (uint8_t)(cd - 1));
            if ((int8_t)cd < 0) {
              const uint32_t kind = (uint32_t)(c->mem_r16(node + 0x68) >> 8) & 0xf;
              if (kind == 1 || (kind == 2 && c->mem_r16s(node + 0x4e) > 0x500)) {
                const uint32_t gsp = c->r[29];
                c->mem_w16(gsp + 0x12, (uint16_t)c->mem_r16(node + 0x2e));
                c->mem_w16(gsp + 0x16, (uint16_t)c->mem_r16(node + 0x6a));
                c->mem_w16(gsp + 0x1a, (uint16_t)c->mem_r16(node + 0x36));
                eng(c).spawn.spawnAndInit(8u, gsp + 0x10, (uint32_t)(int32_t)-0x50);
                c->mem_w8(G_8014BF5E, 10);
              }
              c->mem_w16(node + 0x68, 0);
            }
          }
        }
idle_after_substate:
        if (c->mem_r8(node + 0x2a) == 1 && c->mem_r16s(node + 0x2e) > 0x31a8) {
          c->mem_w16(node + 0x2e, 0x31a8);
        }
        goto switchD_caseD_3;
      }
    case 7:
      goto switchD_caseD_7;
    case 8:
      goto switchD_caseD_8;
    case 10: {
      call1(c, node, FN_801280E8);
      const int32_t sVar4 = (int32_t)(int16_t)c->r[R_V0];
      if (sVar4 != 0) goto LAB_801451b0;
      c->mem_w8(node + 5, 2);
      goto switchD_caseD_8;
    }
    case 0xb: {
      c->r[R_A0] = node; c->r[R_A1] = c->mem_r32(node + 0xc0); c->r[R_A2] = 1u;
      rec_dispatch(c, FN_80080750);
      const int32_t iVar5 = (int32_t)c->r[R_V0];
      if (iVar5 != 0) goto LAB_801451b0;
      c->mem_w8(node + 5, 2);
      break;
    }
  }

switchD_caseD_2:
  if (c->mem_r8(node + 6) == 0) {
    call2(c, node, FN_801402B8, 2u, 4u);
    c->r[R_A0] = (uint32_t)c->mem_r8(node + 0x2b) << 4;
    c->r[R_A1] = (uint32_t)c->mem_r16s(node + 0x60);
    c->r[R_A2] = 0;
    rec_dispatch(c, FN_80077768);
    uint16_t v62 = c->mem_r16(node + 0x62);
    v62 = (c->r[R_V0] == 0) ? (uint16_t)(v62 | 1) : (uint16_t)(v62 & 0xfffe);
    c->mem_w16(node + 0x62, v62);
    c->mem_w16(node + 0x4e, 0xe000);
    c->mem_w16(node + 0x50, 0xffd8);
    c->mem_w8(node + 0x29, 0);
    c->mem_w8(node + 6, 1);
    c->r[R_A0] = 0x88u; c->r[R_A1] = 0; c->r[R_A2] = 0; rec_dispatch(c, FN_80074590);
  } else {
    if (c->mem_r8(node + 6) != 1) {
      c->mem_w8(node + 4, 3);
      return;
    }
    int32_t iVar5 = c->mem_r16s(node + 0x48) * c->mem_r16s(node + 0x4e);
    int32_t iVar7 = c->mem_r16s(node + 0x4c) * c->mem_r16s(node + 0x4e);
    if ((c->mem_r16(node + 0x62) & 1) == 0) {
      iVar5 = (int32_t)c->mem_r32(node + 0x2c) + iVar5;
      iVar7 = (int32_t)c->mem_r32(node + 0x34) + iVar7;
    } else {
      iVar5 = (int32_t)c->mem_r32(node + 0x2c) - iVar5;
      iVar7 = (int32_t)c->mem_r32(node + 0x34) - iVar7;
    }
    c->mem_w32(node + 0x2c, (uint32_t)iVar5);
    c->mem_w32(node + 0x34, (uint32_t)iVar7);
    c->mem_w16(node + 0x32, (uint16_t)(c->mem_r16s(node + 0x32) + c->mem_r16s(node + 0x50)));
    c->mem_w16(node + 0x58, (uint16_t)(c->mem_r16s(node + 0x58) + 0xcc));
    bool despawnNow = (c->mem_r8(node + 0x29) != 0);
    if (!despawnNow) {
      c->r[R_A0] = node; c->r[R_A1] = 0; c->r[R_A2] = 0;
      rec_dispatch(c, FN_800495DC);
      despawnNow = (c->r[R_V0] != 0);
    }
    if (despawnNow) {
      c->r[R_A0] = node + 0x2c; rec_dispatch(c, FN_800315D4);
      c->r[R_A0] = 0x1bu; c->r[R_A1] = 0; c->r[R_A2] = 0; rec_dispatch(c, FN_80074590);
      goto LAB_801451b0;
    }
    const int32_t sVar4 = c->mem_r16s(node + 0x50);
    c->mem_w16(node + 0x50, (uint16_t)(sVar4 + 4));
    if ((int16_t)(sVar4 + 4) > 0x3c) c->mem_w16(node + 0x50, 0x3c);
    goto LAB_801451a0;
  }
  goto switchD_caseD_3;

// These two live at FUNCTION scope (not nested in the if/else above) so the case1/case10/case0xb
// external jumps into them don't cross iVar5/iVar7/despawnNow/sVar4's initializations above.
LAB_801451b0:
  c->mem_w8(node + 4, 3);
  return;

LAB_801451a0:
  call1(c, node, FN_80076D68);

switchD_caseD_3:
  call1(c, node, FN_8014047C);
  {
    const int32_t iVar5 = (int32_t)c->r[R_V0];
    if (iVar5 == 0) {
      call1(c, node, FN_800777FC);
      if (c->r[R_V0] != 0) {
        call1(c, node, FN_800518FC);
        uint8_t bVar6;
        if (c->mem_r8(node + 0x29) == 0) bVar6 = (uint8_t)(c->mem_r8(node + 0xb) & 0x3f);
        else bVar6 = (uint8_t)((c->mem_r8(node + 0xb) & 0xc0) | 0x80);
        c->mem_w8(node + 0xb, bVar6);
      }
    }
  }
  c->mem_w8(node + 0x29, 0);
  return;

switchD_caseD_8:
  if ((c->mem_r32(node + 4) & 0xffff00u) == 0x800u) {
    c->r[R_A0] = node; c->r[R_A1] = 0xffu; c->r[R_A2] = 0x30u; c->r[R_A3] = 0x30u;
    rec_dispatch(c, FN_80077E20);
switchD_caseD_7:;
  }
  if ((c->mem_r32(node + 4) & 0xffff00u) == 0x700u) {
    c->r[R_A0] = node; c->r[R_A1] = 0x20u; c->r[R_A2] = 0x30u; c->r[R_A3] = 0xffu;
    rec_dispatch(c, FN_80077E20);
  }
  goto switchD_caseD_2;
}

extern void ov_a00_gen_8014047C(Core*);
extern void ov_a00_gen_80140544(Core*);
extern void ov_a00_gen_801409C0(Core*);
extern void ov_a00_gen_80143A00(Core*);
extern void ov_a00_gen_80144928(Core*);
extern void ov_a00_gen_80144B50(Core*);

void ActorZonedAttacker::registerOverrides(Game* /*game*/) {
  using overrides::install;
  install(FN_8014047C, "ActorZonedAttacker::gateCheck",              ActorZonedAttacker::gateCheck,              ov_a00_gen_8014047C);
  install(0x80140544u, "ActorZonedAttacker::typeInit",               ActorZonedAttacker::typeInit,               ov_a00_gen_80140544);
  install(FN_801409C0, "ActorZonedAttacker::pickAttackByRange",      ActorZonedAttacker::pickAttackByRange,      ov_a00_gen_801409C0);
  install(0x80143A00u, "ActorZonedAttacker::defaultSubStateMachine", ActorZonedAttacker::defaultSubStateMachine, ov_a00_gen_80143A00);
  install(0x80144928u, "ActorZonedAttacker::approachAndFace",        ActorZonedAttacker::approachAndFace,        ov_a00_gen_80144928);
  install(0x80144B50u, "ActorZonedAttacker::idleTick",               ActorZonedAttacker::idleTick,               ov_a00_gen_80144B50);
}
