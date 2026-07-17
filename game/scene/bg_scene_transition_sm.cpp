// game/scene/bg_scene_transition_sm.cpp — PC-native SOP intro BG scene-transition / screen-fade machine.
//
// Owns the resident leaf FUN_8002655c (the next contiguous top-down frontier below the already-native SOP
// field update ov_sop_field_update / FUN_801092b4, sop.cpp). It is the intro-cutscene fade manager
// (white→normal fade-in, hold, fade-to-black on scene/area change), driven by the scene-transition request
// code DAT_1f800236 and the direction/abort byte 0x800bf80f (== DAT_800bf80c._3_1_). Fixed arg:
// a0 = 0x80100400 (the scene-transition struct in BSS).
//
//   state 0 (init/select): advance; FUN_80075cec(-1)+FUN_80075cec(0x47ff); pick fade params by the request
//     code DAT_1f800236 (1-4,9 / 5,6 / 7,8→0 / 0); set the fade color via FUN_8007e9c8; FUN_80050970.
//   state 1 (fade-in): u = ((s16)P[0xA] * (s16)P[8]) & 0xff -> grey fade rect; decrement P[8]; when it WAS 1
//     (reached 0) advance + clear 0x800bf80f.
//   state 2 (hold/wait): when 0x800bf849==0 && 0x800ed06d==0, on 0x800bf80f==2/==4: FUN_80026470 + re-arm
//     P[8]=0x1f/P[0xA]=8/P[3]=0or1 + advance.
//   state 3 (fade-out): u = ((s16)P[8] * -8) & 0xff -> grey rect; decrement P[8] unless (0x800bf80f&0x80);
//     when P[8]==0 advance + clear 0x800bf80f + FUN_80026510.
//   state 4 (commit/restart): FUN_8007e9c8 color; if 0x800bf80f!=0: pick P[3] (==1→0, ==3→1); FUN_800264bc;
//     P[4]=1 (absolute); re-arm P[8]=0x1f/P[0xA]=8.
//   default (P[4]>=5): return.
//
// CONTROL FLOW + every struct/global WRITE owned native byte-for-byte; every sub-call stays a pure-PSX leaf
// via rec_dispatch (incl. FUN_8007e9c8, the PSX fade-rect builder — kept PSX here; the PC-native fade is a
// separate frontier). RE'd 1:1 from disas 0x8002655c (Ghidra decomp scratch/decomp/field2/8002655c.c).
// GOTCHAs: P[8]/P[0xA] are signed `lh` (compares) / `sh` (stores); P[3]/P[4] + the globals are bytes;
// 0x800bf80f = DAT_800bf80c._3_1_; the fade-in/out "reached zero" tests use value-BEFORE-decrement==1 (st1)
// and value-AFTER-decrement==0 (st3) — transcribe the read/write/compare order exactly; P[4] is += in
// states 0-3 but = 1 (absolute) in state 4. Byte-exact A/B gate (full RAM+scratchpad vs rec_super_call).

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);
#include "render/screen_fade.h"   // class ScreenFade — the single fade driver
#include "bg_scene_transition_sm.h"
#include "core/engine.h"
#include "override_registry.h"    // overrides::install — the one native-override registry

// Ground-truth substrate bodies (the ORACLE gen leg runs these) + the two still-substrate sub-leaves
// opSceneEventArmWait calls (kept pure-PSX via their func_ thunks, byte-faithful to the gen body).
extern void gen_func_80042758(Core*);
extern void gen_func_80042884(Core*);
extern void shard_set_override(uint32_t, void (*)(Core*));
void func_80040B48(Core*);   // SceneEvents::arm (scene-event ARM primitive)
void func_80042728(Core*);   // BgSceneTransitionSm::readyForProgress predicate thunk

namespace {

constexpr uint32_t SM_FN = 0x8002655Cu;
constexpr uint32_t P      = 0x80100400u;   // the scene-transition struct (fixed BSS arg a0)
constexpr uint32_t G_dir  = 0x800BF80Fu;   // DAT_800bf80c._3_1_ — direction/abort byte
constexpr uint32_t G_req  = 0x1F800236u;   // DAT_1f800236 — scene-transition request code (scratchpad)
constexpr uint32_t G_flag80a = 0x800BF80Au; // scene sub-state flag byte (in the 0x800BF808 control block)

// script-node field offsets used by the opcode leaves below
constexpr uint32_t NODE_PARAM0   = 114;    // node+0x72 — (s16) scene-event id / first param
constexpr uint32_t NODE_PARAM1   = 116;    // node+0x74 — (s16) second param / settle latch
constexpr uint32_t NODE_SUBSTATE = 120;    // node+0x78 — u8 opcode sub-state

}  // namespace

// Screen fade — same shape as the guest's FUN_8007e9c8(color, P[3], 4) leaf. P[3]!=0 => additive/white,
// P[3]==0 => subtractive/black. Delivered via fade(c) instead of a PSX OT rect.
void BgSceneTransitionSm::fadeRect(Core* c, uint32_t color) {
  cfg_logf("fadesites", "[fadesite] BgSm-state%u color=%06x dir=%u",
           c->mem_r8(P + 4), color, c->mem_r8(P + 3));
  fade(c).applyLeafCall(color, c->mem_r8(P + 3));
}

// -- Native ports of the tiny sub-leaves this SM calls -------------------------------------------
//
// FUN_80075CEC — AUDIO FADE-TARGET setter. Writes 0x800BE222 (fade target) and, when its arg is
// negative, mirrors the same value into 0x800BE224 (a "matched" pair used by the mixer to hard-
// jump both endpoints at once). Positive values are clamped to 0x7FFF. Ghidra decomp
// scratch/decomp/bg_scene_subleaves.c. Same primitive that music_coord.cpp inlines directly
// (see the comment "FUN_80075CEC(0x47FF): fade target"). Will hoist to
// a proper audio class the first time a THIRD caller shows up.
void BgSceneTransitionSm::audioFadeTarget(Core* c, int32_t v) {
  if (v < 0) {
    c->mem_w16(0x800BE222u, (uint16_t)(int16_t)(-v));
    c->mem_w16(0x800BE224u, (uint16_t)(int16_t)(-v));
    return;
  }
  if (v > 0x7FFF) v = 0x7FFF;
  c->mem_w16(0x800BE222u, (uint16_t)(int16_t)v);
}

// Common guard shared by FUN_80026470/80026510/800264BC — three inline audio-fade-target callers,
// each with a different arg pattern; all three fire only when 800BF80D > 1 OR 800BF839 != 0
// (the same "we're actually mid-transition" gate the SM's state 2/3/4 use elsewhere).
bool BgSceneTransitionSm::midTransitionGate(Core* c) {
  return c->mem_r8(0x800BF80Du) > 1 || c->mem_r8(0x800BF839u) != 0;
}
void BgSceneTransitionSm::audioStub26470(Core* c) { if (midTransitionGate(c)) audioFadeTarget(c, 0); }
void BgSceneTransitionSm::audioStub26510(Core* c) { if (midTransitionGate(c)) audioFadeTarget(c, -1); }
void BgSceneTransitionSm::audioStub264BC(Core* c) {
  if (midTransitionGate(c)) { audioFadeTarget(c, -1); audioFadeTarget(c, 0x47FF); }
}

// FUN_80050970 — tiny dispatcher on the 800BF816 mode byte: 0 = ModeStateArm::armFromAreaTable ; else =
// ModeStateArm::arm(0,0,0). Both native now.
void BgSceneTransitionSm::bf816Dispatch(Core* c) {
  if (c->mem_r8(0x800BF816u) == 0) eng(c).modeStateArm.armFromAreaTable();
  else                             eng(c).modeStateArm.arm();
}

void BgSceneTransitionSm::body(Core* c) {
  uint8_t st = c->mem_r8(P + 4);
  switch (st) {
    case 0: {
      c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
      audioFadeTarget(c, -1);            // native — was rec_dispatch(0x80075CEC, -1)
      audioFadeTarget(c, 0x47FF);      // native — was rec_dispatch(0x80075CEC, 0x47FF)
      uint8_t req = c->mem_r8(G_req);
      switch (req) {
        case 1: case 2: case 3: case 4: case 9:
          c->mem_w8(G_req, 0);
          c->mem_w16(P + 8, 0x1f);
          c->mem_w16(P + 0xa, 8);
          c->mem_w8(P + 3, 1);
          break;
        case 5: case 6:
          c->mem_w16(P + 8, 0x7f);
          c->mem_w16(P + 0xa, 2);
          c->mem_w8(P + 3, 1);
          break;
        case 7: case 8:
          c->mem_w8(G_req, 0);
          // fallthrough to case 0
        case 0:
          c->mem_w16(P + 8, 0x1f);
          c->mem_w16(P + 0xa, 8);
          c->mem_w8(P + 3, 0);
          break;
        default: break;
      }
      fadeRect(c, 0xffffff);
      bf816Dispatch(c);                 // native — was rec_dispatch(0x80050970)
      break;
    }
    case 1: {
      uint32_t u = (uint32_t)(((int)c->mem_r16s(P + 0xa) * (int)c->mem_r16s(P + 8)) & 0xff);
      fadeRect(c, (u << 16) | (u << 8) | u);
      int16_t before = c->mem_r16s(P + 8);
      c->mem_w16(P + 8, (uint16_t)(before - 1));
      if (before == 1) {
        c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
        c->mem_w8(G_dir, 0);
      }
      break;
    }
    case 2:
      if (c->mem_r8(0x800BF849u) == 0 && c->mem_r8(0x800ED06Du) == 0) {
        uint8_t dir = c->mem_r8(G_dir);
        if (dir == 2 || dir == 4) {
          audioStub26470(c);           // native — was rec_dispatch(0x80026470)
          c->mem_w16(P + 8, 0x1f);
          c->mem_w16(P + 0xa, 8);
          c->mem_w8(P + 3, dir == 2 ? 0 : 1);
          c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
        }
      }
      break;
    case 3: {
      uint32_t u = (uint32_t)(((int)c->mem_r16s(P + 8) * -8) & 0xff);
      fadeRect(c, (u << 16) | (u << 8) | u);
      if ((c->mem_r8(G_dir) & 0x80) == 0)
        c->mem_w16(P + 8, (uint16_t)(c->mem_r16s(P + 8) - 1));
      if (c->mem_r16s(P + 8) == 0) {
        c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
        c->mem_w8(G_dir, 0);
        audioStub26510(c);             // native — was rec_dispatch(0x80026510)
      }
      break;
    }
    case 4: {
      fadeRect(c, 0xffffff);
      uint8_t dir = c->mem_r8(G_dir);
      if (dir != 0) {
        if (dir == 1)      c->mem_w8(P + 3, 0);
        else if (dir == 3) c->mem_w8(P + 3, 1);
        audioStub264BC(c);             // native — was rec_dispatch(0x800264BC)
        c->mem_w8(P + 4, 1);                                  // absolute, not +=
        c->mem_w16(P + 8, 0x1f);
        c->mem_w16(P + 0xa, 8);
      }
      break;
    }
    default:
      return;
  }
}

void BgSceneTransitionSm::verifyBody(Core* c) {
  int s_v = c->game->verify.on("bgscenesmverify");
  if (!s_v) { body(c); return; }
  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  body(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = P;
  rec_super_call(c, SM_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("bgscenesmverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[bgscenesmverify] MISMATCH st=%u ram@%x (nat=%02x ora=%02x) spad@%x\n",
                           c->mem_r8(P + 4), ro, ro >= 0 ? ramN[ro] : 0, ro >= 0 ? c->ram[ro] : 0, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[bgscenesmverify] %ld matches\n", ng);
}

void BgSceneTransitionSm::step() { verifyBody(core); }
bool BgSceneTransitionSm::readyForProgress() const {
  return core->mem_r8(0x800BF849u) == 0 && core->mem_r8(0x800ED06Du) == 0;
}

// -- Cutscene-script opcode leaves (adjacent to readyForProgress in the guest binary) ------------
//
// opSceneEventArmWait (FUN_80042758) — READY-FRAME leaf. The gen body descends sp by 24 and spills
// the callee-saved regs ra/s0 (r31/r16) at sp+20/sp+16 with their LIVE incoming values, restoring
// them before every return; the native port mirrors that guest stack frame exactly (see
// docs/faithful-execution.md, game/player/collision.cpp::flatNormal). node comes in a0 (= s0 after
// the prologue). Both sub-calls (SceneEvents::arm, readyForProgress) stay pure-PSX via their func_
// thunks with the RE'd jal-site ra constant, byte-faithful to gen.
// ORACLE: gen_func_80042758
void BgSceneTransitionSm::opSceneEventArmWait(Core* c) {
  c->r[29] = c->r[29] + (uint32_t)-24;               // addiu sp,-0x18 — descend the guest frame
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);   // sw s0,0x10(sp) — LIVE incoming s0
  c->r[16] = c->r[4] + c->r[0];                       // s0 = node (a0)
  c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);   // sw ra,0x14(sp)
  c->r[3] = (uint32_t)c->mem_r8((c->r[16] + NODE_SUBSTATE));
  { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_state0; }
  { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_state1; }
   goto L_ret;
L_state0:;                                            // state 0 — arm the scene event, latch completion
  c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + NODE_PARAM0));
  { int _t = ((int32_t)c->r[4] < 0);  if (_t) goto L_advance; }
  c->r[31] = 0x800427A0u;
   func_80040B48(c);                                  // SceneEvents::arm(param0)
  c->r[3] = c->r[0] + (uint32_t)1;
  { int _t = (c->r[2] != c->r[3]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_ret; }
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + NODE_PARAM1));
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_ret; }
L_advance:;
  c->r[2] = (uint32_t)c->mem_r8((c->r[16] + NODE_SUBSTATE));
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((c->r[16] + NODE_SUBSTATE), (uint8_t)c->r[2]); goto L_ret0;
L_state1:;                                            // state 1 — poll the scene-progress-ready predicate
  c->r[31] = 0x800427D8u;
   func_80042728(c);                                  // readyForProgress
   goto L_ret;
L_ret0:;
  c->r[2] = c->r[0] + c->r[0];
L_ret:;
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));   // lw ra,0x14(sp)
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));   // lw s0,0x10(sp)
  c->r[29] = c->r[29] + (uint32_t)24;                 // addiu sp,0x18 — ascend the guest frame
}

// opClearSceneFlag80a (FUN_80042884) — one-shot opcode leaf: clear the scene sub-state flag byte
// and return 1. No frame.
// ORACLE: gen_func_80042884
void BgSceneTransitionSm::opClearSceneFlag80a(Core* c) {
  c->mem_w8(G_flag80a, (uint8_t)c->r[0]);   // *0x800BF80A = 0
  c->r[2] = c->r[0] + (uint32_t)1;          // v0 = 1
}

// eov_* wrappers — guest-ABI adapters (node in c->r[4], return in c->r[2]).
static void eov_opSceneEventArmWait(Core* c) { BgSceneTransitionSm::opSceneEventArmWait(c); }
static void eov_opClearSceneFlag80a(Core* c) { BgSceneTransitionSm::opClearSceneFlag80a(c); }

void BgSceneTransitionSm::registerOverrides() {
  using overrides::install;
  install(0x80042758u, "BgSceneTransitionSm::opSceneEventArmWait", eov_opSceneEventArmWait, gen_func_80042758, shard_set_override);
  install(0x80042884u, "BgSceneTransitionSm::opClearSceneFlag80a", eov_opClearSceneFlag80a, gen_func_80042884, shard_set_override);
}
