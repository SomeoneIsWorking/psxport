// game/object/actor_sm_24448.cpp — Tomba!2 per-object actor STATE-MACHINE handler FUN_80024448, PC-native.
//
// SCOPE (per CLAUDE.md "FULL OWNERSHIP" + the move-and-collide RE in docs/engine_re.md): this owns ONE
// concrete actor "move-and-collide" SM step end-to-end — its CONTROL FLOW + branch decisions + the guest
// field writes — PC-native. The shared fixed-point LEAVES it calls (the grid move-collide probe
// FUN_80046A44, the slide-finalize FUN_80048654, and the per-result sub-step FUN_80024AF0 which itself
// only calls the engine rsin/rcos LUTs) stay dispatched: they compute exact >>12 fixed-point results that
// the still-recomp AI/render read back, and transcribing their table math would be PSX-simulation, which
// the methodology rejects. We own the SM around them.
//
// ---------------------------------------------------------------------------------------------------
// RE — FUN_80024448(obj a0)  (disas.py over scratch/bin/tomba2/MAIN.EXE)
// ---------------------------------------------------------------------------------------------------
//   80024448 prologue (frame 0x20; saves s0=obj, s1, ra)
//   8002445c lh   v0, +0x17E(obj)                 ; dir/mode selector (signed)
//   80024464 bltz v0 -> a3 = 0x25 (37)            ; maxiter for the probe
//   8002446c          else a3 = 0x4A (74)
//   80024470 lhu  a2, +0x68(obj)                  ; Y-velocity (u16)
//   80024474 lh   a1, +0x66(obj)                  ; X-velocity (s16) -> "speed" arg
//   8002447c sb   zero, +0x17D(obj)               ; clear floor-type-out byte
//   80024480 a2 = sign_ext16( -a2 )               ; negate Y-vel (ystep arg)
//   80024478 a0 = obj
//   80024488 jal  FUN_80046A44(obj, X-vel, -Y-vel, maxiter)   ; the grid move-collide PROBE  [DISPATCH]
//   80024490 s1 = v0                              ; probe result tag  ((tag>>9)&3)
//   80024494 if (s1 == 0) -> 0x80024530 : return 0
//
//   --- probe HIT (s1 != 0) ---
//   8002449c lhu  v0, 0x1F8001A6 ; v0 = (v0>>11)&3 ; sb v0, +0x17D(obj)   ; floor-type from result tag
//   800244ac jal  FUN_80048654(obj)                ; slide-finalize: writes obj +0x48/4A/4C, scratch
//                                                   ; 0x1A0/0x1A2, via atan2/sqrt  [DISPATCH]
//   800244b8 v1 = lhu 0x1F8001A0                    ; resolved heading (scratch, written by 0x80048654)
//   800244bc v0 = lbu +0x147(obj)                   ; "flip heading" flag
//   800244c8 sh   v1, +0x140(obj)                   ; ALWAYS store raw resolved heading to +0x140
//   800244c4 if (v0 != 0) { sh ((v1-0x800)&0xFFF), +0x56(obj) }   ; angle = heading +/- 0x800 (flip)
//   800244dc else          { sh  v1,               +0x56(obj) }   ; angle = heading
//
//   --- result-tag tail (s1 in {1,2,...}) ---
//   800244e0 if (s1 != 2) -> 0x80024518 : { sb 4, +0x164(obj); v0ret = 1 }     ; default state-out = 4
//   800244ec else (s1 == 2):
//   800244ec   t = lbu +0x17D(obj) ; if (t & 1) {                              ; floor-type bit0 set?
//   80024500     sb 7, +0x164(obj)                                            ;   state-out = 7
//   80024504     jal FUN_80024AF0(obj)                                        ;   sub-step  [DISPATCH]
//   80024510     v0ret = 1
//                } else { sb 4, +0x164(obj) ; v0ret = 1 }                      ;   else state-out = 4
//   80024524 sb   s1, +0x15C(obj)                   ; store the probe result tag
//   8002452c sw   zero, 0x1F800084                  ; clear scratch word 0x84
//   80024534 return v0ret (== 1 on the HIT path)
//
//   80024530 (miss): return 0.
//
// FIELD WRITES owned native (guest addresses, all relative to the object node `obj`):
//   +0x17D (sb)  floor-type out byte: cleared to 0 at entry; on HIT set to (scratch 0x1F8001A6 >> 11) & 3.
//   +0x140 (sh)  resolved heading (raw scratch 0x1F8001A0), on HIT.
//   +0x56  (sh)  object angle: heading, or (heading-0x800)&0xFFF when the +0x147 flip flag is set.
//   +0x164 (sb)  state-out byte: 4 (default), or 7 when s1==2 and floor-type bit0 is set.
//   +0x15C (sb)  probe result tag s1, on HIT.
//   scratch 0x1F800084 (sw) cleared to 0 on HIT.
// (FUN_80048654 additionally writes obj +0x48/0x4A/0x4C and scratch 0x1A0/0x1A2; those are produced by the
//  DISPATCHED leaf, not by this body — we let the leaf run and only consume its 0x1A0 output afterward.)
//
// LEAVES DISPATCHED (exact fixed-point results consumed by still-recomp content; NOT transcribed):
//   FUN_80046A44  grid move-collide probe (rsin/rcos*speed>>12 slide, atan2 slide-pick, writes +0x2E/32/36)
//   FUN_80048654  slide-finalize (atan2 + sqrt; writes obj +0x48/4A/4C + scratch 0x1A0/0x1A2)
//   FUN_80024AF0  s1==2 floor sub-step (reads +0x147/+0x17E/+0x66/+0x68; rsin/rcos 0x80083E80/0x80083F50)
//
// VERIFY: the `sm24448verify` REPL channel (`debug sm24448verify`) runs the native body, snapshots+rolls
// back full main-RAM + scratchpad + regs, runs the recomp body via rec_super_call, and diffs both. Same
// family rationale as the entity SM gates (child40410/disp26c88/sm40558): the dispatched leaves run in BOTH
// passes and leave transient residue in their own stack frames below entry sp, and this fn's own 0x20 frame
// is dead below sp on return — so the gate excludes the [sp-0x800, sp) stack window (far above all game
// data). A 0-diff over many frames is the content-interface gate. See docs/port-progress.md.

#include "core.h"
#include "actor.h"
#include "cfg.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (super-call / A/B reference)
void rec_dispatch(Core*, uint32_t);     // hybrid call: recomp body if emitted, else interpret (honors overrides)

namespace {

constexpr uint32_t SM_FN        = 0x80024448u;  // FUN_80024448 — the actor move-and-collide SM step
constexpr uint32_t FN_PROBE     = 0x80046A44u;  // grid move-collide probe                         [leaf]
constexpr uint32_t FN_SLIDEFIN  = 0x80048654u;  // slide-finalize (atan2/sqrt, writes 0x1A0/0x1A2) [leaf]
constexpr uint32_t FN_SUBSTEP   = 0x80024AF0u;  // s1==2 floor sub-step (rsin/rcos)                [leaf]

constexpr uint32_t SC_TAG_A6 = 0x1F8001A6u;     // probe result tag (floor-type bits at >>11)
constexpr uint32_t SC_HEAD_A0= 0x1F8001A0u;     // resolved heading (written by FN_SLIDEFIN)
constexpr uint32_t SC_W84    = 0x1F800084u;     // scratch word cleared on the HIT path

enum { R_A0 = 4, R_A1 = 5, R_A2 = 6, R_A3 = 7, R_V0 = 2 };

}  // namespace

// ------------------------------------------------------------------------------------------------
// Actor::sm24448(c) — the PC-native body. a0 = obj. Returns its result tag in v0 (0 on miss, 1 on
// hit), exactly as the PSX body's epilogue does.
// ------------------------------------------------------------------------------------------------
void Actor::sm24448(Core* c) {
  const uint32_t obj = c->r[R_A0];

  // --- entry: pick maxiter from +0x17E, read velocity, clear floor-type-out ---
  int16_t  mode  = c->mem_r16s(obj + 0x17E);          // lh +0x17E
  uint32_t maxit = (mode < 0) ? 0x25u : 0x4Au;                // bltz -> 37 else 74
  uint16_t yvel  = c->mem_r16(obj + 0x68);                    // lhu +0x68 (Y-vel)
  int16_t  xvel  = c->mem_r16s(obj + 0x66);           // lh  +0x66 (X-vel = speed)
  c->mem_w8(obj + 0x17D, 0);                                  // sb zero, +0x17D
  int32_t  ystep = (int32_t)(int16_t)(uint16_t)(-(int32_t)yvel); // negate, sign-extend 16

  // --- the grid move-collide PROBE (leaf, dispatched) ---
  c->r[R_A0] = obj;
  c->r[R_A1] = (uint32_t)(int32_t)xvel;                       // a1 = X-vel (sign-extended)
  c->r[R_A2] = (uint32_t)ystep;                               // a2 = -Y-vel (sign-extended)
  c->r[R_A3] = maxit;                                         // a3 = maxiter
  rec_dispatch(c, FN_PROBE);
  uint32_t s1 = c->r[R_V0];                                   // probe result tag

  if (s1 == 0) { c->r[R_V0] = 0; return; }                   // 0x80024530: miss -> return 0

  // --- HIT: floor-type byte from the result tag, then slide-finalize ---
  uint32_t a6 = c->mem_r16(SC_TAG_A6);
  c->mem_w8(obj + 0x17D, (uint8_t)((a6 >> 11) & 3));          // sb (tag>>11)&3, +0x17D (delay-slot store)
  c->r[R_A0] = obj;
  rec_dispatch(c, FN_SLIDEFIN);                              // writes scratch 0x1A0/0x1A2 + obj 0x48/4A/4C

  // --- apply the resolved heading -> angle ---
  uint16_t head = c->mem_r16(SC_HEAD_A0);                     // lhu 0x1F8001A0
  uint8_t  flip = c->mem_r8(obj + 0x147);                     // lbu +0x147
  c->mem_w16(obj + 0x140, head);                              // sh head, +0x140 (always)
  if (flip != 0)
    c->mem_w16(obj + 0x56, (uint16_t)((head - 0x800u) & 0xFFFu));  // flipped angle
  else
    c->mem_w16(obj + 0x56, head);                                  // raw heading

  // --- result-tag tail: state-out byte (+0x164) + the s1==2 floor sub-step ---
  if (s1 == 2) {
    uint8_t ft = c->mem_r8(obj + 0x17D);                     // lbu +0x17D (floor-type)
    if (ft & 1) {
      c->mem_w8(obj + 0x164, 7);                             // sb 7, +0x164
      c->r[R_A0] = obj;
      rec_dispatch(c, FN_SUBSTEP);                           // FUN_80024AF0(obj) [leaf]
    } else {
      c->mem_w8(obj + 0x164, 4);                             // sb 4, +0x164
    }
  } else {
    c->mem_w8(obj + 0x164, 4);                               // sb 4, +0x164 (default)
  }

  c->mem_w8(obj + 0x15C, (uint8_t)s1);                        // sb s1, +0x15C
  c->mem_w32(SC_W84, 0);                                     // sw zero, 0x1F800084
  c->r[R_V0] = 1;                                            // return 1 on the HIT path
}

