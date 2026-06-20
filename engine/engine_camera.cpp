// PC-native engine CAMERA update (the per-frame FOLLOW that tracks the target object). Per the CLAUDE.md
// boundary the camera is ENGINE → reimplemented PC-native. The camera STATE it writes (pos/matrix at the
// scratchpad 0x1f8000d0 block) is a CONTENT/RENDER interface — the still-PSX cull FUN_8007712c and the
// native projection both read it — so these overrides must produce RAM-IDENTICAL camera fields vs the
// recomp reference (A/B gate: override-on vs override-off build, PSXPORT_RAMDUMP + cmp -l, on the free-roam
// MOTION scene PSXPORT_AUTO_SKIP=500 AUTO_WALK=r; the static idle field is A==B and can't exercise it).
// RE: docs/engine_re.md "Camera"; disasm tools/disas.py 0x8006d960 (function-scoped). later-173.
#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

// Shared rate-limiter FUN_8006ce74(delta, maxstep) = clamp(delta, -maxstep, +maxstep) on sign-extended s16.
static inline int32_t cam_clamp(int16_t delta, int16_t maxstep) {
  if (delta >= 0) return delta < maxstep ? delta : maxstep;   // (recomp: a1<a0 ? a1 : a0)
  return delta < (int16_t)(-maxstep) ? (int16_t)(-maxstep) : delta;
}

// One axis of FUN_8006d960: snap the camera accumulator to the target when within ±10 of its integer, else
// step it by clamp(delta,±6144)<<13. accum = 32-bit fixed @ (cam+accOff); its high 16 bits (cam+accOff+2 =
// the integer the cull reads) are what `cur` was read from. Returns 1 if it SNAPPED (settled), 0 if it moved.
//   tgt32 = target's full 32-bit fixed value;  tgtInt/curInt = the two u16 integers being compared.
static inline int cam_axis(Core* c, uint32_t accAddr, uint32_t tgt32Addr, uint16_t tgtInt, uint16_t curInt) {
  int16_t delta = (int16_t)(tgtInt - curInt);          // low-16 sign-extended (recomp: subu then sll/sra16)
  if ((uint16_t)(delta + 10) < 21) {                   // |delta| <= 10  -> snap
    c->mem_w32(accAddr, c->mem_r32(tgt32Addr));
    return 1;
  }
  int32_t step = cam_clamp(delta, 6144) << 13;         // (recomp: clampResult << 16 >>a 3 = << 13)
  c->mem_w32(accAddr, c->mem_r32(accAddr) + (uint32_t)step);
  return 0;
}

// FUN_8006d960 (0x8006d960) — track the camera X and Z toward the target object a1.
//   X: accum 0x1f8000dc (int 0x1f8000de) toward a1[+0]/a1[+2];  Z: accum 0x1f8000e4 (int 0x1f8000e6) toward
//   a1[+8]/a1[+a].  Returns v0 = 1 iff BOTH axes are settled (snapped) this frame, else 0.
static void ov_cam_track_xz(Core* c) {
  uint32_t a1 = c->r[5];
  if (cfg_dbg("cam")) { static int once = 0; if (!once) { once = 1;
    fprintf(stderr, "[cam] ov_cam_track_xz FIRED (a1=0x%08X)\n", a1); } }
  int snapX = cam_axis(c, 0x1f8000dcu, a1 + 0, c->mem_r16(a1 + 2),  c->mem_r16(0x1f8000deu));
  int snapZ = cam_axis(c, 0x1f8000e4u, a1 + 8, c->mem_r16(a1 + 10), c->mem_r16(0x1f8000e6u));
  c->r[2] = (snapX && snapZ) ? 1u : 0u;                // v0 = both-settled flag (recomp's a0 fold)
}

void rec_set_override(uint32_t addr, void (*fn)(Core*));

void engine_camera_register(void) {
  rec_set_override(0x8006d960u, ov_cam_track_xz);     // per-frame camera X/Z follow (engine_re "Camera")
}
