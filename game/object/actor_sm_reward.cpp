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
#include "actor_sm_reward.h"
#include "engine_overrides.h"
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

// ActorReward::registerOverrides() — wire all five guest addresses into BOTH the recompiler's own
// call table (shard_set_override — reaches the substrate's DIRECT func_<addr>(c) calls from
// FUN_8004AAC4) and EngineOverrides (reaches any NATIVE caller going through rec_dispatch, and gets
// traced by the `dispatch` debug channel). See actor_sm_reward.h "WIRING" for why both are needed.
// ----------------------------------------------------------------------------------------------
namespace {
// psx_fallback-GATED trampolines for shard_set_override: g_override[] is a single table shared by
// EVERY Core (both SBS cores read the SAME array — unlike EngineOverrides, which is per-Game and
// whose rec_dispatch call site already skips psx_fallback cores). So the gate has to live IN the
// installed function itself: core B (the pure substrate reference) must keep running the exact
// recompiled body, or SBS would just be comparing our native port against itself (a fake 0-diff).
void ov_smWindowScroll(Core* c)  { if (c->game->psx_fallback) { gen_func_80049A60(c); return; } ActorReward::smWindowScroll(c); }
void ov_smTallyTick(Core* c)     { if (c->game->psx_fallback) { gen_func_80049E54(c); return; } ActorReward::smTallyTick(c); }
void ov_smEventDispatch(Core* c) { if (c->game->psx_fallback) { gen_func_8004A3D4(c); return; } ActorReward::smEventDispatch(c); }
void ov_smBlinkA(Core* c)        { if (c->game->psx_fallback) { gen_func_8004B150(c); return; } ActorReward::smBlinkA(c); }
void ov_smBlinkB(Core* c)        { if (c->game->psx_fallback) { gen_func_8004B208(c); return; } ActorReward::smBlinkB(c); }
}  // namespace

void ActorReward::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x80049A60u, "ActorReward::smWindowScroll",  ActorReward::smWindowScroll);
  ov.register_(0x80049E54u, "ActorReward::smTallyTick",     ActorReward::smTallyTick);
  ov.register_(0x8004A3D4u, "ActorReward::smEventDispatch", ActorReward::smEventDispatch);
  ov.register_(0x8004B150u, "ActorReward::smBlinkA",        ActorReward::smBlinkA);
  ov.register_(0x8004B208u, "ActorReward::smBlinkB",        ActorReward::smBlinkB);

  shard_set_override(0x80049A60u, ov_smWindowScroll);
  shard_set_override(0x80049E54u, ov_smTallyTick);
  shard_set_override(0x8004A3D4u, ov_smEventDispatch);
  shard_set_override(0x8004B150u, ov_smBlinkA);
  shard_set_override(0x8004B208u, ov_smBlinkB);
}
