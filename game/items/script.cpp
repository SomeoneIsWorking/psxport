// engine/script.cpp — PC-native per-object SCRIPT-VM subsystem.
// The per-object script-VM tick (FUN_8004CE14) — the most-called field function. Control flow + object
// memory owned native; every sub-behavior stays reachable by address via rec_dispatch (each honors its
// own override identically). NO GTE, NO render packets. Extracted verbatim from game_tomba2.cpp (one
// behavior, byte-identical) into its own module for PC-game code structure. The `scriptvm` diagnostic
// A/B gate (full RAM+scratchpad vs rec_super_call) is a REPL channel, unchanged.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// FUN_8004CE14 — per-object SCRIPT-VM tick (the MOST-CALLED field function, ~14900 calls/run). a0 = obj.
// Dispatches on the state byte obj[4]: 2 -> no-op; 3 -> jal 0x8007A624(obj); >3 -> no-op; 0 -> if the
// global enable byte 0x800BF873!=0 set obj[4]=3 & return, else INIT (obj[4]=1, obj[0]=1, load the per-obj
// behavior fn ptr from table 0x800A3F00[obj[3]] into obj[108], obj[116]=0, jal 0x8004B354(obj,0)) then
// fall into state 1. State 1 is the VM: a pause/mode gate (global 0x800BF870/871 + scratchpad 0x1F800207
// + the per-obj run-condition obj[3]) decides whether to run the command loop. The loop walks the
// 16-byte-stride command stream at cursor obj[108] (s4): opcode lbu[s4]==0xFF terminates; else a flag
// byte s4[2] bit7 picks predicate 0x8004D7EC (clear) vs 0x8004D868 (set), gated by the per-slot mask
// obj[116] & (1<<idx); a passing entry executes either 0x80111CCC(s4[12]) (when 0x800BF870==1 &&
// 0x800BF871>=15) or the cull/anim call 0x80077ACC(obj, s4[4], s4[6], s4[8]); a nonzero return ORs the
// slot bit into obj[112]. On terminator: obj[106]=slot count, obj[11]=31, obj[1]=1, jal 0x80077EFC(obj).
// CONTROL FLOW + memory ops are owned native; every jal sub-behavior stays interpreted via rec_dispatch
// (each honors its own override identically in the super-call path). `scriptvm` gate = full RAM+scratchpad
// A/B vs rec_super_call (each path runs once from one checkpoint; the native run is rolled back).
static void script_vm_4ce14(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t s5  = obj + 96;
  uint8_t state = c->mem_r8(obj + 4);
  if (state == 2) { c->r[2] = 2; return; }
  if (state == 3) { c->r[4] = obj; rec_dispatch(c, 0x8007A624u); return; }   // v0 = sub return
  if (state > 3)  { c->r[2] = 3; return; }
  if (state == 0) {
    if (c->mem_r8(0x800BF873u) != 0) { c->mem_w8(obj + 4, 3); c->r[2] = 3; return; }  // global not enabled yet
    uint32_t fnptr = c->mem_r32(0x800A3F00u + (uint32_t)c->mem_r8(obj + 3) * 4);
    c->mem_w8(obj + 4, 1);
    c->mem_w8(obj + 0, 1);
    c->mem_w32(s5 + 20, 0);          // obj[116] = 0 (slot mask)
    c->mem_w32(s5 + 12, fnptr);      // obj[108] = behavior fn ptr (cursor base)
    c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x8004B354u);
    // fall through into state 1
  }
  // ---- STATE 1: pause/mode gate, then the command loop ----
  uint8_t G0 = c->mem_r8(0x800BF870u);
  if (G0 == 0) {
    uint8_t o3 = c->mem_r8(obj + 3);
    if (o3 == 1) {
      if (c->mem_r8(0x1F800207u) >= 28) { c->r[2] = 0; return; }      // run only when scratch<28
    } else if (o3 == 2) {
      if (c->mem_r8(0x1F800207u) < 28)  { c->r[2] = 1; return; }      // run only when scratch>=28
    }
    // o3 not 1/2 -> always run
  } else {
    if (G0 == 6 && c->mem_r8(0x800BF871u) == 19) { c->r[2] = 19; return; }
    // else run
  }
  // ---- command loop ----
  uint32_t s4 = c->mem_r32(s5 + 12);   // cursor
  c->mem_w32(s5 + 16, 0);              // obj[112] = 0 (result accumulator)
  uint32_t s3 = 0;                     // slot index
  uint32_t v0_ret = 0;                 // v0 at function exit (from the terminator sub-call)
  for (;;) {
    if (c->mem_r8(s4) == 0xFF) {       // terminator opcode
      c->mem_w16(s5 + 10, (uint16_t)s3);   // obj[106] = slot count
      c->mem_w8(obj + 11, 31);
      c->mem_w8(obj + 1, 1);
      c->r[4] = obj; rec_dispatch(c, 0x80077EFCu);
      v0_ret = c->r[2];
      break;
    }
    uint32_t mask = 1u << (s3 & 31);
    uint8_t  flag = c->mem_r8(s4 + 2);
    bool s2set = (flag & 0x80) != 0;
    bool skip = false;
    if (!s2set) {                                        // bit7 clear -> predicate 0x8004D7EC
      c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 10); c->r[5] = 0;
      rec_dispatch(c, 0x8004D7ECu);
      if (c->r[2] != 0) skip = true;
    }
    if (!skip && (c->mem_r32(s5 + 20) & mask)) skip = true;   // slot already done
    if (!skip && s2set) {                                // bit7 set -> predicate 0x8004D868
      c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 10); c->r[5] = 0;
      rec_dispatch(c, 0x8004D868u);
      if (c->r[2] != 0) skip = true;
    }
    if (!skip && (c->mem_r32(s5 + 20) & mask)) skip = true;   // re-check (predicate may have set it)
    if (!skip) {
      uint32_t ret;
      if (c->mem_r8(0x800BF870u) == 1 && c->mem_r8(0x800BF871u) >= 15) {
        c->r[4] = (uint32_t)c->mem_r8(s4 + 12);
        rec_dispatch(c, 0x80111CCCu);
        ret = c->r[2];
      } else {
        c->r[4] = obj;
        c->r[5] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 4);
        c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 6);
        c->r[7] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 8);
        rec_dispatch(c, 0x80077ACCu);
        ret = c->r[2];
      }
      if (ret != 0) c->mem_w32(s5 + 16, c->mem_r32(s5 + 16) | mask);
    }
    s3++; s4 += 16;
  }
  c->r[2] = v0_ret;
}

void ov_script_vm_4ce14(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("scriptvm") ? 1 : 0;
  if (!s_v) { script_vm_4ce14(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  script_vm_4ce14(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8004CE14u);
  uint32_t v0_o = c->r[2];
  // Ignore FUN_8004CE14's OWN 56-byte stack frame [sp-56, sp): the gen prologue saves regs there and the
  // native body never touches the guest stack, so those bytes are dead-below-sp on return. (Sub-call stack
  // frames are identical between paths — both use the guest stack via rec_dispatch / interpreted jals.)
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 56) ? sp - 56 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[scriptvm] MISMATCH obj=%08x state=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           obj, c->mem_r8(obj + 4), v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[scriptvm] %ld matches\n", ng);
}
