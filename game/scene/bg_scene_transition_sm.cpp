// engine/bg_scene_transition_sm.cpp — PC-native SOP intro BG scene-transition / screen-fade machine.
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
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);
#include "render/screen_fade/screen_fade.h"   // class ScreenFade — the single fade driver

namespace {

constexpr uint32_t SM_FN = 0x8002655Cu;
constexpr uint32_t P      = 0x80100400u;   // the scene-transition struct (fixed BSS arg a0)
constexpr uint32_t G_dir  = 0x800BF80Fu;   // DAT_800bf80c._3_1_ — direction/abort byte
constexpr uint32_t G_req  = 0x1F800236u;   // DAT_1f800236 — scene-transition request code (scratchpad)

// Screen fade — same shape as the guest's FUN_8007e9c8(color, P[3], 4) leaf. P[3]!=0 => additive/white,
// P[3]==0 => subtractive/black. Delivered via c->screenFade instead of a PSX OT rect.
static inline void fade_rect(Core* c, uint32_t color) {
  c->screenFade.applyLeafCall(color, c->mem_r8(P + 3));
}

void bg_scene_transition_sm(Core* c) {
  uint8_t st = c->mem_r8(P + 4);
  switch (st) {
    case 0: {
      c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
      c->r[4] = 0xFFFFFFFFu; rec_dispatch(c, 0x80075CECu);
      c->r[4] = 0x47FFu;     rec_dispatch(c, 0x80075CECu);
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
      fade_rect(c, 0xffffff);
      rec_dispatch(c, 0x80050970u);
      break;
    }
    case 1: {
      uint32_t u = (uint32_t)(((int)c->mem_r16s(P + 0xa) * (int)c->mem_r16s(P + 8)) & 0xff);
      fade_rect(c, (u << 16) | (u << 8) | u);
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
          rec_dispatch(c, 0x80026470u);
          c->mem_w16(P + 8, 0x1f);
          c->mem_w16(P + 0xa, 8);
          c->mem_w8(P + 3, dir == 2 ? 0 : 1);
          c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
        }
      }
      break;
    case 3: {
      uint32_t u = (uint32_t)(((int)c->mem_r16s(P + 8) * -8) & 0xff);
      fade_rect(c, (u << 16) | (u << 8) | u);
      if ((c->mem_r8(G_dir) & 0x80) == 0)
        c->mem_w16(P + 8, (uint16_t)(c->mem_r16s(P + 8) - 1));
      if (c->mem_r16s(P + 8) == 0) {
        c->mem_w8(P + 4, (uint8_t)(c->mem_r8(P + 4) + 1));
        c->mem_w8(G_dir, 0);
        rec_dispatch(c, 0x80026510u);
      }
      break;
    }
    case 4: {
      fade_rect(c, 0xffffff);
      uint8_t dir = c->mem_r8(G_dir);
      if (dir != 0) {
        if (dir == 1)      c->mem_w8(P + 3, 0);
        else if (dir == 3) c->mem_w8(P + 3, 1);
        rec_dispatch(c, 0x800264BCu);
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

static void bg_scene_transition_sm_verify(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("bgscenesmverify") ? 1 : 0;
  if (!s_v) { bg_scene_transition_sm(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  bg_scene_transition_sm(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = P;
  rec_super_call(c, SM_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[bgscenesmverify] MISMATCH st=%u ram@%x (nat=%02x ora=%02x) spad@%x\n",
                           c->mem_r8(P + 4), ro, ro >= 0 ? ramN[ro] : 0, ro >= 0 ? c->ram[ro] : 0, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[bgscenesmverify] %ld matches\n", ng);
}

}  // namespace

#include "bg_scene_transition_sm.h"
void BgSceneTransitionSm::step() { bg_scene_transition_sm_verify(core); }
bool BgSceneTransitionSm::readyForProgress() const {
  return core->mem_r8(0x800BF849u) == 0 && core->mem_r8(0x800ED06Du) == 0;
}
