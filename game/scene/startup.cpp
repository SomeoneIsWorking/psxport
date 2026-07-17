// PC-native engine STARTUP/INIT — reimplementing the engine's own init from the game's main entry point
// (crt0 -> FUN_80050b08 = game_main). The engine boot does platform/PSX-library setup (ResetGraph,
// CdInit, VSync, DMA, heap — owned by the native platform) and then sets up the engine's OWN state:
// frame/double-buffer pacing, the display + GTE projection, and the camera. Those engine pieces are pure
// state setters with no PSX mechanism to preserve, so they are reimplemented PC-native here (readable C
// that sets the same engine state), replacing the 1:1 `rec_dispatch` transcription in game_main.
//
// Per the boundary (CLAUDE.md): this is ENGINE init → reimplement PC-native. The values/widths are the
// engine's interface state that the rest of the engine + the retained PSX content read back, so they are
// set EXACTLY as the original (verified against the MAIN.EXE disassembly — store widths matter; e.g. the
// vblank counter is a halfword, the pacing/parity flags are bytes). RE source: scratch/decomp +
// MAIN.EXE disasm (tools/recomp decode). later-159.
//
// Structure: methods on `class Engine` (game/core/engine.h). Callers reach the init entry points as
// `eng(c).initFrameState()` etc. Was the free functions eng_init_* — promoted with the class-instance
// arc (no Core arg on the method surface; reach Core via `this->core`).
#include "engine.h"
#include "game_ctx.h"
#include "core.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);   // run a guest fn (for the few sub-bits not yet PC-native)

// FUN_80051794 (identity 3x3 rotation matrix + zero translation) is owned by `Mtx::identity`
// (game/math/mtx.cpp) — the same leaf; deduped 2026-07-08 (this file used to carry a redundant
// copy as `Engine::identityMatrixAt`). Callers use `mtxOf(c).identity(p)` directly.

// FUN_80050a0c — engine frame-state init: zero the vblank counter and the double-buffer / frame-pacing
// flags the main loop reads (DAT_1f800235 = frame-rate divisor, DAT_1f800135 = buffer parity,
// DAT_1f80019c = buffer-swap mode). Last write = FUN_8009a480(0x45) -> DAT_80105ee8 = 0x45 (a word).
void Engine::initFrameState() {
  Core* c = this->core;
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
void Engine::initDisplay() {
  Core* c = this->core;
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
  // Widescreen: shift the projection center to the aspect center (nw/2 = 214@16:9 / 280@21:9) so the
  // GUEST GTE itself projects the wider FOV — the guest OT packets AND every native re-projection then
  // agree on one center (avoids the ~54px double-image when only the native pass is widened). Off at 4:3 /
  // oracle (gpu_gpu_wide_engine()==0), so the substrate/SBS reference keeps the stock 160.
  { int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_ofx(Core*);
    int ofx = gpu_gpu_wide_engine(c) ? gpu_gpu_wide_engine_ofx(c) : 160;
    gte_write_ctrl(24, (uint32_t)ofx << 16); }
  gte_write_ctrl(25, (uint32_t)120 << 16);
  c->mem_w16(0x801003F8, 350);          // DAT_801003f8 = projection plane H (initCamera reads this)
  // SetGeomScreen(350) (FUN_800846f0): projection plane distance H.
  gte_write_ctrl(26, 350);
  rec_dispatch(c, 0x80050738u);         // FUN_80050738: draw/disp env structs (still PSX; native next)
}

// FUN_80050a80 — engine CAMERA init: identity camera-rotation matrix at scratchpad 0x1F8000F8 (the same
// camera matrix the renderer reads as SCR+0xF8) and a second matrix at 0x1F800118, then the camera
// position/state fields. _DAT_1f8000ec = 0x1000 (1.0 scale); _DAT_1f8000ee = H*-5 and _DAT_1f8000d8 =
// H*-0x50000 where H = DAT_801003f8 (the projection plane, signed halfword = 350, set by FUN_800509b4).
void Engine::initCamera() {
  Core* c = this->core;
  mtxOf(c).identity(0x1F8000F8);
  mtxOf(c).identity(0x1F800118);
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
// scheduler starts). Pure engine-state setup: NO GPU/DMA/SPU/GTE touch, fully synchronous.
// FUN_8007b328 — ENTITY-POOL init: clear the 8-byte entity-pool control header @0x800fb160 (just before
// the entity list head DAT_800fb168) and set its control bytes, then FUN_8007b2c0(0) which writes four
// default fixed-point scale params to scratchpad. Owned PC-native (the entity/object model is engine-
// owned, see docs/engine_re.md).
void Engine::initEntityPool() {
  Core* c = this->core;
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

// FUN_8007b2c0 — direction-mask seeder. Called with 0 at boot (initEntityPool above) and again
// from reloadEntityPool below with the staged per-area orientation byte; a nonzero arg reverses
// the 4 fixed-point words' order (mirrored-facing swap).
void Engine::seedDirectionMasks(bool flipped) {   // FUN_8007B2C0
  Core* c = this->core;
  if (!flipped) {
    c->mem_w16(0x1f800170u, 0x8000);
    c->mem_w16(0x1f800172u, 0x4000);
    c->mem_w16(0x1f800174u, 0x2000);
    c->mem_w16(0x1f800176u, 0x1000);
  } else {
    c->mem_w16(0x1f800170u, 0x1000);
    c->mem_w16(0x1f800172u, 0x2000);
    c->mem_w16(0x1f800174u, 0x4000);
    c->mem_w16(0x1f800176u, 0x8000);
  }
}

// FUN_8007b3f4 — re-copy the staged per-area entity-pool control bytes onto the live header
// initEntityPool seeded at boot, then reseed the direction masks from the staged orientation byte.
void Engine::reloadEntityPool() {   // FUN_8007B3F4
  Core* c = this->core;
  uint8_t orient = c->mem_r8(0x800bfe4cu);
  c->mem_w8(0x800fb166u, orient);
  c->mem_w8(0x800fb161u, c->mem_r8(0x800bf8a3u));
  c->mem_w8(0x800fb162u, c->mem_r8(0x800bfe4eu));
  c->mem_w8(0x800fb163u, c->mem_r8(0x800bfe4fu));
  c->mem_w8(0x800fb164u, c->mem_r8(0x800bf88au));
  c->mem_w8(0x800fb165u, c->mem_r8(0x800bf88bu));
  c->mem_w8(0x800fb167u, c->mem_r8(0x800bfe4du));
  seedDirectionMasks(orient != 0);
}

// FUN_80086620 — engine MODE control: file-local helper (only called from Engine::initSubsystems, which
// invokes it with a0=1 where both flags + both counters are still BSS-zero, so it EARLY-RETURNS with no
// writes — a verified no-op; the logic below reproduces that and the general case).
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

// FUN_80087a60 — a thin wrapper that just calls FUN_80086970; owned as initInput().
// FUN_80086970 — engine INPUT subsystem init: orchestrates the input/controller setup. We own the
// direct engine-state writes + the call sequence PC-native and rec_dispatch the sub-init callees.
void Engine::initInput() {
  Core* c = this->core;
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

// FUN_80088b00 — engine ALLOCATOR / dispatch-table init. `s1` / `s2` are the struct-span pointers
// (0x800bf4f8 / 0x800bf51a) that initSubsystems supplies. Installs the 6-entry mode dispatch table
// (0x800abe38..0x800abe4c) + 0x800abe5c/6c, then builds the 480-byte allocator heap at 0x80102500:
// 2 per-mode records (stride 240), each tagging its source byte 0xff and writing a 6-byte run + the
// two roving pointers a2/a3 (+35 each). We own the direct writes + the loop native and rec_dispatch
// the 3 callees (FUN_80089160 pre-init, FUN_8009a340 = the 480-byte clear, FUN_80086738 post) in-context.
void Engine::initAlloc(uint32_t s1, uint32_t s2) {
  Core* c = this->core;
  c->mem_w32(0x800abe70, 0);
  c->mem_w32(0x800abe84, 0);
  c->r[4] = s1; rec_dispatch(c, 0x80089160u);
  uint32_t s0 = 0x80102500u;
  c->mem_w32(0x800abe38, 0x80088cc8u);
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

void Engine::initSubsystems() {
  Core* c = this->core;
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  initEntityPool();                          // entity-pool init (FUN_8007b328) — owned native
  c->mem_w16(0x800bf4fa, 0xffff);
  c->mem_w16(0x800ecf4a, 0);
  c->mem_w8 (0x800ecf4c, 0);
  c->mem_w8 (0x800ecf4d, 0);
  c->mem_w8 (0x800ecf4e, 0);
  c->mem_w8 (0x800ecf4f, 0);
  initAlloc(0x800bf4f8u, 0x800bf51au);        // allocator/dispatch table — owned native
  eng_init_mode_ctrl(c, 1);                    // mode control(1) — owned native (no-op at init)
  initInput();                                 // input subsystem init (FUN_80087a60->80086970) — owned native
  c->r[29] = sp; c->r[31] = ra;          // mirror epilogue: addiu sp,+0x18; jr ra
}

// FUN_80086604 — Engine::activeModeCtx. Accessor: returns the active mode/draw-env context pointer
// held at the mode-state global 0x800ABE20. Leaf, no frame. (The gen body's trailing func_80086620
// is a fall-through decompiler artifact past the jr ra — not a callee.)
// ORACLE: gen_func_80086604
uint32_t Engine::activeModeCtx() {
  Core* c = this->core;
  return c->mem_r32(0x800ABE20);       // active mode/draw-env context ptr
}

// FUN_80086738 — Engine::installModeHandlers. Installs the mode handler table at 0x80102444:
// [+0] = 0x800867CC, [+4] = 0x80086764 (Engine::runModeEnter); zeroes the guard slot [-4]
// (0x80102440) and the trailing slot [+8] (0x8010244C). Leaf, no frame. Called from initAlloc.
// ORACLE: gen_func_80086738
void Engine::installModeHandlers() {
  Core* c = this->core;
  const uint32_t TABLE = 0x80102444u;                  // mode handler table base
  c->mem_w32(TABLE + 0, 0x800867CCu);                  // handler[0]
  c->mem_w32(TABLE + 4, 0x80086764u);                  // handler[1] = Engine::runModeEnter
  c->mem_w32(TABLE - 4, 0);                             // guard slot 0x80102440
  c->mem_w32(TABLE + 8, 0);                             // trailing slot 0x8010244C
}

// FUN_80086764 — Engine::runModeEnter. If both bit0 flags in the mode ctx (*0x800ABE98)[+0]/[+4]
// are set, dispatches the mode-enter handler at 0x800ABE60 (when non-null) and returns 1; otherwise
// returns 0. Ready-FRAME: sp descends 24, ra spilled at +16 (no callee-save s-regs). The ctx ptr is
// loaded BEFORE the frame descent, matching the guest.
// PORT_CHECK: UNPROVABLE — the mode-enter dispatch is an INDIRECT call through the runtime pointer
// at 0x800ABE60 (guest jalr v0), so port_check cannot statically resolve the target; the oracle does
// the identical indirect rec_dispatch(c, *0x800ABE60) with ra=0x800867B8. Frame + flag-gate + ra
// constant verified by hand against gen_func_80086764. Not a divergence.
// ORACLE: gen_func_80086764
uint32_t Engine::runModeEnter() {
  Core* c = this->core;
  uint32_t ctx = c->mem_r32(0x800ABE98);               // mode ctx ptr (loaded before frame descent)
  c->r[29] = c->r[29] + (uint32_t)-24;                 // addiu sp,-0x18 — descend the guest frame
  c->mem_w32(c->r[29] + 16, c->r[31]);                 // sw ra,0x10(sp) — LIVE incoming ra
  uint32_t result = 0;
  if ((c->mem_r32(ctx + 4) & 1u) != 0 &&
      (c->mem_r32(ctx + 0) & 1u) != 0) {               // both mode flags armed
    uint32_t handler = c->mem_r32(0x800ABE60);         // mode-enter handler
    if (handler != 0) { c->r[31] = 0x800867B8u; rec_dispatch(c, handler); }  // jalr handler
    result = 1;
  }
  c->r[31] = c->mem_r32(c->r[29] + 16);                // lw ra,0x10(sp)
  c->r[29] = c->r[29] + (uint32_t)24;                  // addiu sp,+0x18 — ascend the guest frame
  return result;
}
