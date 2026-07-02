// PC-native engine STARTUP/INIT — reimplementing the engine's own init from the game's main entry point
// (crt0 -> FUN_80050b08 = ov_game_main). The engine boot does platform/PSX-library setup (ResetGraph,
// CdInit, VSync, DMA, heap — owned by the native platform) and then sets up the engine's OWN state:
// frame/double-buffer pacing, the display + GTE projection, and the camera. Those engine pieces are pure
// state setters with no PSX mechanism to preserve, so they are reimplemented PC-native here (readable C
// that sets the same engine state), replacing the 1:1 `rec_dispatch` transcription in ov_game_main.
//
// Per the boundary (CLAUDE.md): this is ENGINE init → reimplement PC-native. The values/widths are the
// engine's interface state that the rest of the engine + the retained PSX content read back, so they are
// set EXACTLY as the original (verified against the MAIN.EXE disassembly — store widths matter; e.g. the
// vblank counter is a halfword, the pacing/parity flags are bytes). RE source: scratch/decomp +
// MAIN.EXE disasm (tools/recomp decode). later-159.
#include "core.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);   // run a guest fn (for the few sub-bits not yet PC-native)

// FUN_80051794 — set an identity 3x3 rotation matrix (0x1000 = 1.0 fixed on the diagonal) + zero
// translation: 8 contiguous words [R11|R12, R13|R21, R22|R23, R31|R32, R33, 0, 0, 0]. (PSX "InitMatrix".)
static inline void eng_identity_matrix(Core* c, uint32_t p) {
  c->mem_w32(p + 0,  0x1000); c->mem_w32(p + 4,  0);
  c->mem_w32(p + 8,  0x1000); c->mem_w32(p + 12, 0);
  c->mem_w32(p + 16, 0x1000); c->mem_w32(p + 20, 0);
  c->mem_w32(p + 24, 0);      c->mem_w32(p + 28, 0);
}

// FUN_80050a0c — engine frame-state init: zero the vblank counter and the double-buffer / frame-pacing
// flags the main loop reads (DAT_1f800235 = frame-rate divisor, DAT_1f800135 = buffer parity,
// DAT_1f80019c = buffer-swap mode). Last write = FUN_8009a480(0x45) -> DAT_80105ee8 = 0x45 (a word).
void eng_init_framestate(Core* c) {
  c->mem_w16(0x800E809C, 0);     // vblank counter (sh)
  c->mem_w8 (0x1F800235, 2);     // frame-rate divisor / vblank pacing target
  c->mem_w8 (0x1F800135, 0);     // double-buffer parity
  c->mem_w8 (0x1F80019C, 0);     // buffer-swap mode flag
  c->mem_w8 (0x1F80023B, 0);
  c->mem_w8 (0x1F800233, 0);
  c->mem_w8 (0x1F800236, 0);
  c->mem_w8 (0x1F800234, 0);
  c->mem_w8 (0x1F80019B, 0);
  c->mem_w8 (0x1F80027E, 0);
  c->mem_w32(0x80105EE8, 0x45);  // FUN_8009a480(0x45)
}

// FUN_800509b4 — engine DISPLAY + GTE-projection init, PC-native. Sets the GTE projection control regs
// (the camera/projection the renderer reads: H, OFX/OFY, ZSF, DQ) and the projection-plane global. The
// libgte InitGeom (FUN_80083ff8) only does these CR writes + installs a cop2-unusable exception handler
// (FUN_80085810) that our always-on native GTE does not need, so it reduces to the CR writes here.
// FUN_80050738 (the PSX double-buffer draw/disp env structs) is still dispatched — those structs are read
// by native_step_frame's PutDrawEnv/PutDispEnv; a PC-native single display env is the next display step.
void eng_init_display(Core* c) {
  c->mem_w16(0x800E7E70, 0);     // DAT_800e7e70
  c->mem_w16(0x800E7E72, 8);     // DAT_800e7e72
  // InitGeom (FUN_80083ff8): GTE projection control registers.
  gte_write_ctrl(29, 0x155);            // ZSF3
  gte_write_ctrl(30, 0x100);            // ZSF4
  gte_write_ctrl(26, 1000);             // H (replaced by SetGeomScreen below)
  gte_write_ctrl(27, 0xFFFFEF9E);       // DQA
  gte_write_ctrl(28, 0x01400000);       // DQB
  gte_write_ctrl(24, 0);                // OFX
  gte_write_ctrl(25, 0);                // OFY
  // SetGeomOffset(160,120) (FUN_800846d0): screen-center projection offset, (x,y) << 16.
  gte_write_ctrl(24, (uint32_t)160 << 16);
  gte_write_ctrl(25, (uint32_t)120 << 16);
  c->mem_w16(0x801003F8, 350);          // DAT_801003f8 = projection plane H (eng_init_camera reads this)
  // SetGeomScreen(350) (FUN_800846f0): projection plane distance H.
  gte_write_ctrl(26, 350);
  rec_dispatch(c, 0x80050738u);         // FUN_80050738: draw/disp env structs (still PSX; native next)
}

// FUN_80050a80 — engine CAMERA init: identity camera-rotation matrix at scratchpad 0x1F8000F8 (the same
// camera matrix the renderer reads as SCR+0xF8) and a second matrix at 0x1F800118, then the camera
// position/state fields. _DAT_1f8000ec = 0x1000 (1.0 scale); _DAT_1f8000ee = H*-5 and _DAT_1f8000d8 =
// H*-0x50000 where H = DAT_801003f8 (the projection plane, signed halfword = 350, set by FUN_800509b4).
void eng_init_camera(Core* c) {
  eng_identity_matrix(c, 0x1F8000F8);
  eng_identity_matrix(c, 0x1F800118);
  int32_t h = c->mem_r16s(0x801003F8);   // projection plane H (set earlier; signed lh)
  c->mem_w32(0x1F8000DC, 0); c->mem_w32(0x1F8000E0, 0); c->mem_w32(0x1F8000E4, 0);
  c->mem_w32(0x1F8000D0, 0); c->mem_w32(0x1F8000D4, 0);
  c->mem_w16(0x1F8000E8, 0); c->mem_w16(0x1F8000EA, 0);
  c->mem_w16(0x1F8000EC, 0x1000);
  c->mem_w16(0x1F8000F0, 0); c->mem_w16(0x1F8000F2, 0); c->mem_w16(0x1F8000F4, 0);
  c->mem_w16(0x1F8000EE, (uint16_t)(h * -5));
  c->mem_w32(0x1F8000D8, (uint32_t)(h * -0x50000));
}

// FUN_800520e0 — engine SUBSYSTEM init (init-prefix slot, dispatched at native_boot.cpp once before the
// scheduler starts). Pure engine-state setup: NO GPU/DMA/SPU/GTE touch, fully synchronous. The body sets
// six engine flag fields and orchestrates four subsystem-init callees. We own the ORCHESTRATION + the six
// direct writes PC-native (the engine owns its subsystem-init sequence); the four callees stay dispatched
// to the retained PSX content for now (each is itself engine-state init — the next descent targets), and
// they are SYNCHRONOUS with no effective indirect calls at init (FUN_80086620's gated jalr sites need
// runtime counters >0x95, which are 0 here), so dispatching them is faithful. Faithful to 0x800520e0:
//   addiu sp,-24; sw ra,0x10; jal 0x8007b328; *0x800bf4fa=0xffff(h);
//   *0x800ecf4a=0(h); *0x800ecf4c/4d/4e/4f=0(b); jal 0x80088b00(a0=0x800bf4f8,a1=0x800bf51a);
//   jal 0x80086620(a0=1); jal 0x80087a60; epilogue.
//   callees: 8007b328 entity-pool init · 80088b00 allocator/dispatch-table init (a0/a1 = its struct span)
//            · 80086620 mode/subsystem control(1) · 80087a60 input subsystem init.
// FUN_8007b328 — ENTITY-POOL init: clear the 8-byte entity-pool control header @0x800fb160 (just before
// the entity list head DAT_800fb168) and set its control bytes, then FUN_8007b2c0(0) which writes four
// default fixed-point scale params to scratchpad. Owned PC-native (the entity/object model is engine-
// owned, see docs/engine_re.md). The header is the memset(0x800fb160,0,8)+{+1=1,+4=7,+5=9} net result.
// FUN_8007b2c0 is called with a0=0 here (deterministic: a0 is zeroed at the call site), so its a0==0
// branch is inlined (0x1f800170=0x8000, 0172=0x4000, 0174=0x2000, 0176=0x1000); the a0!=0 branch is a
// different code path never taken from this call site.
void eng_init_entity_pool(Core* c) {
  c->mem_w8(0x800fb160u, 0);             // memset(0x800fb160, 0, 8) ...
  c->mem_w8(0x800fb161u, 1);             // ... then the control bytes
  c->mem_w8(0x800fb162u, 0);
  c->mem_w8(0x800fb163u, 0);
  c->mem_w8(0x800fb164u, 7);
  c->mem_w8(0x800fb165u, 9);
  c->mem_w8(0x800fb166u, 0);
  c->mem_w8(0x800fb167u, 0);
  c->mem_w16(0x1f800170u, 0x8000);       // FUN_8007b2c0(0): default fixed-point scale params
  c->mem_w16(0x1f800172u, 0x4000);
  c->mem_w16(0x1f800174u, 0x2000);
  c->mem_w16(0x1f800176u, 0x1000);
}

// FUN_80086620 — engine MODE control: set the two mode flags (0x800abe88 bit0-state, 0x800abe8c
// bit1-state) from the a0 bitmask, and when a per-mode dwell counter (0x80102450/0x80102454) has
// reached >=150 run the mode's transition handler (*0x800abe3c). Returns the prior packed mode.
// Faithful to 0x80086620:
//   s1 = (*0x800abe8c<<1) | (*0x800abe88==0); if (s1==a0) return s1;  // no change
//   *0x800abe70=0;
//   bit0: if set -> *0x800abe88=0; if(*0x80102450>=150) handler(*0x800abe6c); *0x80102450=0;
//         else (clear) -> *0x800abe88=1;
//   bit1: if set -> *0x800abe8c=1; if(*0x80102454>=150) handler(*0x800abe6c+0xf0); *0x80102454=0;
//         else (clear) -> *0x800abe8c=0;
//   *0x800abe70=1; return s1;
// At the init call site (a0=1, both flags + both counters still BSS-zero) s1 == 1 == a0, so it
// EARLY-RETURNS with no writes — a verified no-op; the logic below reproduces that and the general case.
static uint32_t eng_init_mode_ctrl(Core* c, uint32_t a0) {
  uint32_t s1 = (c->mem_r32(0x800abe8c) << 1) | (c->mem_r32(0x800abe88) == 0 ? 1u : 0u);
  if (s1 == a0) return s1;                                  // mode unchanged -> nothing to do
  c->mem_w32(0x800abe70, 0);
  if (a0 & 1) { c->mem_w32(0x800abe88, 0);
    if ((int32_t)c->mem_r32(0x80102450) >= 150) { c->r[4] = c->mem_r32(0x800abe6c); rec_dispatch(c, c->mem_r32(0x800abe3c)); }
    c->mem_w32(0x80102450, 0);
  } else c->mem_w32(0x800abe88, 1);
  if (a0 & 2) { c->mem_w32(0x800abe8c, 1);
    if ((int32_t)c->mem_r32(0x80102454) >= 150) { c->r[4] = c->mem_r32(0x800abe6c) + 0xf0; rec_dispatch(c, c->mem_r32(0x800abe3c)); }
    c->mem_w32(0x80102454, 0);
  } else c->mem_w32(0x800abe8c, 0);
  c->mem_w32(0x800abe70, 1);
  return s1;
}

// FUN_80087a60 — a thin wrapper that just calls FUN_80086970; owned as eng_init_input.
// FUN_80086970 — engine INPUT subsystem init: orchestrates the input/controller setup. We own the
// direct engine-state writes + the call sequence PC-native and rec_dispatch the sub-init callees (each
// is lower-level input/content init — the dispatch-table handler *0x800abe3c runs in-context here, as it
// did under the recomp body). Faithful to 0x80086970:
//   FUN_80080890(); *0x800abe70=0;
//   FUN_80087400(a0=2, a1=0x80102440); FUN_800873f0(a0=2, a1=0x80102440);
//   v1=*0x800abe98; *v1=-2; *(v1+4) |= 1; FUN_80085b10(a0=3, a1=0);
//   FUN_800808a0();
//   handler(*0x800abe6c); handler(*0x800abe6c + 0xf0);     // *0x800abe3c, both run
//   *0x80102454=0; *0x80102450=0; *0x800abe70=1;
void eng_init_input(Core* c) {
  rec_dispatch(c, 0x80080890u);
  c->mem_w32(0x800abe70, 0);
  c->r[4] = 2; c->r[5] = 0x80102440u; rec_dispatch(c, 0x80087400u);
  c->r[4] = 2; c->r[5] = 0x80102440u; rec_dispatch(c, 0x800873f0u);
  uint32_t v1 = c->mem_r32(0x800abe98);
  c->mem_w32(v1, (uint32_t)-2);
  c->mem_w32(v1 + 4, c->mem_r32(v1 + 4) | 1);
  c->r[4] = 3; c->r[5] = 0; rec_dispatch(c, 0x80085b10u);
  rec_dispatch(c, 0x800808a0u);
  uint32_t handler = c->mem_r32(0x800abe3c);
  c->r[4] = c->mem_r32(0x800abe6c);          rec_dispatch(c, handler);
  c->r[4] = c->mem_r32(0x800abe6c) + 0xf0;   rec_dispatch(c, handler);
  c->mem_w32(0x80102454, 0);
  c->mem_w32(0x80102450, 0);
  c->mem_w32(0x800abe70, 1);
}

// FUN_80088b00 — engine ALLOCATOR / dispatch-table init. a0/a1 = a struct span (0x800bf4f8/0x800bf51a).
// Installs the 6-entry mode dispatch table (0x800abe38..0x800abe4c) + 0x800abe5c/6c, then builds the
// 480-byte allocator heap at 0x80102500: 2 per-mode records (stride 240), each tagging its source byte
// (s1 / s2) 0xff and writing a 6-byte run + the two roving pointers a2/a3 (+35 each). We own the direct
// writes + the loop native and rec_dispatch the 3 callees (FUN_80089160 pre-init, FUN_8009a340 = the
// 480-byte clear, FUN_80086738 post) in-context. Faithful to 0x80088b00 (a0=s1, a1=s2 from the caller).
void eng_init_alloc(Core* c) {
  uint32_t s1 = c->r[4], s2 = c->r[5];        // 0x800bf4f8 / 0x800bf51a from eng_init_subsystems
  c->mem_w32(0x800abe70, 0);
  c->mem_w32(0x800abe84, 0);
  c->r[4] = s1; rec_dispatch(c, 0x80089160u);
  uint32_t s0 = 0x80102500u;
  c->mem_w32(0x800abe38, 0x80088cc8u);   // lui 0x8009 + addiu -N (sign-extended) -> 0x8008xxxx
  c->mem_w32(0x800abe3c, 0x80088c60u);
  c->mem_w32(0x800abe40, 0x80088dccu);
  c->mem_w32(0x800abe44, 0x80088e88u);
  c->mem_w32(0x800abe48, 0x80089104u);
  c->mem_w32(0x800abe4c, 0x8008913cu);
  c->mem_w32(0x800abe6c, s0);
  c->mem_w32(0x800abe5c, 0x80088dbcu);
  c->r[4] = s0; c->r[5] = 480; rec_dispatch(c, 0x8009a340u);   // clear the 480-byte heap
  c->mem_w32(s0 + 48, s1);                     // once: seed record[0].src = s1
  c->mem_w32(s0 + 288, s2);                    // once: seed record[1].src = s2
  uint32_t a0 = s0 + 64, a3 = 0x801024b8u, a2 = 0x80102470u;
  for (int t0 = 0; t0 < 2; t0++) {             // 2 records (a0 == s0+64 throughout: both advance 240)
    uint32_t v0 = c->mem_r32(a0 - 16);         // = *(s0+48) -> the record's tagged source ptr (s1 / s2)
    uint32_t a1 = s0 + 93;
    c->mem_w32(a0 - 52, 0);
    c->mem_w32(a0 - 48, s0);
    c->mem_w8 (v0, 0xff);
    v0 = c->mem_r32(a0 - 16);
    c->mem_w8 (v0 + 1, 0);
    c->mem_w32(a0 - 4, a2);
    c->mem_w32(a0, a3);
    for (int v1 = 5; v1 >= 0; v1--) { c->mem_w8(a1, 0xff); a1 += 1; }  // 6-byte run
    a3 += 35; a2 += 35; a0 += 240; s0 += 240;
  }
  rec_dispatch(c, 0x80086738u);
  c->mem_w32(0x800abe70, 1);
}

void eng_init_subsystems(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  eng_init_entity_pool(c);               // entity-pool init (FUN_8007b328) — owned native
  c->mem_w16(0x800bf4fa, 0xffff);
  c->mem_w16(0x800ecf4a, 0);
  c->mem_w8 (0x800ecf4c, 0);
  c->mem_w8 (0x800ecf4d, 0);
  c->mem_w8 (0x800ecf4e, 0);
  c->mem_w8 (0x800ecf4f, 0);
  c->r[4] = 0x800bf4f8u; c->r[5] = 0x800bf51au; eng_init_alloc(c);  // allocator/dispatch table — owned native
  eng_init_mode_ctrl(c, 1);                    // mode control(1) — owned native (no-op at init)
  eng_init_input(c);                           // input subsystem init (FUN_80087a60->80086970) — owned native
  c->r[29] = sp; c->r[31] = ra;          // mirror epilogue: addiu sp,+0x18; jr ra
}
