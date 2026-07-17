// game/object/animation.cpp — PC-native per-object ANIMATION-VM subsystem.
// The per-object animation-sequence VM stepper (FUN_80076D68): walks an 8-byte-stride keyframe stream,
// counting down the per-frame duration and following tag-keyed jumps. Control flow + memory ops owned
// native; the 3 frame sub-fns (applier / loader / executor) stay reachable by address via rec_dispatch.
// NO GTE. Extracted verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for
// PC-game code structure. The `animvm` diagnostic A/B gate (full RAM+scratchpad vs rec_super_call) is a
// REPL channel, unchanged.
#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "animation.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
#include "override_registry.h"   // overrides::install — the one native-override registry
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
    eng(c).animation.applyFrame(s0, (int32_t)(int16_t)cnt);   // apply current frame (native FUN_80075F0C)
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
    c->r[4] = s0; c->r[5] = a1; c->r[6] = (uint32_t)c->mem_r16s(s0 + 14);
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
    eng(c).animation.loadFrame(s0);                   // load frame (native FUN_80076904)
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
    eng(c).animation.loadFrame(s0);                   // load frame (native FUN_80076904)
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
    eng(c).animation.loadFrame(s0);                   // load frame (native FUN_80076904)
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

// Animation::stepFramed — GUEST-ABI ENTRY ONLY for FUN_80076D68 (RE: generated/shard_7.c
// gen_func_80076D68): `addiu sp,-40; sw s0,16(sp) [BEFORE s0<-a0]; sw ra,32(sp); sw s3,28(sp);
// sw s2,24(sp); sw s1,20(sp)`. Each spill writes the CALLER's LIVE incoming r16/r17/r18/r19/ra
// BEFORE the body repurposes r16 as its own `node` local, mirroring the real frame descent/spill/
// ascent so a caller that reached this function through the GUEST CALL GRAPH (c->r[29] is a real
// guest sp at a real guest call site — the shard_set_override/override-registry trampolines below)
// gets byte-exact guest-stack bytes.
//
// NOT used by step() below: step() is also called directly by NATIVE C++ beh_ handlers
// (beh_actor_move_sm.cpp etc — see step()'s own header comment) that never cross the guest ABI —
// at those call sites c->r[29] is WHATEVER the last unrelated guest call left it at, not a frame
// belonging to this call. Framing there would push/pop against an essentially RANDOM stack
// address, corrupting an unrelated live guest-stack region for the duration of the call (bug
// found + fixed 2026-07-08: wiring this into step() itself produced an SBS packet_pool/scratch
// divergence at f3773 in a completely unrelated subsystem, because the temporary frame collided
// with real, concurrently-live guest stack data at whatever sp happened to be current for a
// native caller). Guest-ABI reachers use THIS method directly instead of step().
void Animation::stepFramed(uint32_t node) {
  Core* c = this->core;
  uint32_t save16 = c->r[16], save17 = c->r[17], save18 = c->r[18], save19 = c->r[19], saveRa = c->r[31];
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 16, save16);          // sw s0,16(sp) — LIVE incoming r16 (before s0<-a0)
  c->r[16] = node;                            // s0 = a0 (node) — the body's own local
  c->mem_w32(c->r[29] + 32, saveRa);          // sw ra,32(sp)
  c->mem_w32(c->r[29] + 28, save19);          // sw s3,28(sp)
  c->mem_w32(c->r[29] + 24, save18);          // sw s2,24(sp)
  c->mem_w32(c->r[29] + 20, save17);          // sw s1,20(sp)

  c->r[4] = node;                             // taxi-in for the still-taxi internal impl
  anim_vm_76d68(c);

  c->r[31] = c->mem_r32(c->r[29] + 32);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 40;
}

void Animation::step(uint32_t node) {
  Core* c = this->core;
  c->r[4] = node;                            // taxi-in for the still-taxi internal impl
  // Lazy gate (re-check each call): this fn can run before the REPL `debug animvm` is processed.
  if (!cfg_dbg("animvm")) { anim_vm_76d68(c); return; }
  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
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
  // Exclude FUN_80076D68's OWN 40-byte stack frame [sp-40, sp) — this NATIVE (unframed) run never
  // touches the guest stack (only stepFramed(), the guest-ABI entry, does); the SUBSTRATE run via
  // rec_super_call pushes/pops its OWN real frame at whatever sp happens to be current for this
  // native-triggered A/B check (not a real guest call site), so that transient frame is expected to
  // differ from "untouched" and is excluded here, same as before this fix.
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 40) ? sp - 40 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("animvm");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[animvm] MISMATCH s0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           s0, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[animvm] %ld matches\n", ng);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80076904 — Animation::loadFrame: POSE-TABLE FRAME LOADER. RE'd via tools/disas.py (Ghidra
// headless timed out on the full-binary import in this session; every field/branch below was
// confirmed by hand-walking the MIPS with delay-slot semantics, including the two "dead" `j`
// targets the compiler emits after an already-exhaustive if/else chain).
//
// obj = node. cur = obj+0x38 (the SAME cursor field the anim-VM/step() and attach() use). The
// CURRENT keyframe-table entry is `rec = *(uint32_t*)(tableBase + idx*4)`, where idx = cur[0]
// (u16) and tableBase = obj+0x3C. `rec`'s top byte is a FLAGS byte (obj+8 always gets this byte,
// unconditionally); its low 24 bits are a byte offset added to tableBase to give the packed
// PAYLOAD STREAM `stream`.
//
//   flags & 0x40: this entry carries per-limb pose data (else the function only stamps obj+8 and
//     returns via the loop's zero-count guard).
//     flags & 0x80 (checked in EITHER the 0x40-set or 0x40-clear arm, each with its own identical
//       byte-code — RE'd once, shared as unpack12x3 below): unpack a signed 12-bit XYZ triple from
//       5 packed bytes into obj+0x88/0x8a/0x8c (a base pose/offset triple).
//     The "phase" seed for the per-limb parity loop (Loop2) differs by arm: flags&0x40 SET keeps
//     `rec` itself (the raw 32-bit table entry) as the phase seed; flags&0x40 CLEAR uses phase=1 if
//     flags&0x80 else phase=0.
//   Then (both arms funnel here): boundA = obj[8]&0x3f becomes the loop's iteration count (obj[8] is
//     OVERWRITTEN with this masked value — the flags byte doesn't survive past this point); if
//     obj[9] (boundB, a distinct count) == 0, return immediately (no per-limb data at all).
//     - flags&0x40 SET  -> Loop1 (unconditional, 6 fields/9 bytes per limb: fields 8/10/12 as plain
//       12-bit values, fields 0x38/0x3a/0x3c as 12-bit values then <<3).
//     - flags&0x40 CLEAR -> Loop2 (parity-gated on (i+phase)&1, 3 fields/4.5 bytes per limb, fields
//       8/10/12 only — nibble-sharing alternates which half of a straddling byte an EVEN vs ODD
//       struct index consumes).
//   Both loops walk `a2` over an array of struct* at (obj+192 + i*4) [i.e. *(uint32_t*)(obj+192+4*i)
//   is the destination limb struct for iteration i], stopping when i>=boundA (checked before the
//   body) OR (after incrementing i) i>=boundB (checked after the body) — whichever hits first.
static inline uint32_t rd_u32(Core* c, uint32_t a) { return c->mem_r32(a); }

// unpack12x3 — shared 5-byte-stream -> 3x signed-12-bit unpack (dest = obj+0x88/0x8a/0x8c). Both
// flags&0x40 arms run this IDENTICAL byte sequence when flags&0x80 is set.
static void anim_unpack_pose_triple(Core* c, uint32_t obj, uint32_t& stream) {
  uint32_t s = stream;
  uint32_t e0 = c->mem_r8(s), e1 = c->mem_r8(s + 1), e2 = c->mem_r8(s + 2);
  uint32_t e3 = c->mem_r8(s + 3), e4 = c->mem_r8(s + 4);
  // Each shift pair mirrors the MIPS `sll N; sra M` idiom exactly (arithmetic right shift on a
  // signed 32-bit value — sign-extends from whatever bit lands at bit31 after the sll):
  //   v88 = sign_extend8(e0)<<4 | (e1>>4)              (sll 24; sra 20 on e0, then OR e1's hi nibble)
  //   v8a = sign_extend4(e1&0xf)<<8 | e2                (sll 28; sra 20 on e1, then OR e2 full byte)
  //   v8c = sign_extend8(e3)<<4 | (e4>>4)               (sll 24; sra 20 on e3, then OR e4's hi nibble)
  int32_t v88 = ((int32_t)(e0 << 24) >> 20) | (int32_t)(e1 >> 4);
  int32_t v8a = ((int32_t)(e1 << 28) >> 20) | (int32_t)e2;
  int32_t v8c = ((int32_t)(e3 << 24) >> 20) | (int32_t)(e4 >> 4);
  c->mem_w16(obj + 0x88, (uint16_t)v88);
  c->mem_w16(obj + 0x8a, (uint16_t)v8a);
  c->mem_w16(obj + 0x8c, (uint16_t)v8c);
  // BUG FIX (2026-07-08, SBS-full f61 divergence 0x800F2Bxx region): this 5-byte window packs
  // THREE 12-bit fields (36 bits) into 40 bits (5 bytes) — v8c only consumes e3 (8 bits) + e4's
  // HIGH nibble (4 bits) = 36 bits total. e4's LOW nibble is never consumed here; it's a SHARED/
  // STRADDLING nibble that belongs to the NEXT 12-bit-field reader (Loop2's ODD-phase first field,
  // which is exactly why `phase` seeds to 1 when this unpack ran — "there's a pending nibble").
  // The advance must leave `stream` POINTING AT e4 (byte offset 4), not past it (offset 5), so the
  // next reader re-reads that byte and masks off the low nibble. Hand-verified byte-for-byte
  // against the recomp oracle (docs/findings/animation.md): node=0x800FB960, entryPtr chain ->
  // stream bytes `.. 00 0F E1 04 A0 9A FF`; with stream left at the 0x0F byte, Loop2's ODD branch
  // (c0=0x0F,c1=0xE1,c2=0x04,c3=0xA0,c4=0x9A) reproduces the substrate's exact output f8=0x0FE1,
  // f10=0x004A, f12=0x009A — byte-identical to recomp_path. The old `s + 5` silently ate the
  // shared nibble, corrupting every subsequent per-limb field for any node whose pose-triple
  // (flags&0x80) unpack precedes a Loop1/Loop2 per-limb walk.
  stream = s + 4;
}

void Animation::loadFrame(uint32_t node) {   // FUN_80076904
  Core* c = this->core;
  const uint32_t obj  = node;
  const uint32_t cur  = c->mem_r32(obj + 0x38);
  const uint32_t idx  = c->mem_r16(cur + 0);
  const uint32_t tableBase = c->mem_r32(obj + 0x3c);
  const uint32_t entryPtr  = tableBase + idx * 4;
  const uint32_t rec  = rd_u32(c, entryPtr);                 // packed table entry (NOT a pointer)
  const int8_t   flagsByte = (int8_t)(rec >> 24);
  const uint32_t off24 = rec & 0x00FFFFFFu;
  uint32_t stream = tableBase + off24;
  c->mem_w8(obj + 8, (uint8_t)flagsByte);                    // always stamped (delay-slot write)

  uint32_t phase;
  if (flagsByte & 0x40) {
    // Gen's 0x40-set arm has its OWN inline pose-triple unpack whose final read advances `r5 += 5`
    // (gen_func_80076904, the block before L_800769C4): the header is PADDED to 9 bytes so Loop1's
    // 9-byte-per-limb stream stays aligned — no shared straddling nibble in this variant. The
    // shared helper leaves stream at s+4 (the Loop2 shared-nibble contract), so skip the 5 pad
    // bytes here. Without this, Loop1 decoded every limb 5 bytes early (watch-cut f289: Charles'
    // one-frame pose transient — native wrote f8=0x0020.. constants where gen wrote 0x0FF5/0x0222).
    if (flagsByte & 0x80) { anim_unpack_pose_triple(c, obj, stream); stream += 5; }
    phase = rec;                                             // raw table entry seeds the parity
  } else {
    if (flagsByte & 0x80) { phase = 1; anim_unpack_pose_triple(c, obj, stream); }
    else                  { phase = 0; }
  }

  uint32_t boundA = c->mem_r8(obj + 8) & 0x3fu;
  uint32_t boundB0 = c->mem_r8(obj + 9);
  c->mem_w8(obj + 8, (uint8_t)boundA);
  if (boundB0 == 0) return;

  uint32_t a2 = obj;
  if (flagsByte & 0x40) {
    // ---- Loop1: unconditional, 6 fields (8/10/12 plain 12-bit; 0x38/0x3a/0x3c 12-bit then <<3) ----
    for (uint32_t i = 0; ; ) {
      if (!(i < boundA)) return;
      uint32_t s = rd_u32(c, a2 + 192);
      uint8_t b0 = c->mem_r8(stream), b1 = c->mem_r8(stream + 1), b2 = c->mem_r8(stream + 2);
      uint8_t b3 = c->mem_r8(stream + 3), b4 = c->mem_r8(stream + 4), b5 = c->mem_r8(stream + 5);
      uint8_t b6 = c->mem_r8(stream + 6), b7 = c->mem_r8(stream + 7), b8 = c->mem_r8(stream + 8);
      stream += 9;
      uint16_t f8  = (uint16_t)((b0 << 4) | (b1 >> 4));
      uint16_t f10 = (uint16_t)(((b1 & 0xf) << 8) | b2);
      uint16_t f12 = (uint16_t)((b3 << 4) | (b4 >> 4));
      uint16_t f56 = (uint16_t)((((b4 & 0xf) << 8) | b5) << 3);
      uint16_t f58 = (uint16_t)(((b6 << 4) | (b7 >> 4)) << 3);
      uint16_t f60 = (uint16_t)((((b7 & 0xf) << 8) | b8) << 3);
      c->mem_w16(s + 8, f8); c->mem_w16(s + 10, f10); c->mem_w16(s + 12, f12);
      c->mem_w16(s + 0x38, f56); c->mem_w16(s + 0x3a, f58); c->mem_w16(s + 0x3c, f60);
      i++;
      if (!(i < c->mem_r8(obj + 9))) return;
      a2 += 4;
    }
  } else {
    // ---- Loop2: parity-gated, 3 fields (8/10/12), nibble-shared 4.5 bytes/limb ----
    for (uint32_t i = 0; ; ) {
      if (!(i < boundA)) return;
      uint32_t s = rd_u32(c, a2 + 192);
      uint16_t f8, f10, f12;
      if ((i + phase) & 1u) {
        // ODD: finish a pending shared nibble, then 4 more bytes.
        uint8_t c0 = c->mem_r8(stream), c1 = c->mem_r8(stream + 1), c2 = c->mem_r8(stream + 2);
        uint8_t c3 = c->mem_r8(stream + 3), c4 = c->mem_r8(stream + 4);
        stream += 5;
        f8  = (uint16_t)(((c0 & 0xf) << 8) | c1);
        f10 = (uint16_t)((c2 << 4) | (c3 >> 4));
        f12 = (uint16_t)(((c3 & 0xf) << 8) | c4);
      } else {
        // EVEN: 4 full bytes, leave the 5th byte's low nibble pending for the next (odd) index.
        uint8_t d0 = c->mem_r8(stream), d1 = c->mem_r8(stream + 1), d2 = c->mem_r8(stream + 2);
        uint8_t d3 = c->mem_r8(stream + 3), d4 = c->mem_r8(stream + 4);
        stream += 4;                                          // d4 stays pending (not consumed)
        f8  = (uint16_t)((d0 << 4) | (d1 >> 4));
        f10 = (uint16_t)(((d1 & 0xf) << 8) | d2);
        f12 = (uint16_t)((d3 << 4) | (d4 >> 4));
      }
      c->mem_w16(s + 8, f8); c->mem_w16(s + 10, f10); c->mem_w16(s + 12, f12);
      i++;
      if (!(i < c->mem_r8(obj + 9))) return;
      a2 += 4;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80077B5C — Animation::advanceLinkChain: countdown-tick + one-step 4-byte-stride tag chain
// walk. Shares node's countdown (+0xE) and cursor (+0x38) fields with loadFrame/step, but a
// DIFFERENT, coarser chain format (tag halfword at cur+2, jump-pointer word at cur+4 — vs the
// anim-VM's 8-byte stride / tag at cur+6 / jump at cur+8). Reused as a generic "tick + advance one
// small event chain" leaf by ~10 non-animation beh_ handlers (rec_dispatch(c, 0x80077B5Cu) /
// `leaf1(c, nd, 0x80077B5Cu)`).
uint32_t Animation::advanceLinkChain(uint32_t node) {
  Core* c = this->core;
  uint16_t v = (uint16_t)(c->mem_r16(node + 0xE) - 1);
  c->mem_w16(node + 0xE, v);
  if (v != 0) return 0;                                       // countdown still running

  uint32_t cur = c->mem_r32(node + 0x38);
  uint32_t tag = c->mem_r16(cur + 2) & 0xc000u;
  uint32_t newcur;
  uint32_t ret;
  switch (tag) {
    case 0x4000u: newcur = c->mem_r32(cur + 4); ret = 0; break;   // FOLLOW jump pointer
    case 0u:      newcur = cur + 4;             ret = 0; break;   // ADVANCE linear
    case 0x8000u:                                                  // TERMINAL/HOLD (no cursor move)
      c->mem_w16(node + 0xE, (uint16_t)(c->mem_r16(cur + 2) & 0x3fffu));
      return 1;
    default:      newcur = c->mem_r32(cur + 4); ret = 1; break;   // 0xc000: FOLLOW jump pointer
  }
  c->mem_w32(node + 0x38, newcur);
  c->mem_w16(node + 0xE, (uint16_t)(c->mem_r16(newcur + 2) & 0x3fffu));
  return ret;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80077C40 — Animation::attach: install animation-table entry `id` onto `node`. `table` is an
// array of struct* (stride 4); entryPtr = table[id]. entryPtr's descriptor halfword (at +6) low-12
// bits seed the countdown; loadFrame() consumes it exactly like the anim-VM would. If the
// descriptor's 0x2000 bit is set, dispatch the SAME frame executor (FUN_80075ff8) the VM's
// exec_tail uses, with the SAME (jump-pointer-vs-address) split on the descriptor's tag bits.
//
// Guest frame mirror — RE-INVESTIGATED 2026-07-08 (docs/findings/animation.md). The 2026-07-08
// revert above was based on the assumption that c->r[29] at attach's entry is "whatever an
// unrelated prior guest call left it at" and would therefore differ between SBS core A and core B.
// A direct probe (PSXPORT_DEBUG=animstack in rec_dispatch, comparing c->r[29] at every reach of
// 0x80077C40 on both cores over a full autonav run) DISPROVES that: r29 is IDENTICAL between A and
// B at every single call, every frame (all 4 leaf-dispatch call sites: beh_a06_scripted_actor,
// beh_id_routed_dispatch, beh_sop_intro_narration/lifted). This makes sense once stated: every
// caller reaches attach via rec_dispatch (leaf3/call3/direct), and rec_dispatch is a NATIVE C++ call
// on both cores — it never itself pushes/pops a guest frame, so whatever r29 the CALLER (a beh_*
// handler, itself invoked at a specific point in the per-object walk) currently holds is passed
// through unchanged to attach on both A and B alike. The prior revert's actual bug must therefore
// have been in the SPILL/RESTORE bookkeeping itself (e.g. wrong offsets or restoring stale values),
// not "there is no canonical frame". FUN_80077C40's real 32-byte frame (`addiu sp,-32; sw ra,24
// (sp); sw r17,20(sp); sw r16,16(sp)`, RE'd from generated/shard_5.c gen_func_80077C40) is mirrored
// below with the same LIVE-spill/restore RAII pattern as NodeXform's frames (node_xform.cpp) and
// Cull::performBaseCullFramed (cull.cpp). isDeadStackScratch's exclusion in sbs.cpp is REMOVED.
void Animation::attach(uint32_t node, uint32_t table, uint32_t id) {
  Core* c = this->core;
  uint32_t s16 = c->r[16], s17 = c->r[17], sra = c->r[31];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 24, sra);
  c->mem_w32(c->r[29] + 20, s17);
  c->mem_w32(c->r[29] + 16, s16);
  struct Restore {
    Core* c; uint32_t s16, s17, sra;
    ~Restore() {
      c->r[31] = c->mem_r32(c->r[29] + 24);
      c->r[17] = c->mem_r32(c->r[29] + 20);
      c->r[16] = c->mem_r32(c->r[29] + 16);
      c->r[29] += 32;
    }
  } restore{c, s16, s17, sra};
  const uint32_t entryPtr = rd_u32(c, table + id * 4);
  c->mem_w32(node + 0x38, entryPtr);
  uint32_t desc = c->mem_r16(entryPtr + 6);
  c->mem_w16(node + 0xE, (uint16_t)(desc & 0xfffu));
  loadFrame(node);

  // v0/v1 mirror (gen_func_80077C40 never sets v0 itself — the CALLER-visible v0 is whatever the
  // last callee left, and the SOP overlay call site at 0x8010B828 (ra=0x8010B830) BRANCHES on it.
  // Leaving native junk here flipped the lifted actor's pose-kickoff arm exactly once per anim
  // change (watch-cut f289: A snapped angles via the overlay's own FUN_80075FF8 call while B took
  // gen attach's executor path — one-frame pose transient, packet-pool divergence). Gen dataflow:
  //   FUN_80076904 returns 0 on every path (all three exits are failed `<` compares), then
  //   r3 = desc; r2 = r3 & 0x2000; early-exit leaves v0=0, v1=desc&0xC000 (delay slot);
  //   tag==0x8000 exit leaves v0=0xC000 (delay-slot r2 arm), v1=0x8000;
  //   executor exits return FUN_80075FF8's own v0/v1 (rec_dispatch below leaves them naturally.)
  desc = c->mem_r16(entryPtr + 6);                              // re-read (loadFrame may not touch it)
  uint32_t tag = desc & 0xc000u;
  if ((desc & 0x2000u) == 0) { c->r[2] = 0; c->r[3] = tag; return; }
  uint32_t a1;
  if (tag == 0x8000u) { c->r[2] = 0xC000u; c->r[3] = tag; return; }     // no executor call
  if (tag == 0x4000u || tag == 0xc000u) a1 = rd_u32(c, entryPtr + 8);   // follow jump pointer
  else                                  a1 = entryPtr + 8;              // (tag == 0) address itself
  c->r[4] = node; c->r[5] = a1; c->r[6] = (uint32_t)c->mem_r16s(node + 0xE);
  rec_dispatch(c, 0x80075ff8u);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80075F0C — Animation::applyFrame: per-frame KEYFRAME APPLIER. Advances node's own base pose
// by its stored per-frame delta, then (if any child limbs are registered) advances every child
// limb's position by ITS OWN delta the same way. RE'd via Ghidra headless (scratch/decomp
// cluster1.c, function FUN_80075f0c): a0=node, a1=snapCursor (only ever compared against 1).
void Animation::applyFrame(uint32_t node, int32_t snapCursor) {
  Core* c = this->core;
  c->mem_w16(node + 0x88, (uint16_t)(c->mem_r16(node + 0x88) + c->mem_r16(node + 0x90)));
  c->mem_w16(node + 0x8a, (uint16_t)(c->mem_r16(node + 0x8a) + c->mem_r16(node + 0x92)));
  c->mem_w16(node + 0x8c, (uint16_t)(c->mem_r16(node + 0x8c) + c->mem_r16(node + 0x94)));
  uint8_t childCount = c->mem_r8(node + 9);
  if (childCount != 0) {
    uint8_t limit = c->mem_r8(node + 8);
    uint32_t p = node;                                   // walks +0xC0 off successive +4 slots
    for (uint8_t i = 0; i < limit; i++, p += 4) {
      uint32_t child = c->mem_r32(p + 0xC0);
      c->mem_w16(child + 8,  (uint16_t)(c->mem_r16(child + 8)  + c->mem_r16(child + 0x10)));
      c->mem_w16(child + 10, (uint16_t)(c->mem_r16(child + 10) + c->mem_r16(child + 0x12)));
      c->mem_w16(child + 12, (uint16_t)(c->mem_r16(child + 12) + c->mem_r16(child + 0x14)));
      if (i + 1 >= childCount) break;
    }
  }
  if (snapCursor == 1) {
    c->mem_w32(node + 0x38, c->mem_r32(node + 0x38) | 0x80000000u);
  }
}

static void eov_animLoadFrame(Core* c)      { eng(c).animation.loadFrame(c->r[4]); c->r[2] = 0; }
static void eov_animAdvanceLink(Core* c)    { c->r[2] = eng(c).animation.advanceLinkChain(c->r[4]); }
static void eov_animAttach(Core* c)         { eng(c).animation.attach(c->r[4], c->r[5], c->r[6]); }
static void eov_animApplyFrame(Core* c)     { eng(c).animation.applyFrame(c->r[4], (int32_t)c->r[5]); }

// FUN_80076D68 (Animation::step) is DELIBERATELY LEFT UNWIRED here — investigated + reverted
// 2026-07-08 (docs/findings/animation.md has the full trace). stepFramed() (above) DOES mirror
// FUN_80076D68's real 40-byte guest-stack frame byte-exact — that part of the RE is correct and
// kept for future use. But wiring gov_animStep (via shard_set_override, so every direct substrate
// `func_80076D68(c)` call routes to the native body) reproducibly diverges at f3773 in
// packet_pool/packet_pool_ptrs (0x800BF4F4+), cascading to 30,000+ RAM bytes within a few hundred
// frames. Root-caused via a per-call A/B trace comparing every node/frame/return value: the
// divergence is NOT the frame (proved — it persists identically even routing gov_animStep through
// the fully UNFRAMED step() instead of stepFramed()) and NOT the decision logic (every TRACED
// node/return-value pair matched between cores). It isolates to exactly one call: node address
// 0x800E7E80 — which sits immediately after the active-object-pool free-counter
// (0x800E7E7C, see game/render/cull.cpp's CULL_FAR_MULT comment) — one specific call-site
// (ra=0x8005AA90) passes this address as FUN_80076D68's node argument. Excluding just that one
// address from the native route (a targeted A/B test, not a shipped fix) made the divergence
// disappear completely (0-diff through 7000+ frames). This means anim_vm_76d68 (the existing,
// PRE-EXISTING native reimplementation of FUN_80076D68 — not written or modified this session)
// has a genuine fidelity gap for whatever this pool-adjacent "node" really represents — most
// likely it aliases pool bookkeeping fields, not a real per-object animation struct, and the
// existing port doesn't reproduce the substrate's handling of that edge case. This is a logic bug
// in anim_vm_76d68 itself, not a stack-frame problem, and needs its own RE session (what does the
// substrate actually do with a "node" at the pool-adjacent address — is 0x800E7E80 a legitimate
// sentinel entry, or is this call site's argument computed wrong upstream). Out of scope for a
// frame-mirror fix: reverted to unwired rather than special-casing one address as a bandaid.
// step() stays reachable only via DIRECT native C++ callers (beh_actor_move_sm etc — see step()'s
// own header comment) and via the override registry (ActorZonedAttacker's call1() leaf-dispatch
// convenience), which never cross the guest ABI/stack boundary in the first place.
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80076904(Core*);
extern void gen_func_80077B5C(Core*);
extern void gen_func_80077C40(Core*);
extern void gen_func_80075F0C(Core*);

void Animation::registerOverrides() {
  // 0x80075F0C is ALSO reached by DIRECT substrate `func_<addr>(c)` calls (jal), not only via
  // rec_dispatch; install() puts the shared thunk into g_override[] so both paths are intercepted
  // uniformly (FUN_80075F0C's guest body has no stack-frame adjustment — generated/shard_4.c).
  using overrides::install;
  install(0x80076904u, "Animation::loadFrame",        eov_animLoadFrame,   gen_func_80076904, shard_set_override);
  install(0x80077B5Cu, "Animation::advanceLinkChain", eov_animAdvanceLink, gen_func_80077B5C, shard_set_override);
  install(0x80077C40u, "Animation::attach",           eov_animAttach,      gen_func_80077C40, shard_set_override);
  install(0x80075F0Cu, "Animation::applyFrame",       eov_animApplyFrame,  gen_func_80075F0C, shard_set_override);
}
