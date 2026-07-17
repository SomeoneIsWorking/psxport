// game/object/actor_sm_reward.cpp — PC-native bodies for the reward/tally window actor SM family.
// See actor_sm_reward.h for the RE summary and the wiring rationale.
//
// RE SOURCE: Ghidra headless decompile (tools/decomp.sh, project scratch/ghidra/main_ram) cross-
// checked against tools/disas.py --mem --all traces for every bare global (DAT_*) width, plus a full
// manual instruction-level trace for FUN_8004B150 (confirmed byte-for-byte against the Ghidra C). All
// object-relative field widths come straight from Ghidra's explicit casts (`*(short*)(obj+N)` etc.),
// which are reliable; only the bare-symbol global widths needed the disas.py cross-check (Ghidra names
// them DAT_<addr> without a declared type in the decompiled text). See docs/engine_re.md's move-and-
// collide section for the shared grid-probe leaves (FUN_8004766C etc.) this family calls.
#include "core.h"
#include "game_ctx.h"
#include "actor_sm_reward.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "game.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t);                     // hybrid call: override if wired, else substrate
extern void shard_set_override(uint32_t, void (*)(Core*)); // wire the recompiler's OWN g_override[] table
                                                            // (needed because our sole caller, FUN_8004AAC4,
                                                            // is substrate and calls us by direct C call —
                                                            // see actor_sm_reward.h "WIRING").

// The recompiled bodies we redirect FROM (only reached on an SBS psx_fallback core B — the pure
// substrate reference — or when running outside SBS with GATE/RENDER_PSX; see the ov_* gates below).
extern void gen_func_80049A60(Core*);
extern void gen_func_80049E54(Core*);
extern void gen_func_8004A3D4(Core*);
extern void gen_func_8004B150(Core*);
extern void gen_func_8004B208(Core*);

namespace {

enum { R_A0 = 4, R_A1 = 5, R_A2 = 6, R_A3 = 7, R_V0 = 2 };

// Callee addresses (all still substrate/leaves except FN_77ACC, which is Cull::cullWrap77acc — LIVE —
// reached uniformly through rec_dispatch like every other callee here).
constexpr uint32_t FN_766C   = 0x8004766Cu;  // grid resolve-in-place (leaf; docs/engine_re.md)
constexpr uint32_t FN_49250  = 0x80049250u;  // directional grid snap wrapper (leaf)
constexpr uint32_t FN_493E8  = 0x800493E8u;  // directional grid snap wrapper (leaf)
constexpr uint32_t FN_41194  = 0x80041194u;  // directional snap + camera-anchor seed (leaf)
constexpr uint32_t FN_77ACC  = 0x80077ACCu;  // Cull::cullWrap77acc (LIVE)
constexpr uint32_t FN_72114  = 0x80072114u;  // tally digit/render update (leaf)
constexpr uint32_t FN_D4C4   = 0x8004D4C4u;  // give-and-flag item (leaf; Inventory::giveAndFlagBody body)
constexpr uint32_t FN_D4F4   = 0x8004D4F4u;  // give-only item (leaf; Inventory::giveBody body)
constexpr uint32_t FN_ED0C   = 0x8004ED0Cu;  // inventory flag gate (leaf)
constexpr uint32_t FN_ED94   = 0x8004ED94u;  // announcer cue queue (leaf; Engine::announcerCue body)
constexpr uint32_t FN_40B48  = 0x80040B48u;  // UI/event side-effect (leaf, not independently RE'd)
constexpr uint32_t FN_40C00  = 0x80040C00u;  // UI/event side-effect (leaf, not independently RE'd)
constexpr uint32_t FN_D650   = 0x8004D650u;  // inventory-adjacent leaf (not independently RE'd)
constexpr uint32_t FN_67DA8  = 0x80067DA8u;  // leaf (not independently RE'd)
constexpr uint32_t FN_310F4  = 0x800310F4u;  // object-by-type finder (leaf; returns obj ptr in v0)
constexpr uint32_t FN_74590  = 0x80074590u;  // leaf (not independently RE'd)
constexpr uint32_t FN_115AEC = 0x80115AECu;  // leaf (not independently RE'd)
constexpr uint32_t FN_114F24 = 0x80114F24u;  // leaf (not independently RE'd)
constexpr uint32_t FN_71B44  = 0x80071B44u;  // leaf (not independently RE'd)

// --- WIDE-RE DRAFT callees (region 0x80070000-0x8007FFFF survey, 2026-07-08) ---
constexpr uint32_t FN_A624  = 0x8007A624u;   // leaf (not independently RE'd) -- "despawn/finalize" (state 3)
constexpr uint32_t FN_B3F4  = 0x8004B3F4u;   // Spawn::dropScoreGem (LIVE) -- fixed AP-gem values 100/200/500/1000/100000
constexpr uint32_t FN_A118  = 0x8004A118u;   // leaf (not independently RE'd; depended-on by beh_visibility_gate_dispatch)
constexpr uint32_t FN_A2A0  = 0x8004A2A0u;   // leaf (not independently RE'd; depended-on by beh_visibility_gate_dispatch)
constexpr uint32_t FN_B428  = 0x8004B428u;   // leaf (not independently RE'd; depended-on by beh_visibility_gate_dispatch)
constexpr uint32_t FN_517F8 = 0x800517F8u;   // GraphicsBind::renderUpdateBody (LIVE)
constexpr uint32_t FN_77B5C = 0x80077B5Cu;   // Animation::advanceLinkChain (LIVE)

// Globals used by ActorReward::resolvePosition (FUN_800702C0) -- two pointer-globals to the
// scene's tracked entities (already named G_800E7F50/G_800E7F5C elsewhere, e.g.
// game/ai/beh_a08_scene_actor.cpp, game/ai/beh_typed_anim_spawn.cpp -- consistently DEREFERENCED
// pointers, read via a base-register load at 0x800E7E80+0xD0/0xDC in the raw recompiled code).
constexpr uint32_t G_800E7F50 = 0x800E7F50u;
constexpr uint32_t G_800E7F5C = 0x800E7F5Cu;
constexpr uint32_t G_800E7ED6 = 0x800E7ED6u;  // i16 angle override used by resolvePosition case 0

// Globals (widths confirmed via tools/disas.py --mem --all + Ghidra's data-type DB; see file header).
constexpr uint32_t SC_1F80017C = 0x1F80017Cu; // u16 frame-parity/vblank tick (blink source)
constexpr uint32_t SC_1F8001A6 = 0x1F8001A6u; // u16 grid-probe result tag (floor bits)
constexpr uint32_t G_TALLY_CUR  = 0x800E7FEEu; // u16 tally CURRENT/target display value
constexpr uint32_t G_TALLY_PREV = 0x800E7FF0u; // u16 tally PREVIOUS latched value
constexpr uint32_t G_TALLY_BUSY = 0x800ED061u; // u8  tally busy-flags (bit 2)
constexpr uint32_t G_TALLY_CAP  = 0x800BF87Du; // u8  tally cap/max
constexpr uint32_t G_TALLY_CAP2 = 0x800BF87Cu; // u8  tally cap scratch (swapped with CAP on reset)
constexpr uint32_t G_BF870   = 0x800BF870u;  // u8  UI/dialog-mode byte
constexpr uint32_t G_BF816   = 0x800BF816u;  // u8  camera-lock gate byte
constexpr uint32_t G_BF817   = 0x800BF817u;  // u8  camera-lock id byte
constexpr uint32_t G_BF812   = 0x800BF812u;  // i16 camera-lock target Y
constexpr uint32_t G_E7FEB   = 0x800E7FEBu;  // u8  UI mode byte
constexpr uint32_t G_BF9CF   = 0x800BF9CFu;  // u8  event counter
constexpr uint32_t G_BFB24   = 0x800BFB24u;  // u8  one-shot gate A
constexpr uint32_t G_BFB26   = 0x800BFB26u;  // u8  one-shot gate B
constexpr uint32_t G_BFB28   = 0x800BFB28u;  // u8  one-shot gate C
constexpr uint32_t G_BF88C   = 0x800BF88Cu;  // u8  announcer id echo A
constexpr uint32_t G_E7EEC   = 0x800E7EECu;  // u8  announcer id mirror A
constexpr uint32_t G_BF88D   = 0x800BF88Du;  // u8  announcer id echo B
constexpr uint32_t G_E7EED   = 0x800E7EEDu;  // u8  announcer id mirror B
constexpr uint32_t G_BF874   = 0x800BF874u;  // u32 running score/point accumulator

// Shared frame-parity BLINK-BIT update, byte-identical across FUN_80049A60/8004B150/8004B208's
// common tail: side==0 toggles obj+0xd bit 0x20, side!=0 toggles bit 0x02, both gated on a 32-frame
// parity tick (SC_1F80017C & 0x1f == 0).
void applyBlinkTail(Core* c, uint32_t obj, uint32_t side) {
  bool tick = (c->mem_r16(SC_1F80017C) & 0x1f) == 0;
  uint8_t bit = (side == 0) ? 0x20u : 0x02u;
  uint8_t v = c->mem_r8(obj + 0xd);
  v = tick ? (uint8_t)(v | bit) : (uint8_t)(v & ~bit);
  c->mem_w8(obj + 0xd, v);
}

}  // namespace

// ActorReward::smBlinkA(c) — FUN_8004B150(obj a0, side a1). One-shot init (state -> 1, alive -> 1,
// seed color[+0x18..1a]) then the shared blink tail.
// ----------------------------------------------------------------------------------------------
void ActorReward::smBlinkA(Core* c) {
  const uint32_t obj  = c->r[R_A0];
  const uint32_t side = c->r[R_A1];
  if (c->mem_r8(obj + 5) == 0) {
    c->mem_w8(obj + 5, 1);
    c->mem_w8(obj + 0, 1);
    if (side == 0) {
      c->mem_w8(obj + 0x18, 0);
    } else {
      c->mem_w8(obj + 0x18, 0xff);
      c->mem_w8(obj + 0x19, 0xff);
      c->mem_w8(obj + 0x1a, 0xff);
    }
  }
  applyBlinkTail(c, obj, side);
}

// ActorReward::smBlinkB(c) — FUN_8004B208(obj a0, side a1). Same shape as smBlinkA, but the init
// also fires a directional grid snap (FUN_80041194) seeded from the obj+0x84/0x86 Y-target pair.
// ----------------------------------------------------------------------------------------------
void ActorReward::smBlinkB(Core* c) {
  const uint32_t obj  = c->r[R_A0];
  const uint32_t side = c->r[R_A1];
  if (c->mem_r8(obj + 5) == 0) {
    c->mem_w8(obj + 5, 1);
    c->mem_w8(obj + 0, 1);
    int32_t delta = c->mem_r16s(obj + 0x86) - c->mem_r16s(obj + 0x84);
    c->r[R_A0] = obj; c->r[R_A1] = (uint32_t)delta; c->r[R_A2] = 0; c->r[R_A3] = 0;
    rec_dispatch(c, FN_41194);
    if (side == 0) {
      c->mem_w8(obj + 0x18, 0);
    } else {
      c->mem_w8(obj + 0x18, 0xff);
      c->mem_w8(obj + 0x19, 0xff);
      c->mem_w8(obj + 0x1a, 0xff);
    }
  }
  applyBlinkTail(c, obj, side);
}

// ActorReward::smWindowScroll(c) — FUN_80049A60(obj a0, side a1). Scroll/fade sub-SM: states 0
// (init), 1 (scroll toward 0, probing the grid near the end of the ramp), 2 (post-scroll hold /
// re-probe / camera-lock branch), 3 (re-trigger a scroll-out), 6 (despawn check via the cull
// wrapper). Ends with the shared blink tail (applyBlinkTail), same as smBlinkA/B.
// ----------------------------------------------------------------------------------------------
void ActorReward::smWindowScroll(Core* c) {
  const uint32_t obj  = c->r[R_A0];
  const uint32_t side = c->r[R_A1];
  const uint8_t  state = c->mem_r8(obj + 5);

  switch (state) {
    case 0: {
      c->mem_w16(obj + 0x4a, 0xd000);
      c->mem_w16(obj + 0x50, 0x200);
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      if (side == 0) {
        c->mem_w8(obj + 0x18, 0);
      } else {
        c->mem_w8(obj + 0x18, 0xff);
        c->mem_w8(obj + 0x19, 0xff);
        c->mem_w8(obj + 0x1a, 0xff);
      }
      break;
    }
    case 1: {
      int16_t  prevScroll = c->mem_r16s(obj + 0x4a);
      uint16_t newScroll  = (uint16_t)(c->mem_r16(obj + 0x4a) + c->mem_r16(obj + 0x50));
      c->mem_w16(obj + 0x4a, newScroll);
      int32_t accum = (int32_t)c->mem_r32(obj + 0x30) + (int32_t)prevScroll * 0x100;
      c->mem_w32(obj + 0x30, (uint32_t)accum);
      int32_t iv = (int32_t)(int16_t)newScroll;
      if (iv < 0) {
        if (iv > -0x1800) c->mem_w8(obj + 0, 1);
        c->r[R_A0] = obj; rec_dispatch(c, FN_766C);
        c->r[R_A0] = obj; c->r[R_A1] = 0;
        c->r[R_A2] = (uint32_t)(int32_t)(-c->mem_r16s(obj + 0x84));
        rec_dispatch(c, FN_493E8);
        if (c->r[R_V0] != 0) {
          c->mem_w8(obj + 0, 1);
          c->mem_w16(obj + 0x4a, 0);
          c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
        }
      } else {
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      }
      break;
    }
    case 2: {
      int16_t scroll = c->mem_r16s(obj + 0x4a);
      int32_t accum  = (int32_t)c->mem_r32(obj + 0x30) + (int32_t)scroll * 0x100;
      c->mem_w32(obj + 0x30, (uint32_t)accum);
      if (scroll < 0x3000) {
        int16_t speed = c->mem_r16s(obj + 0x50);
        c->mem_w16(obj + 0x4a, (uint16_t)(int16_t)(scroll + speed));
      }
      if (c->mem_r8(obj + 0x29) == 0) {
        bool setState5 = false;
        if ((c->mem_r8(obj + 0x28) & 0x80) == 0) {
          c->r[R_A0] = obj; rec_dispatch(c, FN_766C);
          int16_t delta = (int16_t)(c->mem_r16(obj + 0x86) - c->mem_r16(obj + 0x84));
          c->r[R_A0] = obj; c->r[R_A1] = 0; c->r[R_A2] = (uint32_t)(int32_t)delta;
          rec_dispatch(c, FN_49250);
          uint32_t tag = c->r[R_V0];
          if (tag == 1) {
            setState5 = true;
          } else if (tag == 2) {
            uint16_t a6    = c->mem_r16(SC_1F8001A6);
            uint8_t  st870 = c->mem_r8(G_BF870);
            if ((a6 & 0x8000) != 0 && st870 != 8) {
              if (st870 != 1 || c->mem_r8(obj + 2) == 0) c->mem_w8(obj + 5, 6);
              break;  // no state-5, no scroll reset (matches the RE'd early break)
            }
            setState5 = true;
          }
          // tag == 0 (or anything else): no state change, falls to the shared tail below via break
        } else {
          if (c->mem_r8(G_E7FEB) == 8) {
            bool cond = (c->mem_r8(G_BF816) != 1)
                     || ((uint32_t)c->mem_r8(G_BF817) != c->mem_r16(obj + 0x6a))
                     || ((int32_t)c->mem_r16s(obj + 0x32)
                         + ((int32_t)c->mem_r16s(obj + 0x86) - (int32_t)c->mem_r16s(obj + 0x84))
                         < (int32_t)c->mem_r16s(G_BF812));
            if (cond) break;  // no state-5, no scroll reset
            c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(G_BF812)
                         - (c->mem_r16s(obj + 0x86) - c->mem_r16s(obj + 0x84))));
            setState5 = true;
          } else {
            c->r[R_A0] = obj; rec_dispatch(c, FN_766C);
            int16_t delta = (int16_t)(c->mem_r16(obj + 0x86) - c->mem_r16(obj + 0x84));
            c->r[R_A0] = obj; c->r[R_A1] = 0; c->r[R_A2] = (uint32_t)(int32_t)delta;
            rec_dispatch(c, FN_49250);
            if (c->r[R_V0] == 0) break;  // no state-5, no scroll reset
            setState5 = true;
          }
        }
        if (setState5) c->mem_w8(obj + 5, 5);
        c->mem_w16(obj + 0x4a, 0);
        break;
      }
      // obj+0x29 != 0 ("holding"): reset scroll, advance state by one.
      uint8_t st = c->mem_r8(obj + 5);
      c->mem_w16(obj + 0x4a, 0);
      c->mem_w8(obj + 5, (uint8_t)(st + 1));
      break;
    }
    case 3: {
      bool condA = (c->mem_r8(obj + 0x28) & 0x80) == 0;
      bool condB = (c->mem_r8(G_BF816) != 0) && ((uint32_t)c->mem_r8(G_BF817) == c->mem_r16(obj + 0x6a));
      if ((condA || condB) && c->mem_r8(obj + 0x29) == 0) {
        c->mem_w16(obj + 0x4a, 0x2000);
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) - 1));
      }
      break;
    }
    case 6: {
      int16_t scroll = c->mem_r16s(obj + 0x4a);
      int32_t accum  = (int32_t)c->mem_r32(obj + 0x30) + (int32_t)scroll * 0x100;
      c->mem_w32(obj + 0x30, (uint32_t)accum);
      if (scroll < 0x3000) {
        int16_t speed = c->mem_r16s(obj + 0x50);
        c->mem_w16(obj + 0x4a, (uint16_t)(int16_t)(scroll + speed));
      }
      c->r[R_A0] = obj;
      c->r[R_A1] = (uint32_t)(int32_t)c->mem_r16s(obj + 0x2e);
      c->r[R_A2] = (uint32_t)(int32_t)c->mem_r16s(obj + 0x32);
      c->r[R_A3] = (uint32_t)(int32_t)c->mem_r16s(obj + 0x36);
      rec_dispatch(c, FN_77ACC);
      if (c->r[R_V0] == 0) c->mem_w8(obj + 4, 3);
      break;
    }
    default:
      break;  // states 4/5/7... : no-op, matches the RE'd switch (only 0,1,2,3,6 are handled)
  }

  applyBlinkTail(c, obj, side);
}

// ActorReward::smTallyTick(c) — FUN_80049E54(obj a0, step a1) -> v0. Ticks the displayed tally value
// (G_TALLY_CUR) toward obj-local target by `step`, clamped to G_TALLY_CAP, latching the PREVIOUS
// value with a +/-1 "changed" adjustment; then a short settle countdown (obj+0x40) before reporting
// done (v0==1). obj+7 is this object's own tally sub-state (0 armed, 1 counting down).
// ----------------------------------------------------------------------------------------------
void ActorReward::smTallyTick(Core* c) {
  const uint32_t obj  = c->r[R_A0];
  const uint32_t step = c->r[R_A1];

  uint16_t before = c->mem_r16(G_TALLY_CUR);
  uint32_t ret = 0;

  if (c->mem_r8(obj + 7) == 0) {
    if ((int16_t)before < 1) {
      ret = 0;
    } else {
      c->mem_w8(obj + 7, 1);
      c->mem_w16(obj + 0x40, 0x10);
      uint32_t sum = (uint32_t)before + step;
      c->mem_w8(G_TALLY_BUSY, (uint8_t)(c->mem_r8(G_TALLY_BUSY) | 2));
      c->mem_w16(G_TALLY_CUR, (uint16_t)sum);
      bool changed = c->mem_r16(G_TALLY_PREV) != before;
      uint8_t cap = c->mem_r8(G_TALLY_CAP);
      if ((int32_t)(uint32_t)cap <= (int32_t)(int16_t)(uint16_t)sum) {
        c->mem_w16(G_TALLY_CUR, cap);
      }
      uint16_t cur = c->mem_r16(G_TALLY_CUR);
      c->mem_w16(G_TALLY_PREV, cur);
      if (changed) c->mem_w16(G_TALLY_PREV, (uint16_t)(cur - 1));
      uint32_t savedA1 = step;
      c->r[R_A0] = obj; c->r[R_A1] = savedA1;
      rec_dispatch(c, FN_72114);
      ret = 0;
    }
  } else {
    ret = 0;
    if (c->mem_r8(obj + 7) == 1) {
      int16_t cd = (int16_t)(c->mem_r16(obj + 0x40) - 1);
      c->mem_w16(obj + 0x40, (uint16_t)cd);
      ret = 0;
      if (cd == -1) {
        ret = 1;
        c->mem_w8(G_TALLY_BUSY, (uint8_t)(c->mem_r8(G_TALLY_BUSY) & 0xfd));
      }
    }
  }
  c->r[R_V0] = ret;
}

// ActorReward::smEventDispatch(c) — FUN_8004A3D4(obj a0) -> v0. Mechanical translation of the RE'd
// event-id switch (obj+0x68 is the event id): gives items / sets flags / queues announcer cues per
// id, then (always) re-tags any object of "type 0x1f, id -140" found via FN_310F4 with the visible-
// flag + this object's position, and pokes FN_74590(0x28,0,0). The id constants are NOT independently
// understood beyond their observed effects — every external call is dispatched by address so the
// exact substrate behavior is reproduced regardless.
// ----------------------------------------------------------------------------------------------
void ActorReward::smEventDispatch(Core* c) {
  const uint32_t obj = c->r[R_A0];

  if (c->mem_r16(G_TALLY_CUR) == 0) { c->r[R_V0] = 0; return; }

  int16_t  evt  = c->mem_r16s(obj + 0x68);
  int32_t  key  = (int32_t)(int16_t)(uint16_t)(evt - 1);

  switch (key) {
    case 0: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->mem_w8(G_BF88C, (uint8_t)evt);
      c->mem_w8(G_E7EEC, c->mem_r8(G_BF88C));
      c->r[R_A0] = 0x1e; rec_dispatch(c, FN_40C00);
      break;
    }
    case 5: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->mem_w8(G_BF88C, (uint8_t)evt);
      c->mem_w8(G_E7EEC, c->mem_r8(G_BF88C));
      c->r[R_A0] = 0x0f; rec_dispatch(c, FN_40C00);
      break;
    }
    case 3:
      c->r[R_A0] = 9; rec_dispatch(c, FN_40B48);
      [[fallthrough]];
    case 1: case 2: case 4: case 6: case 7: case 8: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->mem_w8(G_BF88C, (uint8_t)evt);
      c->mem_w8(G_E7EEC, c->mem_r8(G_BF88C));
      break;
    }
    case 10: {
      c->r[R_A0] = 0x0b; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->mem_w8(G_BF88D, 1);
      c->mem_w8(G_E7EED, c->mem_r8(G_BF88D));
      c->r[R_A0] = 0; rec_dispatch(c, FN_67DA8);
      c->mem_w8(G_BF9CF, (uint8_t)(c->mem_r8(G_BF9CF) + 1));
      break;
    }
    case 0xb: {
      c->r[R_A0] = 0x0c; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->mem_w8(G_BF88D, 2);
      c->mem_w8(G_E7EED, c->mem_r8(G_BF88D));
      c->r[R_A0] = 0; rec_dispatch(c, FN_67DA8);
      c->mem_w8(G_BF9CF, (uint8_t)(c->mem_r8(G_BF9CF) + 1));
      break;
    }
    case 0x2e: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = 0x18; rec_dispatch(c, FN_40B48);
      break;
    }
    case 0x46: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = 0x4d; rec_dispatch(c, FN_40B48);
      break;
    }
    case 0x56: {
      c->r[R_A0] = 0x4d; c->r[R_A1] = 2; rec_dispatch(c, FN_D4F4);
      c->r[R_A0] = 0x57; c->r[R_A1] = 2; rec_dispatch(c, FN_ED0C);
      c->r[R_A0] = 0x3e; rec_dispatch(c, FN_40B48);
      break;
    }
    case 0x69: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = 0x58; rec_dispatch(c, FN_40C00);
      break;
    }
    case 0x6a: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = 0x59; rec_dispatch(c, FN_40C00);
      break;
    }
    case 0x6f: case 0x71: case 0x73: {
      uint32_t cue;
      if (key == 0x6f) {
        if (c->mem_r8(G_BFB24) == 0) { c->r[R_A0]=0x70; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4); cue = 0x10; }
        else { c->r[R_A0]=0x6f; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4);
               c->r[R_A0]=0x70; c->r[R_A1]=1; rec_dispatch(c, FN_D650); cue = 0x11; }
      } else if (key == 0x71) {
        if (c->mem_r8(G_BFB26) == 0) { c->r[R_A0]=0x72; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4); cue = 0x12; }
        else { c->r[R_A0]=0x71; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4);
               c->r[R_A0]=0x72; c->r[R_A1]=1; rec_dispatch(c, FN_D650); cue = 0x13; }
      } else {
        if (c->mem_r8(G_BFB28) == 0) { c->r[R_A0]=0x74; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4); cue = 0x14; }
        else { c->r[R_A0]=0x73; c->r[R_A1]=1; rec_dispatch(c, FN_D4F4);
               c->r[R_A0]=0x74; c->r[R_A1]=1; rec_dispatch(c, FN_D650); cue = 0x15; }
      }
      c->r[R_A0] = cue; c->r[R_A1] = 0x41; rec_dispatch(c, FN_ED94);
      break;
    }
    case 0x7b: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = 0x50; rec_dispatch(c, FN_40B48);
      break;
    }
    case 0x88: {
      c->r[R_A0] = 0x60; c->r[R_A1] = 2; rec_dispatch(c, FN_D4F4);
      c->r[R_A0] = 0x89; c->r[R_A1] = 2; rec_dispatch(c, FN_ED0C);
      break;
    }
    case 0x89: {
      c->r[R_A0] = 0x7c; c->r[R_A1] = 3; rec_dispatch(c, FN_D4F4);
      c->r[R_A0] = 0x8a; c->r[R_A1] = 2; rec_dispatch(c, FN_ED0C);
      c->r[R_A0] = 0x50; rec_dispatch(c, FN_40B48);
      break;
    }
    case 0x90: {
      if (c->mem_r8(G_TALLY_CAP) == 8) c->mem_w8(G_TALLY_CAP, c->mem_r8(G_TALLY_CAP2));
      c->mem_w8(G_TALLY_CAP2, 0x10);
      c->mem_w16(G_TALLY_CUR, c->mem_r8(G_TALLY_CAP));
      c->mem_w16(G_TALLY_PREV, c->mem_r16(G_TALLY_CUR));
      c->r[R_A0] = 0x0f; c->r[R_A1] = 0x41; rec_dispatch(c, FN_ED94);
      c->r[R_A0] = 0x711; c->r[R_A1] = 0; rec_dispatch(c, FN_310F4);
      { uint32_t o2 = c->r[R_V0];
        if (o2 != 0) c->mem_w8(o2 + 0x28, (uint8_t)(c->mem_r8(o2 + 0x28) | 0x80)); }
      break;
    }
    case 0x95: case 0x96: case 0x98: case 0x99: {
      c->mem_w32(G_BF874, c->mem_r32(G_BF874) + 100000);
      c->r[R_A0] = obj; c->r[R_A1] = 100000; c->r[R_A2] = 0; rec_dispatch(c, FN_71B44);
      [[fallthrough]];
    }
    default: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      break;
    }
    case 0x9f: case 0xa0: case 0xa1: case 0xa2: {
      c->r[R_A0] = obj; rec_dispatch(c, FN_115AEC);
      break;
    }
    case 0xa5: {
      c->r[R_A0] = evt; c->r[R_A1] = 1; rec_dispatch(c, FN_D4C4);
      c->r[R_A0] = obj; rec_dispatch(c, FN_114F24);
      break;
    }
  }

  c->r[R_A0] = 0x1f; c->r[R_A1] = (uint32_t)-140;
  rec_dispatch(c, FN_310F4);
  uint32_t o2 = c->r[R_V0];
  if (o2 != 0) {
    c->mem_w8(o2 + 0x28, (uint8_t)(c->mem_r8(o2 + 0x28) | 0x80));
    c->mem_w16(o2 + 0x2c, c->mem_r16(obj + 0x2e));
    c->mem_w16(o2 + 0x2e, c->mem_r16(obj + 0x32));
    c->mem_w16(o2 + 0x30, c->mem_r16(obj + 0x36));
  }
  c->r[R_A0] = 0x28; c->r[R_A1] = 0; c->r[R_A2] = 0; rec_dispatch(c, FN_74590);
  c->r[R_V0] = 1;
}

// ==================================================================================================
// WIDE-RE DRAFT (2026-07-08, region 0x80070000-0x8007FFFF). UNWIRED, UNVERIFIED -- no override
// registration, no SBS run (see registerOverrides() below: these three are NOT in it). RE source:
// Ghidra headless decompile (scratch/ghidra/main_ram, tools/decomp.sh) cross-checked against the
// raw recompiled ground truth (generated/shard_0.c gen_func_80070018, shard_1.c gen_func_800702C0,
// shard_2.c gen_func_80070650) for exact field widths, arithmetic order, and (for update/
// resolvePosition) the guest-stack frame shape.
// ==================================================================================================

// ActorReward::update(c) — FUN_80070018(obj a0). The reward/score-gem actor's TOP-LEVEL per-frame
// state machine (obj+4): state 0 -> 1 (arm); state 1 -> position solve (resolvePosition/
// approachTargetX per obj+5 sub-mode) then advance obj+1 from the owner's script byte (owner =
// obj+0x10), holding at state 2 if obj+0xbe is set and the global camera-lock gate (G_BF816) is
// clear; state 2 -> either a countdown (obj+0x74) that re-advances obj+1 from the owner, or (once
// the countdown hits 0) a fixed-value gem dispatch keyed by obj+3 (smTallyTick x1/x2, dropScoreGem
// x100/200/500/1000/100000, or the three unnamed FN_A118/A2A0/B428 leaves) gated by obj+0x5f bits
// 0/1, else smEventDispatch; state 3 -> the FN_A624 finalize/despawn leaf. Every state that keeps
// running ends by branching obj+0x5f bit 0x80 into GraphicsBind::renderUpdateBody (bit clear) or
// Animation::advanceLinkChain (bit set).
// Guest frame: addiu sp,-0x20; spills s0(obj)=sp+16, ra=sp+24, s1(owner)=sp+20 (matches
// gen_func_80070018 exactly -- see ObjectTable::dispatchFaithful for the same mirror shape).
// ----------------------------------------------------------------------------------------------
namespace {
inline void updateEpilogue(Core* c) {
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = c->r[29] + 32;
}
}  // namespace

void ActorReward::update(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29];
  uint32_t s0 = c->r[16], s1 = c->r[17];

  c->r[29] = sp - 32;
  c->mem_w32(c->r[29] + 16, s0);           // sw s0,0x10(sp) -- LIVE incoming s0
  c->r[16] = c->r[R_A0];                   // s0 = obj
  c->mem_w32(c->r[29] + 24, ra);           // sw ra,0x18(sp)
  c->mem_w32(c->r[29] + 20, s1);           // sw s1,0x14(sp) -- LIVE incoming s1

  const uint32_t obj = c->r[16];
  uint8_t state = c->mem_r8(obj + 4);
  uint32_t flags5f = 0;
  bool haveFlags = false;

  if (state == 1) {
    if (c->mem_r8s(obj + 0xbe) != 0 && c->mem_r8(G_BF816) == 0) {
      c->mem_w8(obj + 4, 2);
      updateEpilogue(c);
      return;
    }
    c->r[17] = c->mem_r32(obj + 0x10);     // s1 = owner
    if (c->mem_r8(obj + 5) == 0) {
      // ra mirror: gen_func_80070018 sets c->r[31]=0x800700C8u before func_800702C0(c) --
      // resolvePosition's own frame spills this ra at its +32 slot; must byte-match.
      c->r[R_A0] = obj; c->r[31] = 0x800700C8u; ActorReward::resolvePosition(c);
    } else if (c->mem_r8(obj + 5) == 1) {
      // ra mirror: ground truth sets c->r[31]=0x800700D8u before func_80070650(c).
      c->r[R_A0] = obj; c->r[31] = 0x800700D8u; ActorReward::approachTargetX(c);
    }
    int8_t next = (int8_t)c->mem_r8(c->r[17] + 1);
    c->mem_w8(obj + 1, (uint8_t)next);
    if (next == 0) { updateEpilogue(c); return; }
    flags5f = c->mem_r8(obj + 0x5f);
    haveFlags = true;
  } else {
    if (state < 2) {
      if (state != 0) { updateEpilogue(c); return; }
      c->mem_w8(obj + 4, 1);
      updateEpilogue(c);
      return;
    }
    if (state != 2) {
      if (state != 3) { updateEpilogue(c); return; }
      // ra mirror: ground truth sets c->r[31]=0x800702ACu before func_8007A624(c); FN_A624
      // spills it at its own +20 frame slot -- must byte-match.
      c->r[R_A0] = obj; c->r[31] = 0x800702ACu; rec_dispatch(c, FN_A624);
      updateEpilogue(c);
      return;
    }
    if (c->mem_r16s(obj + 0x74) == 0) {
      if ((c->mem_r8(obj + 0x5f) & 1) != 0) {
        if ((c->mem_r8(obj + 0x5f) & 2) != 0) {
          // ra mirror (per-case): ground truth sets a distinct c->r[31] retaddr right before each
          // jump-table target -- every callee here (smTallyTick, dropScoreGem, FN_A118/A2A0/B428)
          // spills the incoming ra at its own frame's +16/+20 slot, so the exact literal must be
          // reproduced or the guest stack byte at that slot diverges.
          uint32_t result = 0;
          switch (c->mem_r8(obj + 3)) {
            case 0:    c->r[R_A0] = obj; c->r[R_A1] = 1;      c->r[31] = 0x800701C8u; ActorReward::smTallyTick(c); result = c->r[R_V0]; break;
            case 1:    c->r[R_A0] = obj; c->r[R_A1] = 2;      c->r[31] = 0x800701DCu; ActorReward::smTallyTick(c); result = c->r[R_V0]; break;
            case 4:    c->r[R_A0] = obj; c->r[R_A1] = 100;    c->r[31] = 0x800701F0u; rec_dispatch(c, FN_B3F4);  result = c->r[R_V0]; break;
            case 5:    c->r[R_A0] = obj; c->r[R_A1] = 200;    c->r[31] = 0x80070204u; rec_dispatch(c, FN_B3F4);  result = c->r[R_V0]; break;
            case 6:    c->r[R_A0] = obj; c->r[R_A1] = 500;    c->r[31] = 0x80070218u; rec_dispatch(c, FN_B3F4);  result = c->r[R_V0]; break;
            case 7:    c->r[R_A0] = obj; c->r[R_A1] = 1000;   c->r[31] = 0x8007022Cu; rec_dispatch(c, FN_B3F4);  result = c->r[R_V0]; break;
            case 0xb:  c->r[R_A0] = obj; c->r[R_A1] = 100000; c->r[31] = 0x80070244u; rec_dispatch(c, FN_B3F4);  result = c->r[R_V0]; break;
            case 0xf:  c->r[R_A0] = obj;                      c->r[31] = 0x80070254u; rec_dispatch(c, FN_A118);  result = c->r[R_V0]; break;
            case 0x10: c->r[R_A0] = obj;                      c->r[31] = 0x80070264u; rec_dispatch(c, FN_A2A0);  result = c->r[R_V0]; break;
            case 0x11: c->r[R_A0] = obj;                      c->r[31] = 0x80070274u; rec_dispatch(c, FN_B428);  result = c->r[R_V0]; break;
            default: break;
          }
          if (result == 0) { updateEpilogue(c); return; }
          c->mem_w8(obj + 4, 3);
          updateEpilogue(c);
          return;
        }
        // ra mirror: ground truth sets c->r[31]=0x80070290u before func_8004A3D4(c).
        c->r[R_A0] = obj; c->r[31] = 0x80070290u; ActorReward::smEventDispatch(c);   // same class, direct call
        if (c->r[R_V0] == 0) { updateEpilogue(c); return; }
      }
      c->mem_w8(obj + 4, 3);
      updateEpilogue(c);
      return;
    }
    c->mem_w16(obj + 0x74, (uint16_t)(c->mem_r16s(obj + 0x74) - 1));
    c->r[17] = c->mem_r32(obj + 0x10);     // s1 = owner
    int8_t next = (int8_t)c->mem_r8(c->r[17] + 1);
    c->mem_w8(obj + 1, (uint8_t)next);
    if (next == 0) { updateEpilogue(c); return; }
    flags5f = c->mem_r8(obj + 0x5f);
    haveFlags = true;
  }

  (void)haveFlags;  // always true on every fallthrough to here (asserted by RE, kept for clarity)
  // ra mirror: ground truth sets c->r[31]=0x80070168u before func_800517F8(c) (renderUpdateBody
  // IS framed -- see GraphicsBind::renderUpdateBody -- and its nested propagateRotmat() tail call
  // spills whatever's in c->r[31] into its own frame), or 0x80070158u before func_80077B5C(c).
  if ((flags5f & 0x80) == 0) {
    c->r[R_A0] = obj; c->r[31] = 0x80070168u; rec_dispatch(c, FN_517F8);  // GraphicsBind::renderUpdateBody
  } else {
    c->r[R_A0] = obj; c->r[31] = 0x80070158u; rec_dispatch(c, FN_77B5C);  // Animation::advanceLinkChain
  }
  updateEpilogue(c);
}

// ActorReward::resolvePosition(c) — FUN_800702C0(obj a0). Solves obj+0x2e/0x32/0x36 (x/y/z) from
// one of 8 position sources selected by obj+0x5e (the owner's various linked-entity slots +0xc0/
// 0xd0/0xdc/0xe4, or the midpoint of the two scene-tracked entities G_800E7F50/G_800E7F5C), each
// case optionally overriding the trailing radial-offset angle/radius (obj+0x62 amplitude,
// owner+0x56 angle by default) applied via rcos/rsin at the tail (identical to the tail every case
// falls into). Guest frame: addiu sp,-0x28; spills s2(obj)=sp+24, ra=sp+32, s3(radius applied
// count)=sp+28, s1(angle)=sp+20, s0(owner)=sp+16 (matches gen_func_800702C0 exactly).
// ----------------------------------------------------------------------------------------------
namespace {
inline void resolvePositionEpilogue(Core* c) {
  c->r[31] = c->mem_r32(c->r[29] + 32);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = c->r[29] + 40;
}
}  // namespace

void ActorReward::resolvePosition(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29];
  uint32_t s0 = c->r[16], s1 = c->r[17], s2 = c->r[18], s3 = c->r[19];

  c->r[29] = sp - 40;
  c->mem_w32(c->r[29] + 24, s2);
  c->r[18] = c->r[R_A0];                  // s2 = obj
  c->mem_w32(c->r[29] + 32, ra);
  c->mem_w32(c->r[29] + 28, s3);
  c->mem_w32(c->r[29] + 20, s1);
  c->mem_w32(c->r[29] + 16, s0);

  const uint32_t obj = c->r[18];
  int32_t  radius = c->mem_r16s(obj + 0x62);
  uint32_t owner  = c->mem_r32(obj + 0x10);
  c->r[16] = owner;
  int32_t angle = c->mem_r16s(owner + 0x56);

  uint8_t sel = c->mem_r8(obj + 0x5e);
  if (sel < 8) {
    switch (sel) {
      case 0: {
        uint32_t entA = c->mem_r32(G_800E7F50), entB = c->mem_r32(G_800E7F5C);  // pointer globals, deref
        int32_t sumX = (int32_t)c->mem_r32(entA + 0x2c) + (int32_t)c->mem_r32(entB + 0x2c);
        c->mem_w16(obj + 0x2e, (uint16_t)(sumX / 2));
        int32_t sumY = (int32_t)c->mem_r32(entA + 0x30) + (int32_t)c->mem_r32(entB + 0x30);
        c->mem_w16(obj + 0x32, (uint16_t)((int32_t)(sumY / 2) + c->mem_r16s(obj + 0x60)));
        int32_t sumZ = (int32_t)c->mem_r32(entA + 0x34) + (int32_t)c->mem_r32(entB + 0x34);
        c->mem_w16(obj + 0x36, (uint16_t)(sumZ / 2));
        radius = 0x20;
        angle  = c->mem_r16s(G_800E7ED6);
        break;
      }
      case 1: {
        uint32_t e = c->mem_r32(owner + 0xdc);
        c->mem_w16(obj + 0x2e, c->mem_r16(e + 0x2c));
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(e + 0x30) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, c->mem_r16(e + 0x34));
        break;
      }
      case 2: {
        uint32_t eA = c->mem_r32(owner + 0xd0), eB = c->mem_r32(owner + 0xdc);
        int32_t sumX = (int32_t)c->mem_r32(eA + 0x2c) + (int32_t)c->mem_r32(eB + 0x2c);
        c->mem_w16(obj + 0x2e, (uint16_t)(sumX / 2));
        int32_t sumY = (int32_t)c->mem_r32(eA + 0x30) + (int32_t)c->mem_r32(eB + 0x30);
        c->mem_w16(obj + 0x32, (uint16_t)((int32_t)(sumY / 2) + c->mem_r16s(obj + 0x60)));
        int32_t sumZ = (int32_t)c->mem_r32(eA + 0x34) + (int32_t)c->mem_r32(eB + 0x34);
        c->mem_w16(obj + 0x36, (uint16_t)(sumZ / 2));
        break;
      }
      case 3: {
        // Raw ground truth uses a ROUND-TOWARD-ZERO shift here (`(v - (v>>31)) >> 13`), unlike the
        // shared tail's plain arithmetic `>>12` -- preserved exactly, do not simplify to `>>13`.
        auto roundShiftR13 = [](int32_t v) -> int32_t { return (v - (v >> 31)) >> 13; };
        uint32_t e = c->mem_r32(owner + 0xc0);
        int32_t ownerAngle = c->mem_r16s(owner + 0x56);
        int32_t ownerRadius = c->mem_r16s(owner + 0x80);
        int32_t px = trigOf(c).rcos(ownerAngle) * ownerRadius;
        c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(e + 0x2c) - (int16_t)roundShiftR13(px)));
        c->mem_w16(obj + 0x32, c->mem_r16(e + 0x30));
        int32_t pz = trigOf(c).rsin(ownerAngle) * ownerRadius;
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(obj + 0x32) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, (uint16_t)(c->mem_r16s(e + 0x34) + (int16_t)roundShiftR13(pz)));
        break;
      }
      case 4: {
        uint32_t e = c->mem_r32(owner + 0xd0);
        c->mem_w16(obj + 0x2e, c->mem_r16(e + 0x2c));
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(e + 0x30) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, c->mem_r16(e + 0x34));
        break;
      }
      case 5: {
        uint32_t e = c->mem_r32(G_800E7F5C);  // pointer global, deref
        c->mem_w16(obj + 0x2e, c->mem_r16(e + 0x2c));
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(e + 0x30) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, c->mem_r16(e + 0x34));
        break;
      }
      case 6: {
        uint32_t e = c->mem_r32(G_800E7F50);  // pointer global, deref
        c->mem_w16(obj + 0x2e, c->mem_r16(e + 0x2c));
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(e + 0x30) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, c->mem_r16(e + 0x34));
        break;
      }
      case 7: {
        uint32_t e = c->mem_r32(owner + 0xe4);
        c->mem_w16(obj + 0x2e, c->mem_r16(e + 0x2c));
        c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(e + 0x30) + c->mem_r16s(obj + 0x60)));
        c->mem_w16(obj + 0x36, c->mem_r16(e + 0x34));
        break;
      }
    }
  }
  // Shared tail (every case, including sel>=8/default, falls here): radial offset by (angle,radius).
  int32_t xOff = trigOf(c).rcos(angle) * radius;
  c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(obj + 0x2e) + (int16_t)(xOff >> 12)));
  int32_t zOff = trigOf(c).rsin(angle) * radius;
  c->mem_w16(obj + 0x36, (uint16_t)(c->mem_r16s(obj + 0x36) + (int16_t)(-zOff >> 12)));

  resolvePositionEpilogue(c);
}

// ActorReward::approachTargetX(c) — FUN_80070650(obj a0). Trivial ease: if obj+0x60 (target-X
// delta scratch, reused for a different purpose than resolvePosition's Y-bias use of the same
// field -- this is the obj+5==1 sibling sub-mode) is nonzero, step obj+0x2e toward it by +8/frame,
// snapping to obj+0x60 and clearing it to 0 once obj+0x2e reaches or passes it. Leaf, no guest
// stack frame (matches gen_func_80070650 exactly -- no sp descent in the raw recompiled body).
// ----------------------------------------------------------------------------------------------
void ActorReward::approachTargetX(Core* c) {
  const uint32_t obj = c->r[R_A0];
  int32_t target = c->mem_r16s(obj + 0x60);
  if (target != 0) {
    int32_t x = c->mem_r16s(obj + 0x2e) + 8;
    c->mem_w16(obj + 0x2e, (uint16_t)x);
    if (target <= x) {
      c->mem_w16(obj + 0x2e, (uint16_t)target);
      c->mem_w16(obj + 0x60, 0);
    }
  }
}

// ActorReward::registerOverrides() — install all five guest addresses into the override registry
// (overrides::install), each with a shard_set_override setter (reaches the substrate's DIRECT
// func_<addr>(c) calls from FUN_8004AAC4) that shares the same entry any NATIVE caller reaches
// going through rec_dispatch, traced by the `dispatch` debug channel. See actor_sm_reward.h
// "WIRING" for why the setter is needed here.
// ----------------------------------------------------------------------------------------------

// --- WIDE-RE DRAFT wiring (2026-07-08 frontier pass) --- update/resolvePosition/approachTargetX.
// update (0x80070018) has NO direct same-shard caller (grepped generated/*.c) -- it's reached only
// via the DYNAMIC per-object dispatch in ObjectTable::dispatchFaithful (rec_dispatch(c, fn) with fn
// read from the object's own type table at runtime), so the rec_dispatch-side registry entry alone
// is the reach path (the shard_set_override setter below is installed anyway, defensively).
// resolvePosition (0x800702C0) / approachTargetX (0x80070650) DO have a direct same-shard caller
// (gen_func_80070018's own func_800702C0(c)/func_80070650(c) calls) -- but that caller is FULLY
// SUPERSEDED once update() is registered (ObjectTable::dispatchFaithful never falls through to
// gen_func_80070018 anymore), so those direct calls become dead in practice for core A. Dual-wired
// anyway for defensive completeness / consistency with the other 5 ActorReward leaves, and because
// core B (psx_fallback) must still be able to reach the real substrate body via g_override[] if
// anything ever calls func_800702C0/80070650 directly outside update()'s own reach.
extern void gen_func_80070018(Core*);
extern void gen_func_800702C0(Core*);
extern void gen_func_80070650(Core*);

void ActorReward::registerOverrides(Game* /*game*/) {
  using overrides::install;
  install(0x80049A60u, "ActorReward::smWindowScroll",  ActorReward::smWindowScroll,  gen_func_80049A60, shard_set_override);
  install(0x80049E54u, "ActorReward::smTallyTick",     ActorReward::smTallyTick,     gen_func_80049E54, shard_set_override);
  install(0x8004A3D4u, "ActorReward::smEventDispatch", ActorReward::smEventDispatch, gen_func_8004A3D4, shard_set_override);
  install(0x80070018u, "ActorReward::update",          ActorReward::update,          gen_func_80070018, shard_set_override);
  install(0x800702C0u, "ActorReward::resolvePosition", ActorReward::resolvePosition, gen_func_800702C0, shard_set_override);
  install(0x80070650u, "ActorReward::approachTargetX", ActorReward::approachTargetX, gen_func_80070650, shard_set_override);
  install(0x8004B150u, "ActorReward::smBlinkA",        ActorReward::smBlinkA,        gen_func_8004B150, shard_set_override);
  install(0x8004B208u, "ActorReward::smBlinkB",        ActorReward::smBlinkB,        gen_func_8004B208, shard_set_override);
}
