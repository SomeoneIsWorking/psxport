// game/ai/beh_a06_script_fades.cpp — PC-native ports of the SCRIPT-DRIVEN cutscene fade fns.
//
// These are the fn pointers embedded as data words in the A06 cutscene scripts (at 0x80149A20 /
// 0x80149A9C / 0x80149CDC), dispatched by opcode 0x03E via `lw v0, 0x74(a0); jalr v0` inside
// FUN_800412CC. With the caller chain (beh_a06_scripted_actor → variant4Sm → variant4Phase3 →
// ScriptInterp::step) now native (commit a70092a), op 0x03E's native path routes the fnptr
// through `BehaviorDispatch::dispatchObj(obj, fnptr)` and any fn registered as a native `beh_*`
// runs native transparently — unregistered addresses fall through to the substrate leaf.
//
// RET-CODE CONVENTION: op 0x03E's guest handler leaves v0 = (fnptr's return). ScriptInterp::step
// reads c->r[2] after dispatchObj. Since native `beh_*` handlers are `void`-returning, each
// native fade fn must set `c->r[2] = ret` before returning — that's the substrate-compatible
// contract. The value is interpreted by the ret-code switch in ScriptInterp::step (VERBATIM
// disas): 0 = pause loop, 1 = FA0(obj,0), 2 = FA0(obj,1), 3 = FA0(obj,0), >=4 = exit loop.
//
// RE'd from Ghidra decomp (scratch/decomp/a06_fade_fns.c + hand-disas 0x8013B29C) + spot-check of
// raw MIPS. Substrate leaves kept reachable via rec_dispatch (each a small standalone leaf):
//   0x8007E9C8  = ScreenFade set — native as `fade(c).applyLeafCall(color, mode)`
//   0x8006CBD0  = geomblk / cmd-list attach (used by FUN_80139728 state 5)
//   0x8006CBA8  = same family (used by FUN_8013B074)
//   0x8003116C  = obj spawner (used by FUN_8013B074)
//   0x80051B04  = music/SFX cue (used by FUN_8013B274)
//   0x80072DDC  = obj allocator (used by FUN_8013AEF0)
//   0x8004D604  = flag/hook set (used by FUN_8013AEF0)
//   0x80070F00  = sound-command play (used by FUN_8013AFD8)
//   0x800708B4  = sound-command mode (used by FUN_8013AFD8)

#include "core.h"
#include "game_ctx.h"
#include "core/engine.h"
#include "render/screen_fade.h"
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {

// External guest globals these fns touch.
constexpr uint32_t G_BFA22   = 0x800BFA22u;   // set to 0x2B in FUN_80139728 state 4
constexpr uint32_t G_BE0E4   = 0x800BE0E4u;   // gate byte (bit 7) in FUN_8013B29C
constexpr uint32_t G_BF8D5   = 0x800BF8D5u;   // gate byte for FUN_8013AEF0
constexpr uint32_t G_E806C   = 0x800E806Cu;   // set to 7 in state 5, to 2 in FUN_8013B074
constexpr uint32_t G_E8076   = 0x800E8076u;   // zeroed in FUN_8013B074
constexpr uint32_t G_E8078   = 0x800E8078u;   // set to 0x800 in FUN_8013B074
constexpr uint32_t G_E8074   = 0x800E8074u;   // computed in FUN_8013B074
constexpr uint32_t G_1003F8  = 0x801003F8u;   // read as u32 in FUN_8013B074 (multiplied by 7)
constexpr uint32_t DAT_8014994C = 0x8014994Cu; // arg to FUN_8006cbd0 (geomblk / cmd list)
constexpr uint32_t OBJ_A_800E8008 = 0x800E8008u;   // fixed obj arg to FUN_8006cbd0
constexpr uint32_t SPAD_1F800214  = 0x1F800214u;   // scratchpad ptr slot read by FUN_8013AFD8

// Object field offsets used by these fns.
constexpr uint32_t O_LEVEL_40  = 0x40u;    // fade level (u16)
constexpr uint32_t O_TIMER_42  = 0x42u;    // countdown timer (u16)
constexpr uint32_t O_STATE_78  = 0x78u;    // sub-state (u8) — the state-machine variable
constexpr uint32_t O_D0        = 0xD0u;    // parent-obj ptr (FUN_8013B274 SFX cue)

inline uint32_t grayRGB(uint32_t u) { return (u << 16) | (u << 8) | u; }

// Set the ret code v0 the way the guest fn's `jr ra` would leave it — read back by callFnptr.
inline void setV0(Core* c, uint32_t v) { c->r[2] = v; }

// Small leaf-call helpers (substrate leaves kept reachable).
inline void callObj2(Core* c, uint32_t a, uint32_t b, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; rec_dispatch(c, addr);
}
inline void callObj3(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; rec_dispatch(c, addr);
}
inline void callObj4(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; c->r[7] = e; rec_dispatch(c, addr);
}
inline uint32_t callObj4Ret(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t addr) {
  callObj4(c, a, b, d, e, addr); return c->r[2];
}

}  // namespace

// ── FUN_80139728 — 8-state additive-gray fade with music/spawn trigger ─────────────────────────
// obj[+0x78] = state; obj[+0x40] = fade level (u16); obj[+0x42] = state-6 countdown timer.
// Every non-terminal state ends by writing the current gray on ScreenFade (`gray << 16 | << 8 | u`)
// and returns 0 (pause loop). State 7 returns 1 (advance) when the ramp reaches 0.
void beh_a06_fade_flash_ramp_80139728(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t state = c->mem_r8(obj + O_STATE_78);
  bool done = false;   // set true → jump to LAB_80139880 (advance state, then draw+return 0)

  switch (state) {
    case 0:
      c->mem_w16(obj + O_LEVEL_40, 0u);
      c->mem_w8(obj + O_STATE_78, (uint8_t)(state + 1u));
      [[fallthrough]];
    case 1:
    case 3: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_LEVEL_40) + 0x40);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)v);
      if (v >= 0x80) done = true;
      break;
    }
    case 2: {
      const int16_t v = (int16_t)c->mem_r16(obj + O_LEVEL_40);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)(v - 0x40));
      if (v == 0x40) done = true;
      break;
    }
    case 4: {
      const int16_t v = (int16_t)c->mem_r16(obj + O_LEVEL_40);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)(v - 0x40));
      if (v == 0x40) {
        c->mem_w8(obj + O_STATE_78, (uint8_t)(state + 1u));
        c->mem_w8(G_BFA22, 0x2Bu);
      }
      break;
    }
    case 5: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_LEVEL_40) + 0x10);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)v);
      if (v > 0xFF) {
        c->mem_w16(obj + O_LEVEL_40, 0xFFu);
        c->mem_w16(obj + O_TIMER_42, 0x1Eu);
        c->mem_w8(obj + O_STATE_78, (uint8_t)(state + 1u));
        c->mem_w8(G_E806C, 7u);
        // FUN_8006CBD0(0x800E8008, &DAT_8014994C) — geomblk / cmd-list attach; keep as substrate.
        callObj2(c, OBJ_A_800E8008, DAT_8014994C, 0x8006CBD0u);
      }
      break;
    }
    case 6: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_TIMER_42) - 1);
      c->mem_w16(obj + O_TIMER_42, (uint16_t)v);
      if (v == -1) done = true;
      break;
    }
    case 7: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_LEVEL_40) - 0x10);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)v);
      // recomp:  (int)((uint)uVar2 << 0x10) < 1 == v <= 0 as int32 — TERMINATE the SM.
      if (v <= 0) { setV0(c, 1u); return; }
      break;
    }
    default:
      break;
  }
  if (done) c->mem_w8(obj + O_STATE_78, (uint8_t)(state + 1u));
  // Tail: apply the current gray level as an ADDITIVE fade rect + return 0 (pause the step loop
  // so the next tick re-enters).
  const uint32_t u = c->mem_r8(obj + O_LEVEL_40);
  fade(eng(c).core).applyLeafCall(grayRGB(u), /*ADDITIVE*/ 1u);
  setV0(c, 0u);
}

// ── FUN_8013AEF0 — spawn a follow-obj and hook it ───────────────────────────────────────────────
// Allocates via FUN_80072DDC(param, 3, 3, 0x3F); if allocation succeeded, sets the new obj's
// handler (obj+0x1C = &FUN_8012E194), obj[+0x03]=2, obj[+0x28] |= 0x80. If the global gate G_BF8D5
// says "not yet" (!= -1), also calls FUN_8004D604(0x31, 1) — a follow-up hook.
// GUEST FRAME (gen ov_a06_gen_8013AEF0, mirror-verify 0x80108B0C finding): sp-24, ra@+16 — pushed
// BEFORE either rec_dispatch call and popped at the tail. Both callees (0x80072DDC, then
// transitively 0x8007A980; 0x8004D604) are real guest leaves that push their OWN frames relative
// to c->r[29] and save the incoming r31, so this fn's frame + the jal-site RA constants must be
// live or their frame-saves land at the wrong (unpushed) address / wrong RA value — exactly the
// 0x801FE8xx byte mismatches the strict mirror gate caught.
void beh_a06_spawn_follow_obj_8013AEF0(Core* c) {
  const uint32_t param1 = c->r[4];
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);
  c->r[31] = 0x8013AF08u;
  callObj4(c, param1, 3u, 3u, 0x3Fu, 0x80072DDCu);
  const uint32_t newObj = c->r[2];
  if (newObj == 0) {
    c->r[31] = c->mem_r32(sp + 16); c->r[29] += 24;
    setV0(c, 0u); return;
  }
  c->mem_w32(newObj + 0x1Cu, 0x8012E194u);
  c->mem_w8 (newObj + 0x03u, 2u);
  c->mem_w8 (newObj + 0x28u, (uint8_t)(c->mem_r8(newObj + 0x28u) | 0x80u));
  if ((int8_t)c->mem_r8(G_BF8D5) != -1) {
    c->r[31] = 0x8013AF4Cu;
    callObj2(c, 0x31u, 1u, 0x8004D604u);
  }
  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
  setV0(c, 1u);
}

// ── FUN_8013AFD8 — kick a sound-command sequence and wait for scratchpad completion ─────────────
// Guest packs a 3-halfword struct at sp+0x18 { 0x34E8, 0xE1BA, 30000 } then calls FUN_80070F00
// (queue play) + FUN_800708B4(3) (mode); on state 1 waits for (*(u8)(*(u32)0x1F800214 + 3) != 0).
void beh_a06_sound_cmd_wait_8013AFD8(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t st = c->mem_r8(obj + O_STATE_78);
  if (st == 0) {
    // Recomp uses a local 6-byte stack struct { u16 0x34E8, u16 0xE1BA, u16 30000 } at sp+0x18. We
    // reserve a small guest-stack slot for it (the substrate leaf reads by (u16*)ptr).
    const uint32_t sp_save = c->r[29];
    c->r[29] = sp_save - 24u;
    const uint32_t scratchArg = c->r[29] + 6u;   // matches recomp's auStack_18 offset
    c->mem_w16(scratchArg + 0u, 0x34E8u);
    c->mem_w16(scratchArg + 4u, 0xE1BAu);
    c->mem_w16(scratchArg + 8u, 30000u);
    callObj3(c, obj, 1u, scratchArg, 0x80070F00u);
    c->r[4] = 3u;
    rec_dispatch(c, 0x800708B4u);
    c->r[29] = sp_save;
    c->mem_w8(obj + O_STATE_78, (uint8_t)(st + 1u));
    setV0(c, 0u);
    return;
  }
  if (st != 1) { setV0(c, 0u); return; }
  const uint32_t ptr = c->mem_r32(SPAD_1F800214);
  if (c->mem_r8(ptr + 3u) == 0) { setV0(c, 1u); return; }
  setV0(c, 0u);
}

// ── FUN_8013B074 — spawn a subobj + set field/anim params ───────────────────────────────────────
// Recomp: builds { u16 0x3340, u16 0xDFEA, u16 0x6F86 } on stack; calls FUN_8003116C(0x718, ptr,
// -0x32); sets obj[+0x28] |= 0x80 on the returned obj; then updates a bank of guest globals
// (0x800E806C=2, 0x800E8076=0, 0x800E8078=0x800, 0x800E8074 = 7 * *(u32)0x801003F8) and calls
// FUN_8006CBA8(ptr).
void beh_a06_spawn_subobj_8013B074(Core* c) {
  const uint32_t sp_save = c->r[29];
  c->r[29] = sp_save - 24u;
  const uint32_t scratchArg = c->r[29] + 6u;
  c->mem_w16(scratchArg + 0u, 0x3340u);
  c->mem_w16(scratchArg + 4u, 0xDFEAu);
  c->mem_w16(scratchArg + 8u, 0x6F86u);
  callObj3(c, 0x718u, scratchArg, (uint32_t)(int32_t)-0x32, 0x8003116Cu);
  const uint32_t newObj = c->r[2];
  if (newObj) {
    c->mem_w8(newObj + 0x28u, (uint8_t)(c->mem_r8(newObj + 0x28u) | 0x80u));
  }
  c->mem_w32(G_E806C, 2u);
  c->mem_w32(G_E8076, 0u);
  c->mem_w32(G_E8078, 0x800u);
  c->mem_w32(G_E8074, c->mem_r32(G_1003F8) * 7u);
  c->r[4] = scratchArg;
  rec_dispatch(c, 0x8006CBA8u);
  c->r[29] = sp_save;
  setV0(c, 1u);
}

// ── FUN_8013B178 — 3-state simple additive fade (0x20 ramp) ─────────────────────────────────────
// obj[+0x78] = state, obj[+0x40] = level.
//   state 0: level = 0; advance to 1
//   state 1: level += 0x20; if signed < 0x80: draw+pause; else advance to 2
//   state 2: level -= 0x20; if was 0x20 → return 1 (done); else draw+pause
void beh_a06_fade_ramp_8013B178(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t st = c->mem_r8(obj + O_STATE_78);
  int8_t goAdvance = 0;   // 1 → advance state before drawing

  if (st == 1) {
    const int16_t v = (int16_t)(c->mem_r16(obj + O_LEVEL_40) + 0x20);
    c->mem_w16(obj + O_LEVEL_40, (uint16_t)v);
    if (v >= 0x80) goAdvance = 1;
  } else if (st > 1) {
    if (st == 2) {
      const int16_t v = (int16_t)c->mem_r16(obj + O_LEVEL_40);
      c->mem_w16(obj + O_LEVEL_40, (uint16_t)(v - 0x20));
      if (v == 0x20) { setV0(c, 1u); return; }
    }
    // else fall to draw
  } else if (st == 0) {
    c->mem_w16(obj + O_LEVEL_40, 0u);
    goAdvance = 1;
  }
  if (goAdvance) c->mem_w8(obj + O_STATE_78, (uint8_t)(st + 1u));
  const uint32_t u = c->mem_r8(obj + O_LEVEL_40);
  fade(eng(c).core).applyLeafCall(grayRGB(u), /*ADDITIVE*/ 1u);
  setV0(c, 0u);
}

// ── FUN_8013B274 — one-shot music/SFX cue then advance ──────────────────────────────────────────
void beh_a06_music_cue_8013B274(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t parent = c->mem_r32(obj + O_D0);
  callObj3(c, parent, 0xFu, 4u, 0x80051B04u);
  setV0(c, 1u);
}

// ── FUN_8013B29C — 2-state (init + counted gate) primitive ──────────────────────────────────────
// state 0: obj[+0x40]=60, advance to 1 (fall through)
// state 1: obj[+0x40]--; if new == -1: return 1 (advance script). Else if (mem[0x800BE0E4]&0x80):
//          return 0 (gate blocks — pause). Else: return 1 (normal tick advance).
void beh_a06_timer_gate_8013B29C(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t st = c->mem_r8(obj + O_STATE_78);
  if (st == 0) {
    c->mem_w8(obj + O_STATE_78, 1u);
    c->mem_w16(obj + O_LEVEL_40, 60u);
    st = 1;
    // fall to state 1
  } else if (st != 1) {
    setV0(c, 0u);   // unknown state — pause
    return;
  }
  // state 1:
  const int16_t v = (int16_t)(c->mem_r16(obj + O_LEVEL_40) - 1);
  c->mem_w16(obj + O_LEVEL_40, (uint16_t)v);
  if (v == -1) { setV0(c, 1u); return; }
  if ((c->mem_r8(G_BE0E4) & 0x80u) != 0) { setV0(c, 0u); return; }
  setV0(c, 1u);
}
