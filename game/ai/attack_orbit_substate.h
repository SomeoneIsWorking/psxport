// game/ai/attack_orbit_substate.h — class AttackOrbitSubstate: the two OVERLAY (A00) sub-behavior
// leaves reached from beh_id_compare_motion_dispatch (FUN_80145230, game/object/behavior_dispatch.cpp
// entry "id_compare_motion_dispatch") when the node's motion-substate byte node[3] is 0x80 / 0x81.
//
// RE'd via Ghidra headless (A00 overlay project, scratch/ghidra/A00, functions FUN_80145AF0 /
// FUN_801458E0) and cross-checked instruction-for-instruction against the recompiled substrate
// (generated/ov_a00_shard_0.c: ov_a00_gen_80145AF0; generated/ov_a00_shard_1.c: ov_a00_gen_801458E0).
// Same taxi convention as the rest of this overlay's owned leaves (Cull::cullWrapperFlag2 etc): the
// object is c->r[4], no separate parameter; all field access goes through c->mem_r*/mem_w*.
//
// Object field layout used by BOTH methods (guest offsets from `node`, the a0 taxi arg — this is a
// generic per-object record, not specific to this behavior; only the fields these two leaves touch
// are named here):
//   +0x00 kind byte (set to 7 on the one-shot hit in aimAtTargetAnchor)
//   +0x04 outer per-object state (owned by beh_id_compare_motion_dispatch; aimAtTargetAnchor can
//         clobber this to 2 via a 4-byte write that ALSO resets +0x07, matching the recomp's own
//         `*(u32*)(node+4) = 2` idiom already used elsewhere in beh_id_compare_motion_dispatch.cpp)
//   +0x07 this behavior's own sub-phase byte (0..5 in orbitTargetMotion; 0..2(+) in aimAtTargetAnchor)
//   +0x10 captured target pointer (u32) — written by orbitTargetMotion phase 0 (FUN_8011740C result);
//         read by aimAtTargetAnchor for the attack-window state check (+0x7A/+0x6C/+0x5E on it)
//   +0x14 aim-point position-source pointer (u32) — read by aimAtTargetAnchor for the +0x2C/0x30/0x34
//         position triple it copies into +0x2E/0x32/0x36. orbitTargetMotion zeroes it at phase 0; some
//         other still-substrate init must set it to something live before node[3]==0x80 runs (not
//         covered by either of these two leaves — ported byte-for-byte regardless of who owns that).
//   +0x1B hit-guard flag byte (bit 0x40 = one-shot latch on the attack-effect call)
//   +0x2B this-behavior sub-flag byte (aimAtTargetAnchor only)
//   +0x2E/+0x32/+0x36 self aim-point triple (written by aimAtTargetAnchor)
//   +0x2C/+0x34 self position triple X/Z (accumulated by orbitTargetMotion phase 2)
//   +0x40 phase timer (u16, reused across phases 2/4/5 with different reload constants)
//   +0x48/+0x4C orbit speed X/Z (s16, read-only here — set elsewhere)
//   +0x4E orbit rate (s16; set to the constant 0x800 in phase 1, multiplied in phase 2)
//   +0x56 orbit angle (s16, swept +0x80/frame in phase 5)
//   +0x58 cleared by aimAtTargetAnchor, otherwise unused here
//   +0x62 flag word (bit0 = orbit accumulate direction / aim one-shot latch, bit2 = "second lap")
//   +0xC4 attack-effect id/arg passed through to func_0x800331D8 (still substrate)
#pragma once
#include <cstdint>
class Core;

class AttackOrbitSubstate {
public:
  Core* core = nullptr;

  // orbitTargetMotion (FUN_801458E0): node[3]==0x81 handler. A 6-phase machine on node[7]:
  //   0: acquire target (FUN_8011740C) into node[0x10]/node[0x14], seed direction flag + angle=0,
  //      falls straight into phase 1 the same call (recomp fallthrough, not a "next frame" edge).
  //   1: re-init (FUN_801402B8), orbit-rate const = 0x800, timer = 20, falls into phase 2.
  //   2: accumulate self position (+0x2C/+0x34) by (speed * rate) rotated by the direction bit;
  //      count the timer down; while >0 this phase repeats every frame (no fallthrough); on
  //      expiry advance to phase 3 — either way the switch ends here this frame.
  //   3: re-init (FUN_801402B8 with different args), timer = 16, falls into phase 4.
  //   4: count 16 down; on expiry reset timer=16, advance to phase 5, set flag bit 4.
  //   5: sweep angle by +0x80/frame using the PRE-decrement timer value for the expiry test (a
  //      recomp quirk preserved faithfully — see the `told` local); on expiry loop back to phase 1,
  //      flip the direction bit, clear angle + the "second lap" flag bit.
  //   default (phase>=6): no-op.
  // Common tail (every path, including the phase>=6 no-op): FUN_801406E4(node) + node[0x32] += 20.
  void orbitTargetMotion();

  // aimAtTargetAnchor (FUN_80145AF0): node[3]==0x80 handler.
  //   phase 0: FUN_801406E4(node) re-arm, then falls into the phase-1 init block.
  //   phase 1: FUN_801402B8 init, phase=2, flag bit0 set, angle=0x800, falls into the aim recompute.
  //   phase 2: (direct entry) aim recompute only.
  //   phase>2: plain return — NO common tail here (unlike orbitTargetMotion; matches recomp exactly).
  //   Aim recompute: self aim-point (+0x2E/0x32/0x36) := position-source's position (+0x2C/0x30/0x34)
  //   offset by a fixed (+0x28,-0xCD,+0) anchor delta; +0x58 cleared.
  //   Attack-window check: if the captured target's state (+0x7A) is 2 or 6, OR its (+0x6C==2 AND
  //   +0x5E in {2,3}) — else plain return, no attack — this is an attack window: set node[0x2B]
  //   (0x80 if the +0x5E branch matched 2, else 0), then clobber node[4..7] (u32) to 2. Finally,
  //   guarded by the one-shot latch node[0x1B]&0x40, fire the attack/effect leaf
  //   func_0x800331D8(node, node[0xC4], -100, 0), latch node[0x1B] bit 0x40, set node[0]=7.
  void aimAtTargetAnchor();
};
