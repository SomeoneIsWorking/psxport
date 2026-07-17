// game/ai/beh_seaside_prox_substate.cpp — per-object behavior handler FUN_8013C1DC (A00 overlay).
//
// The LAST unowned handler in the seaside placement table (0x801469BC[62 records + terminator];
// this handler covers record 61 — cls=4 type=0x02 at world pos (16606, -4980, 11200) — a single
// deep-underwater actor in the seaside area). Every other placement handler in the seaside table
// is already native, so owning this closes the seaside placement-installed set 100%.
//
// STRUCTURAL SHAPE — proximity-gated 3-way substate router:
//   * node+4  (outer state): 0 = INIT / arming, 1 = RUNNING, 3 = DESPAWN.
//   * node+5  (inner INIT sub-state):
//       0 = stamp field-scoped constants (node+0xBF=0xE0, +0x7E=0x1B, +0x7C=0x400, +0x7A=0x0B,
//           +0x78=0x30) and arm mode 0 via FUN_8013C0BC(0), then advance sub-state and fall through
//           to the INIT draw call (FUN_8013B70C(node, DAT_8014ABE8)).
//       1 = keep issuing the INIT draw each frame.
//       2 = PROXIMITY ARM. Compute Chebyshev-ish distance from the camera position (scratch
//           0x1F800160/162/164) to the actor pos via FUN_80078240; if it's ≤ 1000 AND scratch
//           0x1F80027A == 0 AND master-G byte @0x800E7E80 == 6 AND 0x800BF80D == 0, transition to
//           RUNNING: node+4=1, node+5=0, node+0x5E=0. Otherwise this frame is idle.
//       any other = no-op (return).
//   * node+0x5E (RUNNING sub-state): 3-way switch → three sibling sub-behavior leaves in the A00
//     overlay:
//       0 → FUN_8013B868  (sub-behavior A — anim/SFX arm sequence, native `subA` below)
//       1 → FUN_8013BAB0  (sub-behavior B — camera-nudge/facing machine, native `subB` below)
//       2 → FUN_8013BCC8  (sub-behavior C — cutscene data blast + item spawn, native `subC` below)
//       else → no side effects beyond clearing node+0x29.
//     Every RUNNING path clears node+0x29 (a per-frame flag) after its sub-behavior. Before the
//     sub-behavior dispatch, RUNNING runs Actor::boundsCull; if visible it fires FUN_8013C0BC(1)
//     (mode toggle, native `modeArm` below) then FUN_80051844(node) (post-cull graphics/interaction
//     update — sibling of FUN_800518FC used by the beh_sop_intro_pilot chain).
//   * node+4 == 3 → standard pool return via Spawn::despawn (native).
//
// OWNERSHIP (2026-07-08): the control flow (above) AND all five A00-overlay sub-behavior leaves
// this handler drives directly — FUN_8013B70C (`drawInit`), FUN_8013B868 (`subA`), FUN_8013BAB0
// (`subB`), FUN_8013BCC8 (`subC`), FUN_8013C0BC (`modeArm`) — are now native, RE'd 1:1 from Ghidra
// decompile (scratch/decomp/beh_seaside_cluster.c, scratch/decomp/beh_seaside_helpers.c) CROSS-
// CHECKED against a capstone disas of the same RAM dump (scratch/ram/field_seaside.bin, dumped via
// REPL `dumpram` after `newgame; skip 500` reaching the seaside field with the A00 overlay
// resident) for every branch and address computation. FUN_80051844 and FUN_80078240 (post-cull
// update / distance helper) remain un-owned rec_dispatch leaves — outside this cluster's scope.
// Each ported leaf still calls its OWN un-owned callees (FUN_8006E1C0/E1E4 sound-loop start/stop,
// FUN_8004766C/80048750 tile-move+anim-link pair, FUN_8013B534 sub-object init table, FUN_80027144
// GPU packet emit, FUN_8009A450 PRNG) via rec_dispatch, matching the sibling beh_* convention
// (beh_area_transition_machine.cpp, beh_pickup_collect_trigger.cpp) — those leaves are shared with
// many other still-substrate callers and are each their own future frontier item, NOT part of this
// cluster. FUN_8003116C (Spawn::spawnAndInitBody) and FUN_80081218 (Asset::uploadImage /
// GpuState::gpu_native_load_vram) ARE already native via EngineOverride — routed the same way
// (rec_dispatch to the wired guest address) so the override fires transparently.
//
// Ghidra decomp: scratch/decomp/beh_seaside_cluster.c, scratch/decomp/beh_seaside_helpers.c.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "core/engine.h"          // eng(c).spawn
#include "spawn.h"                 // eng(c).spawn.despawn (FUN_8007A624, native)
#include "math/trig.h"             // Trig::ratan2 / Trig::angleCmp (FUN_80085690 / FUN_80077768, native)
#include "audio/sfx.h"             // eng(c).sfx.trigger (FUN_80074590, native)
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN            = 0x8013C1DCu;

// -- guest-RAM addresses this handler reads for the proximity/arming gate ---------------------
constexpr uint32_t CAM_X_SPAD        = 0x1F800160u;   // camera pos scratchpad (s16)
constexpr uint32_t CAM_Y_SPAD        = 0x1F800162u;
constexpr uint32_t CAM_Z_SPAD        = 0x1F800164u;
constexpr uint32_t GATE_SPAD_27A     = 0x1F80027Au;   // per-frame gate byte
constexpr uint32_t MASTER_G_BYTE     = 0x800E7E80u;   // master-G first byte (mode selector)
constexpr uint32_t GATE_BF80D        = 0x800BF80Du;   // scene-transition guard byte

// A00-overlay data ptrs threaded into the sub-behaviors as fixed arguments.
constexpr uint32_t INIT_DRAW_DATA    = 0x8014ABE8u;   // arg to FUN_8013B70C (init draw)
constexpr uint32_t SUB_C_DATA        = 0x8014AC08u;   // arg to FUN_8013BCC8 (sub-behavior C)

constexpr int32_t  PROXIMITY_MAX     = 1000;          // FUN_80078240 result <= 1000 arms the trigger

// -- Camera-G struct (base 0x800E7E80) fields used by subB / mode-arm/-related sub-behaviors.
// Same struct + naming convention as game/ai/beh_area_transition_machine.cpp (G_eac/eb0/eb4 =
// camera pos, hi-words G_eae/eb2/eb6) — reused here verbatim since subB nudges the SAME globals.
constexpr uint32_t G                 = 0x800E7E80u;
constexpr uint32_t G_eac              = 0x800E7EACu;   // camera pos X (32-bit fixed)
constexpr uint32_t G_eb0              = 0x800E7EB0u;   // camera pos Y (32-bit fixed)
constexpr uint32_t G_eb2              = 0x800E7EB2u;   // hi-word of G_eb0 (s16 view)
constexpr uint32_t G_eb4              = 0x800E7EB4u;   // camera pos Z (32-bit fixed)
constexpr uint32_t G_ec8              = 0x800E7EC8u;   // per-frame X velocity multiplier (s16)
constexpr uint32_t G_ecc              = 0x800E7ECCu;   // per-frame Z velocity multiplier (s16)
constexpr uint32_t G_f38              = 0x800E7F38u;   // zoom/shake field A (s16, -= 0x80 per tick)
constexpr uint32_t G_f3a              = 0x800E7F3Au;   // zoom/shake field B (s16, -= 0x80 per tick)
constexpr uint32_t G_f3c              = 0x800E7F3Cu;   // zoom/shake field C (s16, -= 0x80 per tick)

// -- Event/area-slot table walked by FUN_8013B70C. Ghidra resolves these against the nearest prior
// label (DAT_800bf870/871, the single-byte "area id" global documented in docs/engine_re.md), but
// the walk here is over a SEPARATE byte array a few bytes further in memory — the count byte and
// table base below are exactly what the disas computes (0x800BF870+0x13 / +0x14); no other owner
// names this table, so it's kept as a raw address with the derivation noted rather than invented.
constexpr uint32_t EVENT_SLOT_COUNT   = 0x800BF883u;   // *(0x800bf870+0x13)
constexpr uint32_t EVENT_SLOT_TABLE   = 0x800BF884u;   // *(0x800bf870+0x14) + k, k=0..count-1
constexpr uint32_t EVENT_SLOT_ARM_BIT = 0x800BF871u;   // *(0x800bf870+1)  (byte, NOT ushort — disas-verified)
constexpr uint32_t GATE_BFA19         = 0x800BFA19u;   // *(0x800bf870+0x1a9) — per-slot "despawn" bitmask
constexpr uint32_t GATE_BFA17         = 0x800BFA17u;   // *(0x800bf870+0x1a7) — per-slot "commit" bitmask

// -- mode-arm (FUN_8013C0BC) sprite-cycle tables. Single-entry ([0,1)) arrays per the disas loop
// bound (`blez $s1` / `iVar7 < 1`) — this actor has exactly one animated overlay slot.
constexpr uint32_t MODEARM_COUNTDOWN  = 0x8014C77Cu;   // per-slot countdown byte
constexpr uint32_t MODEARM_TABLE_PTR  = 0x8014ABDCu;   // PTR_DAT_8014abdc — cursor into the duration/index list
constexpr uint32_t MODEARM_REC_BASE   = 0x8014ABE6u;   // (x,y) pos + VRAM-base-4 short-record for the sprite

// -- Substrate leaves (A00-overlay code kept dispatched — their own future sub-frontier) ------
inline void post_cull_upd  (Core* c, uint32_t o)   { c->r[4] = o;                                     rec_dispatch(c, 0x80051844u); }
inline int  bounds_cull    (Core* c, uint32_t o)   { c->r[4] = o; rec_dispatch(c, 0x8007778Cu); return (int)c->r[2]; }
inline int32_t cam_distance(Core* c, uint32_t o) {
  c->r[4] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_X_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x2E));
  c->r[5] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_Y_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x32));
  c->r[6] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_Z_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x36));
  rec_dispatch(c, 0x80078240u);
  return (int32_t)c->r[2];
}

// ===============================================================================================
// FUN_8013C0BC — "mode arm" sprite-frame cycler. mode==0 resets the countdown; mode!=0 ticks it
// down and, on expiry, advances a duration/index cursor (with a -1-sentinel loop-back) and blits
// the next sprite frame via FUN_80081218 (native Asset::uploadImage / GpuState upload — reached
// here by rec_dispatch to the wired guest address, per the EngineOverride convention).
// RE'd 1:1 from disas 0x8013c0bc..0x8013c1dc (single active slot; loop bound is `< 1`).
void modeArm(Core* c, int32_t mode) {
  if (mode == 0) {
    c->mem_w8(MODEARM_COUNTDOWN, 0);
    return;
  }
  uint8_t after = (uint8_t)(c->mem_r8(MODEARM_COUNTDOWN) - 1);
  c->mem_w8(MODEARM_COUNTDOWN, after);
  if ((int8_t)after > 0) return;   // `(int)((byte)(bVar1-1) << 0x18) < 1` == (int8_t)after <= 0 taken; guard is inverted here on purpose
  uint32_t ptr = c->mem_r32(MODEARM_TABLE_PTR);         // *ppuVar6
  if (c->mem_r8(ptr) == 0xFF) {                          // sentinel -1 (0xFF as a byte) -> loop back
    ptr = ptr - c->mem_r8(ptr + 1);
    c->mem_w32(MODEARM_TABLE_PTR, ptr);
  }
  uint32_t vramBase = c->mem_r32(MODEARM_REC_BASE - 6);  // *(int*)(puVar8 - 3 shorts)
  uint8_t  index    = c->mem_r8(ptr);                     // bVar1 = **ppuVar6 (post sentinel-fix)
  c->mem_w8(MODEARM_COUNTDOWN, c->mem_r8(ptr + 1));       // reload countdown from the table's 2nd byte
  uint16_t x  = c->mem_r16(MODEARM_REC_BASE - 2);         // puVar8[-1]
  uint16_t y  = c->mem_r16(MODEARM_REC_BASE);             // *puVar8
  // stack struct handed to FUN_80081218: {x, y, w=0x10, h=1}. FUN_8013C0BC's OWN prologue is
  // `addiu sp,sp,-0x30` (disas-verified); the caller's frame (beh_seaside_prox_substate's own
  // -0x20, applied by the top-level wrapper below) is already folded into c->r[29] on entry, so
  // this mirrors ONLY this function's own -0x30 before taking the +0x10 local-var offset.
  uint32_t frameSp = c->r[29] - 0x30;
  uint32_t sp0 = frameSp + 0x10;
  c->mem_w16(sp0 + 0, x);
  c->mem_w16(sp0 + 2, y);
  c->mem_w16(sp0 + 4, 0x10);
  c->mem_w16(sp0 + 6, 1);
  c->r[4] = sp0;
  c->r[5] = vramBase + (uint32_t)index * 0x20;
  uint32_t saveSp = c->r[29]; c->r[29] = frameSp;
  rec_dispatch(c, 0x80081218u);                           // Asset::uploadImage / GpuState upload (EngineOverride)
  c->r[29] = saveSp;
  c->mem_w32(MODEARM_TABLE_PTR, ptr + 2);                 // advance the duration/index cursor
}

// ===============================================================================================
// FUN_8013B70C — event-slot proximity/commit check driving the INIT draw. RE'd 1:1 from disas
// 0x8013b70c..0x8013b868. `data` mirrors the recomp's unclobbered $a1 (== the caller's 2nd arg,
// INIT_DRAW_DATA) carried through unchanged to the FUN_8013B534 leaf call.
void drawInit(Core* c, uint32_t obj, uint32_t data) {
  bool found = false;
  uint8_t count = c->mem_r8(EVENT_SLOT_COUNT);
  if (count != 0) {
    for (uint32_t k = 0; k < count; ++k) {
      if ((c->mem_r8(EVENT_SLOT_TABLE + k) & 0x7Fu) == c->mem_r16(obj + 0x7E)) { found = true; break; }
    }
  }
  if (!found) { c->mem_w8(obj + 4, 3); return; }

  if (((c->mem_r8(GATE_BFA19) >> (c->mem_r8(obj + 3) & 0x1F)) & 1u) != 0) {
    c->mem_w8(obj + 4, 3);
    return;
  }
  c->r[4] = obj; c->r[5] = data; rec_dispatch(c, 0x8013B534u);   // FUN_8013B534(obj) — a1 unchanged
  if (c->r[2] != 0) { c->mem_w8(obj + 4, 3); return; }

  if (c->mem_r8(EVENT_SLOT_ARM_BIT) == c->mem_r16(obj + 0x7A)) {
    c->mem_w16(c->mem_r32(obj + 0xC4) + 0xA, 0x800);
    c->mem_w16(c->mem_r32(obj + 0xC8) + 0xA, 0x800);
    c->mem_w8(obj + 4, 1);
    c->mem_w8(obj + 5, 0);
    c->mem_w8(obj + 0x5E, 2);
    return;
  }
  if (((c->mem_r8(GATE_BFA17) >> (c->mem_r8(obj + 3) & 0x1F)) & 1u) == 0) {
    c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
    return;
  }
  c->mem_w16(c->mem_r32(obj + 0xC4) + 0xA, 0x800);
  c->mem_w16(c->mem_r32(obj + 0xC8) + 0xA, 0x800);
  c->mem_w8(obj + 0, 1);
  c->mem_w8(obj + 4, 1);
  c->mem_w8(obj + 5, 0);
  c->mem_w8(obj + 0x5E, 1);
}

// ===============================================================================================
// FUN_8013B868 — sub-behavior A: arm SFX loop, face the camera, spin up an approach SFX, then
// creep the two linked node cameras' FOV/zoom field until it crosses -0x800, disarm. RE'd 1:1
// from disas 0x8013b868..0x8013bab0 (5-way switch on obj+5).
void subA(Core* c, uint32_t obj) {
  switch (c->mem_r8(obj + 5)) {
    case 0: {
      c->mem_w8(0x1F800137u, 2);
      c->mem_w8(0x800BF841u, 1);
      c->r[4] = 8; rec_dispatch(c, 0x8006E1C0u);                    // FUN_8006E1C0(8) — sound-loop start (leaf)
      c->r[4] = obj + 0x2C; rec_dispatch(c, 0x8006CBA8u);           // CutsceneCamera::initSeedGrp (EngineOverride)
      c->mem_w16(0x1F8000D6u, c->mem_r16(obj + 0x32));
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      break;
    }
    case 1: {
      if ((c->mem_r8(0x800E806Eu) & 3) != 3) return;
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      int32_t dy = -(int32_t)(int16_t)(c->mem_r16(obj + 0x36) - c->mem_r16(G_eb4 + 2));
      int32_t dx =  (int32_t)(int16_t)(c->mem_r16(obj + 0x2E) - c->mem_r16(G_eac + 2));
      int32_t ang = trigOf(c).ratan2(dy, dx);
      int32_t face = Trig::angleCmp((int16_t)ang, c->mem_r16s(obj + 0x60), 0);
      c->mem_w8(obj + 0x46, (uint8_t)face);
      eng(c).sfx.trigger(0x17, -0x12, 0x1e);
      return;   // matches the disas: this case returns, not break-to-common-tail
    }
    case 2: {
      if (c->mem_r8(obj + 0x46) == 0) {
        c->mem_w16(c->mem_r32(obj + 0xC4) + 0xA, (uint16_t)(c->mem_r16s(c->mem_r32(obj + 0xC4) + 0xA) - 0x40));
        c->mem_w16(c->mem_r32(obj + 0xC8) + 0xA, (uint16_t)(c->mem_r16s(c->mem_r32(obj + 0xC8) + 0xA) + 0x40));
        if (c->mem_r16s(c->mem_r32(obj + 0xC4) + 0xA) >= -0x7FF) return;
      } else {
        c->mem_w16(c->mem_r32(obj + 0xC4) + 0xA, (uint16_t)(c->mem_r16s(c->mem_r32(obj + 0xC4) + 0xA) + 0x40));
        c->mem_w16(c->mem_r32(obj + 0xC8) + 0xA, (uint16_t)(c->mem_r16s(c->mem_r32(obj + 0xC8) + 0xA) - 0x40));
        if (c->mem_r16s(c->mem_r32(obj + 0xC4) + 0xA) < 0x800) return;
      }
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      break;
    }
    case 3: {
      c->mem_w16(obj + 0x40, 0x1e);
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      break;
    }
    case 4: {
      int32_t left = c->mem_r16s(obj + 0x40) - 1;
      c->mem_w16(obj + 0x40, (uint16_t)left);
      if (left == 0) {
        c->mem_w8(0x1F800137u, 0);
        c->mem_w8(0x800BF841u, 0);
        rec_dispatch(c, 0x8006E1E4u);                               // FUN_8006E1E4() — sound-loop stop (leaf)
        c->mem_w8(GATE_BFA17, (uint8_t)(c->mem_r8(GATE_BFA17) | (1u << (c->mem_r8(obj + 3) & 0x1F))));
        c->mem_w8(obj + 0x5E, 1);
        c->mem_w8(obj + 0, 1);
        c->mem_w8(obj + 5, 0);
      }
      return;   // default-arm of the switch falls to this same "return" per the disas
    }
    default:
      return;
  }
}

// ===============================================================================================
// FUN_8013BAB0 — sub-behavior B: camera-nudge / facing machine. Nudges the shared camera-G
// position toward the actor and zeroes the zoom/shake fields; RE'd 1:1 from disas
// 0x8013bab0..0x8013bcc8.
void subB(Core* c, uint32_t obj) {
  uint8_t sub = c->mem_r8(obj + 5);
  if (sub == 1) {
    int16_t hi = (int16_t)c->mem_r16(G_eb2);
    if (c->mem_r16s(obj + 0x32) < hi) {
      int16_t nv = (int16_t)(hi - 0x14);
      c->mem_w16(G_eb2, (uint16_t)nv);
      if (nv < c->mem_r16s(obj + 0x32)) c->mem_w16(G_eb2, (uint16_t)c->mem_r16s(obj + 0x32));
    } else {
      int16_t nv = (int16_t)(hi + 0x14);
      if (hi < c->mem_r16s(obj + 0x32)) {
        c->mem_w16(G_eb2, (uint16_t)nv);
        if (c->mem_r16s(obj + 0x32) < nv) c->mem_w16(G_eb2, (uint16_t)c->mem_r16s(obj + 0x32));
      }
    }
    c->mem_w32(G_eac, c->mem_r32(G_eac) + (int32_t)c->mem_r16s(obj + 0x44) * (int32_t)c->mem_r16s(G_ec8));
    c->mem_w32(G_eb4, c->mem_r32(G_eb4) + (int32_t)c->mem_r16s(obj + 0x44) * (int32_t)c->mem_r16s(G_ecc));
    c->r[4] = G; rec_dispatch(c, 0x8004766Cu);                      // FUN_8004766C(&G) — tile-move step (leaf)
    c->r[4] = G; rec_dispatch(c, 0x80048750u);                      // FUN_80048750(&G) — anim-link tick (leaf)
    c->mem_w16(G_f38, (uint16_t)(c->mem_r16s(G_f38) - 0x80));
    c->mem_w16(G_f3a, (uint16_t)(c->mem_r16s(G_f3a) - 0x80));
    c->mem_w16(G_f3c, (uint16_t)(c->mem_r16s(G_f3c) - 0x80));
    if (c->mem_r16s(G_f38) < 0x101) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
    return;
  }
  if (sub < 2) {
    if (sub == 0 && c->mem_r8(obj + 0x29) != 0) {
      c->mem_w8(obj + 5, 1);
      int32_t dy = -(int32_t)(int16_t)(c->mem_r16(obj + 0x36) - (int16_t)(c->mem_r32(G_eb4) >> 16));
      int32_t dx =  (int32_t)(int16_t)(c->mem_r16(obj + 0x2E) - (int16_t)(c->mem_r32(G_eac) >> 16));
      int32_t ang = trigOf(c).ratan2(dy, dx);
      int32_t face = Trig::angleCmp((int16_t)ang, c->mem_r16s(obj + 0x60), 0);
      c->mem_w8(obj + 0x46, (uint8_t)face);
      c->mem_w16(obj + 0x44, (face != 0) ? 0xf800 : 0x0800);
    }
    return;
  }
  // sub == 2
  c->mem_w8(obj + 5, 3);
  c->mem_w8(0x1F800236u, 7);
  c->mem_w8(0x800BF839u, 1);
  c->mem_w16(0x800BF83Au, (uint16_t)(c->mem_r8(obj + 0xBF) << 4));
}

// ===============================================================================================
// FUN_8013BCC8 — sub-behavior C: cutscene data blast into the interface scratch block (0x1F8000Dx)
// + camera-G area-transition seed, then (case 3, on a 0x3C-frame timer) two collectible-style
// FUN_80027144 packet emits per linked node with FUN_80074590 SFX between, tagging both linked
// nodes' render-order byte. RE'd 1:1 from disas 0x8013bcc8..0x8013c0bc.
void subC(Core* c, uint32_t obj, uint32_t data) {
  switch (c->mem_r8(obj + 5)) {
    case 0: {
      c->mem_w16(obj + 0x40, 0x3c);
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      [[fallthrough]];
    }
    case 1: {
      if (c->mem_r8(0x800BF80Fu) != 0) break;   // DAT_800bf80c._3_1_ == byte 3 of the 4-byte word == +0xF
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      c->mem_w8(0x1F800137u, 2);
      c->mem_w8(GATE_BFA19, (uint8_t)(c->mem_r8(GATE_BFA19) | (1u << (c->mem_r8(obj + 3) & 0x1F))));
      c->r[4] = 8; rec_dispatch(c, 0x8006E1C0u);                    // FUN_8006E1C0(8) — sound-loop start (leaf)
      c->mem_w16(0x1F8000D2u, c->mem_r16(data + 0));
      c->mem_w16(0x1F8000D6u, c->mem_r16(data + 2));
      c->mem_w16(0x1F8000DAu, c->mem_r16(data + 4));
      c->mem_w16(0x800E8040u + 2, c->mem_r16(data + 6));
      c->mem_w16(0x800E8044u + 2, c->mem_r16(data + 8));
      c->mem_w16(0x800E8048u + 2, c->mem_r16(data + 0xA));
      c->mem_w8(G + 4, 4);
      c->mem_w8(G + 5, 0x21);
      c->mem_w8(G + 6, 0);
      break;
    }
    case 2: {
      if (c->mem_r8(0x800E806Eu) == 3) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      break;
    }
    case 3: {
      int16_t left = (int16_t)(c->mem_r16s(obj + 0x40) - 1);
      c->mem_w16(obj + 0x40, (uint16_t)left);
      if (left == -1) {
        c->mem_w16(obj + 0x40, 0x3c);
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
        // stack record handed to FUN_8003116C: {x=obj+0x2E, y=obj+0x32+0x80, z=obj+0x36}. FUN_8013BCC8's
        // OWN prologue is `addiu sp,sp,-0x30` (disas-verified); the caller's -0x20 frame (the top-level
        // wrapper below) is already folded into c->r[29] on entry, so this mirrors only this function's
        // own -0x30 before taking the +0x10 local-var offset (matches the disas's `addiu a1,sp,0x10`).
        uint32_t frameSp = c->r[29] - 0x30;
        uint32_t sp0 = frameSp + 0x10;
        c->mem_w16(sp0 + 2, c->mem_r16(obj + 0x2E));
        c->mem_w16(sp0 + 6, (uint16_t)(c->mem_r16s(obj + 0x32) + 0x80));
        c->mem_w16(sp0 + 0xA, c->mem_r16(obj + 0x36));
        uint32_t saveSp = c->r[29]; c->r[29] = frameSp;

        c->r[4] = 0x15; c->r[5] = sp0; c->r[6] = (uint32_t)-0x32;
        rec_dispatch(c, 0x8003116Cu);                               // Spawn::spawnAndInitBody (EngineOverride)
        uint32_t rec1 = c->r[2];
        c->mem_w8(rec1 + 0x28, (uint8_t)(c->mem_r8(rec1 + 0x28) | 0x80));

        c->r[4] = 0x24; c->r[5] = sp0; c->r[6] = (uint32_t)-0x32;
        rec_dispatch(c, 0x8003116Cu);
        uint32_t rec2 = c->r[2];
        c->mem_w8(rec2 + 0x28, (uint8_t)(c->mem_r8(rec2 + 0x28) | 0x80));

        c->r[29] = saveSp;

        uint32_t link1 = c->mem_r32(obj + 0xC0);
        c->r[4] = c->mem_r32(link1 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x800; c->r[7] = 8;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);
        c->r[4] = c->mem_r32(link1 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x600; c->r[7] = 0x18;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);

        uint32_t link2 = c->mem_r32(obj + 0xC4);
        c->r[4] = c->mem_r32(link2 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x800; c->r[7] = 8;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);
        c->r[4] = c->mem_r32(link2 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x600; c->r[7] = 0x18;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);

        uint32_t link3 = c->mem_r32(obj + 0xC8);
        c->r[4] = c->mem_r32(link3 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x800; c->r[7] = 8;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);
        c->r[4] = c->mem_r32(link3 + 0x40); c->r[5] = obj + 0x2C; c->r[6] = 0x600; c->r[7] = 0x18;
        rec_dispatch(c, 0x80027144u);
        eng(c).sfx.trigger(0xc, 0, 0);
      } else {
        uint32_t link1 = c->mem_r32(obj + 0xC0);
        c->mem_w16(link1 + 2, (uint16_t)((c->mem_r8(0x1F80017Cu) & 1) * 6));
        c->r[4] = 0; rec_dispatch(c, 0x8009A450u);                  // FUN_8009A450() — PRNG (leaf)
        c->mem_w16(link1, (uint16_t)(((int32_t)(c->r[2] & 3) - 2) * 6));
      }
      break;
    }
    case 4: {
      int16_t left = (int16_t)(c->mem_r16s(obj + 0x40) - 1);
      c->mem_w16(obj + 0x40, (uint16_t)left);
      if (left == -1) {
        c->mem_w8(obj + 4, 3);
        c->mem_w8(0x1F800137u, 0);
        rec_dispatch(c, 0x8006E1E4u);                               // FUN_8006E1E4() — sound-loop stop (leaf)
      }
      c->mem_w8(obj + 1, 0);
      break;
    }
    default:
      break;
  }
}

// -- State bodies -----------------------------------------------------------------------------
// INIT (outer 0) — internal sub-state at node+5. Only sub 0/1 issue the init-draw; sub 2 is the
// proximity ARM check that transitions the actor to RUNNING when in range.
void state_init(Core* c, uint32_t obj) {
  const uint8_t sub = c->mem_r8(obj + 5);
  if (sub == 2) {
    if (cam_distance(c, obj) > PROXIMITY_MAX)       return;
    if (c->mem_r8(GATE_SPAD_27A)     != 0)          return;
    if (c->mem_r8(MASTER_G_BYTE)     == 6)          return;   // recomp: `if (==6) return` — active outside mode 6
    if (c->mem_r8(GATE_BF80D)        != 0)          return;
    c->mem_w8(obj + 4,    1);   // → RUNNING
    c->mem_w8(obj + 5,    0);
    c->mem_w8(obj + 0x5E, 0);
    return;
  }
  if (sub == 0) {
    modeArm(c, 0);
    c->mem_w8 (obj + 0xBF, 0xE0);
    c->mem_w16(obj + 0x7E, 0x1B);
    c->mem_w16(obj + 0x7C, 0x400);
    c->mem_w16(obj + 0x7A, 0x0B);
    c->mem_w16(obj + 0x78, 0x30);
    c->mem_w8 (obj + 5, (uint8_t)(sub + 1));        // advance sub 0 → 1
    // fall through into the shared INIT draw call below
  } else if (sub != 1) {
    return;                                          // out-of-range sub-state — no-op
  }
  drawInit(c, obj, INIT_DRAW_DATA);                  // shared sub-0 (after advance) + sub-1 tail
}

// RUNNING (outer 1) — cull → optional mode toggle + post-cull update → node[+0x5E] sub-dispatch,
// each sub-body clears node+0x29 on exit (matches the recomp's shared clear).
void state_running(Core* c, uint32_t obj) {
  if (bounds_cull(c, obj) != 0) {
    modeArm(c, 1);
    post_cull_upd(c, obj);
  }
  const uint8_t sub = c->mem_r8(obj + 0x5E);
  if (sub == 0)      subA(c, obj);
  else if (sub == 1) subB(c, obj);
  else if (sub == 2) subC(c, obj, SUB_C_DATA);
  // else: no sub-behavior; still clears node+0x29 below
  c->mem_w8(obj + 0x29, 0);
}

void beh_seaside_prox_substate_body(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t  st  = c->mem_r8(obj + 4);
  if (st == 1)      state_running(c, obj);
  else if (st == 0) state_init   (c, obj);
  else if (st == 3) eng(c).spawn.despawn(obj);
  // else: no-op (matches recomp's `bVar1 != 2 && bVar1 == 3` guard)
}

}  // namespace

// FUN_8013C1DC's own prologue is `addiu sp,sp,-0x20` (disas-verified). modeArm/subC's own further
// -0x30 frames are computed RELATIVE TO c->r[29] as seen at their own entry — mirroring this
// function's -0x20 here (for its whole body, matching how the two direct-jal call sites at
// 0x8013c274/0x8013c354/0x8013c3c4 all execute with this frame still live) keeps their stack-
// scratch addresses byte-exact with the real recomp instead of landing 0x20 bytes off (which would
// show up as a genuine SBS RAM divergence at both the wrong and the right address).
void beh_seaside_prox_substate(Core* c) {
  c->r[29] -= 0x20;
  beh_seaside_prox_substate_body(c);
  c->r[29] += 0x20;
}
