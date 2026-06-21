// engine/animation.cpp — PC-native per-object ANIMATION-VM subsystem.
// The per-object animation-sequence VM stepper (FUN_80076D68): walks an 8-byte-stride keyframe stream,
// counting down the per-frame duration and following tag-keyed jumps. Control flow + memory ops owned
// native; the 3 frame sub-fns (applier / loader / executor) stay reachable by address via rec_dispatch.
// NO GTE. Extracted verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for
// PC-game code structure. The `animvm` diagnostic A/B gate (full RAM+scratchpad vs rec_super_call) is a
// REPL channel, unchanged.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "animation.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80076D68 — per-object ANIMATION-SEQUENCE VM stepper (resident, no GTE; ~3.6% of field interp
// time, a frequency leader on motion scenes). a0 (s0) = anim object; field s0+0x0E (h) is the frame
// COUNTDOWN/duration; field s0+0x38 (cur) is the cursor (word ptr) into an 8-byte-stride keyframe
// stream; each keyframe holds a tag/payload halfword at +6 and a jump pointer (word) at +8. The
// control word s0+0x0E is read once at entry as `ctrl`; its bit 0x1000 is the "do NOT advance the
// cursor this step" flag (sequence freeze/loop).
//
// Logic (transcribed 1:1 from the disasm — control flow + memory ops owned native, the 3 sub-fns
// (0x80075f0c per-frame applier, 0x80076904 frame loader, 0x80075ff8 frame executor) stay PSX,
// dispatched via rec_dispatch so each honors any later override identically in the super-call path):
//  - low12(ctrl) > 1  -> DELAY: still counting down this frame. The cursor's SIGN BIT is a flag:
//    cur<0 (bit31 set) = freeze in place -> sh[s0+14] = (low12-1) | (ctrl & 0x1000), return 0; else
//    apply the current frame (0x80075f0c, a1 = low12-1 — it sets the cursor's KSEG0 bit when a1==1)
//    and sh[s0+14] = (low12-1) | (post-call s0[14] & 0x1000), return 2.
//  - low12(ctrl) == 1 -> STEP: read the keyframe tag at [cur+6]&0xc000 and dispatch (freeze =
//    ctrl & 0x1000; when frozen the cursor is NOT advanced in any block):
//      tag 0x0000: advance cur += 8 (unless frozen), load the new frame's duration into s0+14
//                  (low12 of [cur+6]) via 0x80076904, then if the LOADED frame has the 0x2000
//                  "execute" flag, run its executor (0x80075ff8) keyed by the new tag; return 0.
//      tag 0x4000: FOLLOW the jump pointer cur = [cur+8] (unless frozen), reload duration, same exec;
//                  return 0. (The transient cur+8 store the asm makes is dead — overwritten by [cur+8].)
//      tag 0x8000: terminal/hold — sh[s0+14] = [cur+6]&0xfff (no advance), return 1.
//      tag 0xc000: FOLLOW cur = [cur+8] (unless frozen), reload duration, same exec; return 1 (and on
//                  the no-exec-flag path stores the loaded [cur+6]&0xfff like a hold).
//  The executor calls (L_f40/L_f28/L_ff0/L_7000) differ only in whether the executor's a1 is the
//  cursor's jump target ([cur+8]) or the cursor itself +8, and whether v0 returns 0 or 1.
// `animvm` gate = full RAM+scratchpad A/B vs rec_super_call (each path runs once from one checkpoint;
//  the native run is rolled back; the fn's own 40-byte stack frame [sp-40,sp) is excluded — the gen
//  prologue saves regs there, the native body never touches the guest stack).
static void anim_vm_76d68(Core* c) {
  const uint32_t s0   = c->r[4];
  const uint32_t ctrl = c->mem_r16(s0 + 14);             // a0reg (read once)
  const uint32_t low12 = ctrl & 0x0fffu;
  const uint32_t cnt   = (low12 - 1) & 0xffffffffu;      // s1 = low12 - 1
  const uint32_t freeze = ctrl & 0x1000u;                // bit set => do not advance the cursor

  // ---- DELAY branch: low12 != 1 (counter still running) ----
  if (cnt != 0) {
    int32_t cur = (int32_t)c->mem_r32(s0 + 56);
    if (cur < 0) {                                       // cursor sign bit set -> freeze in place
      // cur<0 path (0x80076dd0): h = (low12-1) + (ctrl & 0x1000); return 0. The +0x1000 term comes from
      // the bltz delay slot `andi v0,a0,0x1000` (a0 = the entry ctrl word) — NOT a literal +2.
      c->mem_w16(s0 + 14, (uint16_t)(cnt + freeze));
      c->r[2] = 0; return;
    }
    // 0x80075f0c takes a1 = cnt (the counter, s1 at the jal) — it uses (int16_t)a1==1 to decide whether
    // to set the KSEG0 bit on the cursor (s0+56). The entry register a1 was `addu a1,s1,zero` (=cnt).
    c->r[4] = s0; c->r[5] = cnt; rec_dispatch(c, 0x80075f0cu);   // apply current frame
    // s0+14 is RE-READ here (the applier 0x80075f0c may have modified it); only bit 0x1000 is kept.
    uint32_t fz = c->mem_r16(s0 + 14) & 0x1000u;
    c->mem_w16(s0 + 14, (uint16_t)(cnt + fz));           // (low12-1) | (post-call s0[14] & 0x1000)
    c->r[2] = 2; return;
  }

  // ---- STEP branch: low12 == 1 (frame elapsed) ----
  uint32_t cur = c->mem_r32(s0 + 56);
  uint32_t op  = c->mem_r16(cur + 6);                    // lhu; tag bits + payload
  uint32_t opu = op;                                     // lhu copy (a1)
  uint32_t tag = op & 0xc000u;                           // s1

  // Run the executor tail and set v0 (mirrors L_f40/L_f28/L_ff0/L_7000):
  //   jump_target: true -> executor a1 = [cur+8]; false -> a1 = cur+8
  //   retval: function return value
  auto exec_tail = [&](uint32_t cur_in, bool jump_target, uint32_t retval) {
    uint32_t a1 = jump_target ? c->mem_r32(cur_in + 8) : (cur_in + 8);
    c->r[4] = s0; c->r[5] = a1; c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s0 + 14);
    rec_dispatch(c, 0x80075ff8u);
    c->r[2] = retval;
  };

  if (tag == 0x4000u) {                                  // block T4000 (0x80076e9c)
    // NOT frozen -> follow the jump pointer cur=[cur+8] (the cur+8 sw is dead, overwritten); frozen ->
    // leave the cursor unchanged (the bne skips the whole follow).
    if (!freeze) { cur = c->mem_r32(cur + 8); c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;        // duration low12
    c->mem_w16(s0 + 14, (uint16_t)dur);                  // sh BEFORE the call (delay slot)
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);          // load frame
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) { c->r[2] = 0; return; }    // no exec flag
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 0); return; }  // -> L_f28
      c->r[2] = 0; return;
    }
    if (t == 0x8000u)      { c->r[2] = 0; return; }               // -> L_701c
    if (t == 0xc000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    c->r[2] = 0; return;
  }

  if (tag < 0x4001u) {                                   // tag == 0 -> block T0 (0x80076e2c)
    if (tag != 0) { c->r[2] = 0; return; }               // (unreachable for &0xc000, kept faithful)
    if (!freeze) { cur = cur + 8; c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;
    c->mem_w16(s0 + 14, (uint16_t)dur);                  // sh in jal delay slot
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);          // load frame
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) { c->r[2] = 0; return; }
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 0); return; }   // == s2 -> L_f40
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 0); return; }  // -> L_f28
      c->r[2] = 0; return;
    }
    if (t == 0x8000u)      { c->r[2] = 0; return; }               // -> L_701c
    if (t == 0xc000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    c->r[2] = 0; return;
  }

  // tag is 0x8000 or 0xc000
  if (tag == 0x8000u) {                                  // block T8000 (0x80076f58)
    c->mem_w16(s0 + 14, (uint16_t)(opu & 0x0fffu));
    c->r[2] = 1; return;
  }
  // tag == 0xc000 -> block TC000 (0x80076f64)
  {
    // Same follow-jump structure as T4000: NOT frozen -> cur=[cur+8]; frozen -> unchanged.
    if (!freeze) { cur = c->mem_r32(cur + 8); c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;
    c->mem_w16(s0 + 14, (uint16_t)dur);
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) {                                    // beq -> 0x80076f5c (hold-store + return 1)
      c->mem_w16(s0 + 14, (uint16_t)(opu & 0x0fffu)); c->r[2] = 1; return;
    }
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 1); return; }   // == s2 -> L_7000
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 1); return; }  // -> L_ff0
      c->r[2] = 1; return;                                        // (delay v0=1) -> 0x80077020
    }
    if (t == 0x8000u)      { c->r[2] = 1; return; }               // == s3 -> 0x80077020 (v0=1)
    if (t == 0xc000u)      { exec_tail(a1c, true, 1); return; }   // == s1 -> L_7000
    c->r[2] = 1; return;
  }
}

void ov_anim_vm_76d68(Core* c) {
  // Lazy gate (re-check each call): this fn can run before the REPL `debug animvm` is processed.
  if (!cfg_dbg("animvm")) { anim_vm_76d68(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t s0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  anim_vm_76d68(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80076D68u);
  uint32_t v0_o = c->r[2];
  // Exclude FUN_80076D68's OWN 40-byte stack frame [sp-40, sp) — gen saves regs there, native never
  // touches the guest stack. Sub-call frames are identical (both interpret the same jals).
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 40) ? sp - 40 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[animvm] MISMATCH s0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           s0, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[animvm] %ld matches\n", ng);
}
