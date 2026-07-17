// demo.cpp — PC-native ownership of the DEMO / front-end MENU stage state machine (the
// title/attract/menu front-end), mirroring engine.cpp's GAME-stage pattern. Boundary: the
// ENGINE owns the substate machine (which substate runs, what sm fields each transition writes); the
// per-substate SYSTEM work (the inner menu input machines, loaders, SFX, render) stays dispatched to
// the retained PSX content/system code. Full RE map: docs/engine_re.md "DEMO / front-end MENU stage".
//
// The DEMO stage lives in the DEMO overlay (loaded at base 0x80106228 — it ALIASES the GAME.BIN base,
// so the same addresses hold DIFFERENT code per overlay; the scan below distinguishes them by the
// root-dispatcher prologue signature). It runs as task-0's cooperative coroutine: the root dispatcher
// 0x801062E4 has a one-time prologue then a per-frame loop @0x80106388 that reads sm[0x48] (the
// SUBSTATE, sm = *0x1F800138), dispatches via word table @0x8010622C with `jr v0`, and the selected
// substate body ends by jumping to one of the TAIL labels (0x80106650 cf2c+attract / 0x80106658
// attract / 0x80106670 none), which bumps the frame counter (0x1F800198) and yields once via
// FUN_80051f80 (the SINGLE yield per frame), then loops.
//
// Each substate body is an INLINE block within the root function, reached by the loop's computed
// `jr v0` (so it runs with ra unchanged) and exited by `j <TAIL>`. This is exactly the ov_game_s4c
// shape: a native override at the body's address does the native work and coro-redirects to the
// chosen TAIL (the guest TAIL then runs the yield in-context). We do NOT override the root function,
// so the guest prologue/loop/yield run unchanged; the loop's `jr v0` into an owned body address
// triggers the override (the flat interp fires overrides on computed jumps — interp.cpp:481).
//
// OWNED HERE: s1, s2, s3, s6 — the substates whose only sub-call is SYNCHRONOUS (verified yield-free
// via tools/yield_reach.py: the inner machines 0x80106f80 / 0x8010696c / 0x80106ac4 / 0x8007b45c and
// the engine-update 0x8001cf2c / commit pair 0x80106824+0x80106690 / page-close 0x800750d8 never
// reach FUN_80051f80). Their inner machine is rec_dispatch'd synchronously, then the engine owns the
// transition LOGIC (sm[0x48] selection + field writes) natively. The DEEP-YIELDING substates — s0
// (loaders 0x80045080/0x80044bd4 yield + falls into s1), s4 (0x8007bf20 yields), s5 (0x80052078
// stage-restart yields), s7 (loader 0x80106c24 yields) — stay as guest code for now; owning them
// needs the coro-redirect-INTO-the-yielder handshake (a plain rec_dispatch of a deep yielder longjmps
// out and destroys the override C frame, killing task 0 — later-169). The inner menu sub-machines and
// loader/SFX/render callees stay dispatched until separately ported.
//
// REGISTER VALUES from the root prologue (the body comparisons use them): s2reg=1, s1reg=2, s3reg=3.
// (Disasm: 0x8010633c addiu s2,zero,1 · 0x80106340 addiu s1,zero,2 · 0x80106344 addiu s3,zero,3.)

#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry                         // Game::pc_skip — frame() case-0 fork
#include "cfg.h"
#include "scheduler.h"                    // native_task_spawn (FUN_80051F14 port) — Slip #4 s0 spawn
#include "world/pool.h"          // ov_pool_init_run (FUN_8007B18C) + siblings
#include "world/placement.h"     // ov_place_objects (FUN_80072A78)
#include "core/asset.h"          // class Asset — preloadTexgroup (static, area-load sync)
#include "sbs.h"                 // skipRendezvousReached — s5 DEMO->GAME frame alignment
#include <stdio.h>
#include <stdlib.h>

static const uint32_t SM_PTR   = 0x1f800138u;
static const uint32_t TAIL_CF2C = 0x80106650u;  // jal 0x8001cf2c (engine update) -> attract render -> yield
static const uint32_t TAIL_REND = 0x80106658u;  // jal 0x80075a80 (attract render) -> yield
static const uint32_t TAIL_NONE = 0x80106670u;  // frame-ctr++ -> yield (no render)

static void demo_log(const char* who, Core* c) {
  if (!cfg_dbg("demo")) return;
  uint32_t sm = c->mem_r32(SM_PTR);
  cfg_logf("demo", "%s sm[0x48]=%u 0x4a=%u 0x68=%u 0x6b=%u 0x50=%u", who,
           c->mem_r16(sm + 0x48), c->mem_r16(sm + 0x4a), c->mem_r8(sm + 0x68),
           c->mem_r8(sm + 0x6b), c->mem_r16(sm + 0x50));
}

// s1 0x8010641C — wait/advance: v0 = inner menu input machine 0x80106f80(0). If v0 != 0 the page is
// done -> sm[0x4a]=0, sm[0x48] += 1 (1 -> 2). Else if any pad edge (*0x800E7E68) request a skip
// (*0x1F80019D = 1). Always -> TAIL_NONE. (Faithful to 0x8010641C.)
void Demo::s1() { Core* c = core;
  demo_log("s1", c);
  c->r[4] = 0;                              // a0 = 0
  rec_dispatch(c, 0x80106f80u);             // inner menu input machine (SYNC)
  uint32_t v0 = c->r[2];
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 != 0) {
    uint16_t cur = c->mem_r16(sm + 0x48);
    c->mem_w16(sm + 0x4a, 0);
    c->mem_w16(sm + 0x48, cur + 1);         // sm[0x48]++  (1 -> 2)
  } else if (c->mem_r16(0x800e7e68u) != 0) {
    c->mem_w8(0x1f80019du, 1);              // skip-request
  }
  rec_coro_redirect(c, TAIL_NONE);
}

// s2 0x80106464 — sub-machine v0 = 0x8010696c(). Outcome 1 -> go to s7 (sm[0x48]=7, reset 0x4a/0x4c).
// Outcome 2 -> two-phase cursor trick on sm[0x68]: first pass (==0) -> s3 (sm[0x48]=3, sm[0x68]=2);
// second pass (!=0) -> s4 (sm[0x48]=4, sm[0x50]=0, sm[0x6b]=0, engine-update 0x8001cf2c,
// *0x800BF84A=0). Both phases then clear sm[0x4a] and -> TAIL_REND. Other outcomes -> TAIL_REND.
void Demo::s2() { Core* c = core;
  demo_log("s2", c);
  rec_dispatch(c, 0x8010696cu);             // sub-machine (SYNC)
  uint32_t v0 = c->r[2];
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 == 1) {                            // s2reg: -> s7
    c->mem_w16(sm + 0x48, 7);
    c->mem_w16(sm + 0x4a, 0);
    c->mem_w16(sm + 0x4c, 0);
    rec_coro_redirect(c, TAIL_REND);
    return;
  }
  if (v0 == 2) {                            // s1reg: cursor two-phase
    if (c->mem_r8(sm + 0x68) == 0) {        // first pass
      c->mem_w16(sm + 0x48, 3);
      c->mem_w8 (sm + 0x68, 2);             // sb s1reg(=2)
    } else {                                // second pass
      c->mem_w8 (sm + 0x68, 0);
      c->mem_w16(sm + 0x48, 4);
      c->mem_w16(sm + 0x50, 0);
      c->mem_w8 (sm + 0x6b, 0);
      rec_dispatch(c, 0x8001cf2cu);         // engine per-frame update (SYNC)
      c->mem_w8 (0x800bf84au, 0);
    }
    c->mem_w16(sm + 0x4a, 0);               // shared 0x801064dc: sm[0x4a]=0
    rec_coro_redirect(c, TAIL_REND);
    return;
  }
  rec_coro_redirect(c, TAIL_REND);
}

// s3 0x801064E8 — sub-machine v0 = 0x80106ac4() (mirror of 0x8010696c). Outcome 1 -> s7 (the same
// SHARED block as s2's outcome-1). Outcome 2 -> phase on sm[0x68]: ==2 -> s5 (sm[0x48]=5, sm[0x68]=0,
// *0x1F800134=0); else -> s6 (sm[0x48]=6, sm[0x6b]=0, sm[0x50]=0, page-close 0x800750d8,
// *0x800BF808=0). Outcome 3 (back/cancel) -> s2 (sm[0x48]=2, sm[0x68]=0). All -> TAIL_REND.
void Demo::s3() { Core* c = core;
  demo_log("s3", c);
  rec_dispatch(c, 0x80106ac4u);             // sub-machine (SYNC)
  uint32_t v0 = c->r[2];
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 == 1) {                            // s2reg: -> s7 (shared block)
    c->mem_w16(sm + 0x48, 7);
    c->mem_w16(sm + 0x4a, 0);
    c->mem_w16(sm + 0x4c, 0);
  } else if (v0 == 2) {                     // s1reg: phase on sm[0x68]
    if (c->mem_r8(sm + 0x68) == 2) {        // bne v0,s1reg(=2)
      c->mem_w16(sm + 0x48, 5);
      c->mem_w8 (sm + 0x68, 0);
      c->mem_w8 (0x1f800134u, 0);
    } else {
      c->mem_w8 (sm + 0x68, 0);
      c->mem_w16(sm + 0x48, 6);
      c->mem_w8 (sm + 0x6b, 0);
      c->mem_w16(sm + 0x50, 0);
      rec_dispatch(c, 0x800750d8u);         // page close (SYNC)
      c->mem_w8 (0x800bf808u, 0);
    }
  } else if (v0 == 3) {                     // s3reg: back/cancel -> s2
    c->mem_w16(sm + 0x48, 2);               // sh s1reg(=2)
    c->mem_w8 (sm + 0x68, 0);
  }
  rec_coro_redirect(c, TAIL_REND);
}

// ===========================================================================================
// DRAFT (UNWIRED, wide-RE fleet wave, band 0x8010A000-0x8010CFFF) — FUN_80106AC4, the main-menu
// title cursor sub-machine s3() dispatches (both s3() above and demo_frame_s3() below still
// `rec_dispatch(c, 0x80106ac4u)`; this method is NOT called from either yet). CONFIDENCE: HIGH —
// byte-exact Ghidra transcription (scratch/decomp/band_menu.c, imported from
// scratch/bin/tomba2/ram_menu.bin, Ghidra project `ram_menu`; regenerate with
// `tools/decomp.sh decomp ram_menu <out.c> list 0x80106ac4` if scratch/ was cleaned).
//
// "Mirror of 0x8010696C" (the s2 sub-machine, both files already document this) but with THREE
// outcomes instead of two: it also handles the Circle/back edge (the s2 twin only has confirm).
// sm = *0x1F800138 (SM_PTR).
//
// GUEST FRAME (2026-07-10 §9 re-verify correction): the original draft's confidence note ("no
// stack frame observed") was WRONG — ov_demo_gen_80106AC4 pushes `addiu sp,-32` and spills
// ra(r31)@sp+24, s1(r17)@sp+20, s0(r16)@sp+16 before any body work (checked directly against
// generated/ov_demo_shard_0.c). The two rec_dispatch calls inside (0x80106690/0x80106824, both
// still-substrate) clobber the shared r16/r17/r31 register file, so mirroring the frame is
// required for SBS byte-exactness, not optional. Restructured to a single exit point below so
// every return path shares the one epilogue.
//
//   entry: if sm[0x4a]==0 (fresh entry): sm[0x4a]=1, sm[0x5a]=0x1C2 (450, the intro/hold timer —
//     matches "sm[0x5a] inits 450" in the class RE map). else if sm[0x4a]!=1: return 0 (only valid
//     re-entry state is 1; guards a stray call with sm[0x4a] in {2,3,...} — never observed but
//     transcribed faithfully).
//   FUN_80106690(0) — inner menu commit/redraw leaf (a0=0), STILL SUBSTRATE (no native owner).
//   FUN_80106824(1, sm[0x68] != 2) — commit pair (a0=1, a1=bool), STILL SUBSTRATE.
//   timer = sm[0x5a] (pre-decrement value); sm[0x5a]--; if timer==1 (just expired): return 1
//     (outcome 1 = "attract launch", -> s7 in both callers).
//   else (timer still running): read pad edges (0x800E7E68). cursor := 3 by default.
//     if Down (0x20) pressed: cursor stays 3, cmp = sm[0x68] (no reassign).
//     else: cursor := 2; if Up (0x80) pressed: cmp = sm[0x68] (no reassign).
//       else (neither Down nor Up): if Confirm (0x4008) pressed: Sfx::trigger(0x11,0,0); return 2
//         (outcome 2 = "launch selected"). else if Circle/back (0x2000) pressed: Sfx::trigger(0x14,
//         -9, 0); return 3 (outcome 3 = "back/cancel" — the s2 twin does NOT have this branch).
//         else: return 0 (no edge — stay).
//     if cmp != cursor: Sfx::trigger(0x15,0,0) (cursor-move blip).
//     sm[0x68] = cursor; sm[0x4a] = 0; return 0.
uint32_t Demo::s3SubMachine() { Core* c = core;                 // FUN_80106AC4
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR17 = c->r[17];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 32u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR17);
  c->mem_w32(c->r[29] + 24u, savedR31);

  uint32_t result = 0;
  uint32_t sm = c->mem_r32(SM_PTR);
  bool proceed = true;
  if (c->mem_r16(sm + 0x4a) == 0) {
    c->mem_w16(sm + 0x4a, 1);
    c->mem_w16(sm + 0x5a, 0x1C2);
  } else if (c->mem_r16(sm + 0x4a) != 1) {
    proceed = false;
  }

  if (proceed) {
    c->r[4] = 0; rec_dispatch(c, 0x80106690u);                              // still substrate
    c->r[4] = 1; c->r[5] = (c->mem_r8(sm + 0x68) != 2) ? 1u : 0u;
    // ov_demo_gen_80106AC4:333 sets s1(r17)=0x1F800000 in the jal DELAY SLOT right before this call —
    // NOT an argument to 0x80106824 (a0/a1=r4/r5 only; 0x80106824 just spills+restores incoming r17
    // verbatim, never reading it), but the CALLER reusing its own callee-saved s1 as a scratch base
    // register it wants ready for the post-call read (line 334: sm = *(r17+312) = *0x1F800138). Since
    // 0x80106824 spills the INCOMING r17 to its OWN guest stack (sp+36) before restoring it, that spill
    // byte is part of the byte-exact guest-stack comparison — mirror the exact register prep so the
    // spilled value matches oracle (was leaking our loop's persistent r17=2 into that slot instead).
    c->r[17] = 0x1F800000u;
    rec_dispatch(c, 0x80106824u);                                           // still substrate

    sm = c->mem_r32(SM_PTR);                                                // reload (matches the recomp's re-read)
    int16_t timer = (int16_t)c->mem_r16(sm + 0x5a);
    c->mem_w16(sm + 0x5a, (uint16_t)(timer - 1));
    if (timer == 1) {
      result = 1;                                                           // attract launch
    } else {
      uint16_t edges = c->mem_r16(0x800e7e68u);
      uint8_t  cursor;
      uint8_t  cmp;
      bool     haveCmp = true;
      if (edges & 0x20u) {                                                  // Down
        cursor = 3;
        cmp = c->mem_r8(sm + 0x68);
      } else {
        cursor = 2;
        if (edges & 0x80u) {                                                // Up
          cmp = c->mem_r8(sm + 0x68);
        } else if (edges & 0x4008u) {                                       // Confirm
          eng(c).sfx.trigger(0x11, 0, 0);
          result = 2;
          haveCmp = false;
        } else if (edges & 0x2000u) {                                       // Circle / back
          eng(c).sfx.trigger(0x14, -9, 0);
          result = 3;
          haveCmp = false;
        } else {
          haveCmp = false;                                                  // no relevant edge -> result stays 0
        }
      }
      if (haveCmp) {
        if (cmp != cursor) eng(c).sfx.trigger(0x15, 0, 0);               // cursor-move blip
        c->mem_w8 (sm + 0x68, cursor);
        c->mem_w16(sm + 0x4a, 0);
      }
    }
  }

  c->r[31] = savedR31;
  c->r[17] = savedR17;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return result;
}

// FUN_8010696C — the TITLE main-menu cursor sub-machine (s2's rec_dispatch target). The s3SubMachine
// twin without the Circle/back (v0=3) outcome — the title menu can't cancel. sm = *0x1F800138 (SM_PTR).
// Byte-exact frame (32; r31/r17/r16 @ sp+24/20/16, per tools/abi_extract): the two render leaves
// 0x80106690/0x80106824 + the 3 Sfx::trigger (0x80074590) calls all spill the shared callee-saved file,
// so the frame AND the per-call callee-saved reg prep are required for SBS byte-exactness —
//   r16 = 0x1F800000 before the item/cursor draw (0x80106824 spills the incoming r16), and
//   r17 = 1 before the SFX calls (Sfx::trigger spills the incoming r17). ra constants are the RE'd
//   guest return addresses so the callees' spilled ra matches the oracle.
//
// Logic (identical to FUN_8010696c): fresh entry (sm[0x4a]==0) arms sm[0x4a]=1 + the attract hold-timer
// sm[0x5a]=0x1C2 (450); a stray re-entry with sm[0x4a] not in {0,1} returns 0. Then it redraws the logos
// (0x80106690) and the 2 menu items + cursor (0x80106824(0, sel!=0)), decrements the hold-timer, and on
// expiry (timer==1) returns 1 (attract launch -> s7). Otherwise it reads the pad edges (0x800E7E68):
// Right(0x20) selects item 1, Left(0x80) selects item 0 (each blips SFX 0x15 on an actual change and
// clears sm[0x4a]); with neither held, Confirm(0x4008) plays SFX 0x11 and returns 2 (launch selected);
// no relevant edge returns 0 (staying, sm[0x4a] left set).
uint32_t Demo::s2SubMachine() { Core* c = core;
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR17 = c->r[17];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 32u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR17);
  c->mem_w32(c->r[29] + 24u, savedR31);

  uint32_t result = 0;
  uint32_t sm = c->mem_r32(SM_PTR);
  bool proceed = true;
  if (c->mem_r16(sm + 0x4a) == 0) {
    c->mem_w16(sm + 0x4a, 1);
    c->mem_w16(sm + 0x5a, 0x1C2);
  } else if (c->mem_r16(sm + 0x4a) != 1) {
    proceed = false;
  }

  if (proceed) {
    c->r[4] = 0;                               c->r[31] = 0x801069B8u; rec_dispatch(c, 0x80106690u);  // logos
    c->r[4] = 0; c->r[5] = (c->mem_r8(sm + 0x68) != 0) ? 1u : 0u;
    c->r[16] = 0x1F800000u;                    c->r[31] = 0x801069E8u; rec_dispatch(c, 0x80106824u);  // items+cursor

    sm = c->mem_r32(SM_PTR);                                            // reload (matches recomp re-read)
    int16_t timer = (int16_t)c->mem_r16(sm + 0x5a);
    c->mem_w16(sm + 0x5a, (uint16_t)(timer - 1));
    if (timer == 1) {
      result = 1;                                                      // attract launch
    } else {
      uint16_t edges = c->mem_r16(0x800e7e68u);
      c->r[17] = c->r[0] + 1u;                                         // s1 = 1 (SFX callee spills it)
      if (edges & 0x20u) {                                             // Right -> select item 1 (Load Game)
        if (c->mem_r8(sm + 0x68) != 1) { c->r[31] = 0x80106AA4u; eng(c).sfx.trigger(0x15, 0, 0); }
        c->mem_w8 (sm + 0x68, 1);
        c->mem_w16(sm + 0x4a, 0);
      } else if (edges & 0x80u) {                                      // Left -> select item 0 (New Game)
        if (c->mem_r8(sm + 0x68) != 0) { c->r[31] = 0x80106A78u; eng(c).sfx.trigger(0x15, 0, 0); }
        c->mem_w8 (sm + 0x68, 0);
        c->mem_w16(sm + 0x4a, 0);
      } else if (edges & 0x4008u) {                                    // Confirm -> launch selected
        c->r[31] = 0x80106A44u; eng(c).sfx.trigger(0x11, 0, 0);
        result = 2;
      }
      // else: no relevant edge -> result stays 0, sm[0x4a] left set (re-enter next frame)
    }
  }

  c->r[31] = savedR31;
  c->r[17] = savedR17;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return result;
}

// ===================================================================================================
// Wiring (2026-07-10). ov_demo_gen_80106AC4 DOES have a direct intra-shard call site
// (ov_demo_shard_0.c:126, inside FUN_801064E8 = the substrate's OWN s3 body) — same shape as
// game/ai/actor_melee_engage.cpp, which would normally call for the ov_demo_set_override dual-wire.
// BUT: empirically wiring that dual-wire double-fires this address every frame on core A (SBS
// PSXPORT_DEBUG=dispatch showed TWO hits per frame, one via demo_frame_s3()'s rec_dispatch, one via
// "direct/g_override, bypassed rec_dispatch") — the DEMO stage's guest root coroutine (0x80106388,
// still substrate/interpreted, per this file's header) evidently keeps walking its OWN per-frame
// substate dispatch IN PARALLEL with the native "OWNED HERE" s1/s2/s3/s6 substates
// (demo_frame_s3() below), redundantly re-entering FUN_801064E8 a second time. Core B (psx_fallback,
// pure substrate — never runs any of this file's C++) reaches 0x80106AC4 via ONLY that same guest
// root-coroutine path, so its call COUNT is whatever the real PSX code does — comparing against it
// requires A's call sites to line up 1:1 with B's, not fire twice for B's once. registerOverrides()
// alone (the route demo_frame_s3()'s rec_dispatch actually uses) matches B's cadence; the direct call site
// inside FUN_801064E8 stays UNHOOKED so it falls through to the plain substrate ov_demo_gen_80106AC4
// body when the guest root coroutine's own redundant pass reaches it — identical to what ran there
// before this wiring pass (harmless, pre-existing, unrelated to this cluster). See docs/findings/ai.md.
namespace {
void ov_demoS3SubMachine(Core* c) {
  c->r[2] = eng(c).demo.s3SubMachine();
}
void ov_demoS2SubMachine(Core* c) {
  c->r[2] = eng(c).demo.s2SubMachine();
}
}  // namespace

extern void ov_demo_gen_80106AC4(Core*);
extern void ov_demo_gen_8010696C(Core*);

void Demo::registerOverrides(Game* /*game*/) {
  using overrides::install;
  // rec_dispatch-only (setter omitted): s3SubMachine's ONLY caller is rec_dispatch(0x80106AC4). Its
  // direct intra-shard call site (inside FUN_801064E8) is DELIBERATELY left on the substrate — a
  // g_override thunk here would intercept it too (see the wiring note above), so no setter. Same for s2.
  install(0x80106AC4u, "Demo::s3SubMachine", ov_demoS3SubMachine, ov_demo_gen_80106AC4);
  install(0x8010696Cu, "Demo::s2SubMachine", ov_demoS2SubMachine, ov_demo_gen_8010696C);
}

// s6 0x801065EC — page sub-machine 0x8007b45c(); if sm[0x50]==3 fire the commit pair 0x80106824(1,1)
// + 0x80106690(1). Then on sm[0x6b]: ==1 -> s3 (sm[0x48]=3, sm[0x68]=3) + TAIL_CF2C; ==2 -> s3
// (sm[0x48]=3, sm[0x68]=2) + TAIL_CF2C; else stay -> TAIL_REND.
void Demo::s6() { Core* c = core;
  demo_log("s6", c);
  rec_dispatch(c, 0x8007b45cu);             // page sub-machine (SYNC)
  uint32_t sm = c->mem_r32(SM_PTR);
  if (c->mem_r16(sm + 0x50) == 3) {
    c->r[4] = 1; c->r[5] = 1; c->r[31] = 0x80106618u; rec_dispatch(c, 0x80106824u);   // commit (a0=1,a1=1)
    c->r[4] = 1;             c->r[31] = 0x80106620u; rec_dispatch(c, 0x80106690u);    // commit2 (a0=1)
    sm = c->mem_r32(SM_PTR);
  }
  uint8_t s6b = c->mem_r8(sm + 0x6b);
  if (s6b == 1) {                           // s2reg
    c->mem_w16(sm + 0x48, 3);               // sh s3reg(=3)
    c->mem_w8 (sm + 0x68, 3);               // sb v0(=3)
    rec_coro_redirect(c, TAIL_CF2C);
  } else if (s6b == 2) {                    // s1reg
    c->mem_w16(sm + 0x48, 3);
    c->mem_w8 (sm + 0x68, 2);               // sb v0(=2)
    rec_coro_redirect(c, TAIL_CF2C);
  } else {
    rec_coro_redirect(c, TAIL_REND);
  }
}

// ===========================================================================================
// DEEP-YIELDING substates s0/s4/s5/s7 (later-183) — the ones whose substantive sub-call reaches
// the single cooperative yield FUN_80051f80, so they CANNOT be rec_dispatch'd from inside the
// override (a deep longjmp destroys the override's C frame and kills task 0 — later-168/169).
// They use the coro-redirect-INTO-the-yielder handshake (ov_game_s4c shape, engine.cpp):
// the override does the ownable native work + sets up the registers/ra the guest stream expects,
// then rec_coro_redirect to the guest address so the deep yield runs IN-CONTEXT (it longjmps to
// the scheduler and resumes correctly; the guest stream then reaches a TAIL and yields).
//
// rec_coro_redirect(c, target) sets the loop's NEXT pc to `target` (interp.cpp coro_next_pc) — the
// flat loop resumes there fresh, executing that instruction + its delay slot normally. So a redirect
// target must be the START of an instruction, and the override must have set up every register the
// redirected stream reads BEFORE its own setup runs. (For these bodies that is only the FIRST call's
// args; subsequent calls set their own args.)
//
// Yield-reachability (tools/yield_reach.py on ram_menu.bin, later-183):
//   s0 first loader 0x80045080 YIELDS (also 0x80044bd4 YIELDS); 0x8007982c/0x80075240/0x8001cf00 SYNC.
//   s4 0x8007bf20 YIELDS.   s5 0x80052078 YIELDS.   s7 phase machine 0x80106c24 YIELDS
//   (phase0 loader 0x80044bd4 + phase1 0x80106ee4/0x80106e28 YIELD; phase2 fully SYNC).

// s0 0x801063C0 — run-once INIT then loaders; FALLS THROUGH into s1 same frame. Disasm:
//   801063c0 lui a0,0x8011 / 801063c8 addiu a0,-28772        -> a0 = 0x80108F9C (loader-0 arg table)
//   801063c4 lw v1,0x138(s0) (s0=0x1f800000) -> v1 = task = mem[0x1f800138]
//   801063d4 sb zero,0x68(v1)                                 -> sm[0x68] = 0
//   801063cc lhu v0,0x48(v1) / 801063dc addiu v0,1 / 801063e0 sh v0,0x48(v1)  -> sm[0x48]++ (0 -> 1)
//   801063d0 addiu a1,zero,2                                  -> a1 = 2  (loader-0 arg)
//   801063d8 lw a2,0x138(s0)                                  -> a2 = task (loader-0 arg)
//   801063e4 jal 0x80045080 ; delay 801063e8 sh zero,0x4a(a2) -> sm[0x4a] = 0 (in the jal delay slot)
// OWNED native: the three sm field writes (sm[0x68]=0, sm[0x48]++, sm[0x4a]=0) and the loader-0 arg
// setup (a0/a1/a2). REDIRECT to the first loader jal 0x801063E4: 0x80045080 YIELDS, and after it the
// guest runs the remaining loaders 0x80044bd4/0x8007982c/0x80075240/0x8001cf00 (they set their own
// args) then falls through into the s1 body (0x8010641C) and on to TAIL_NONE — all in-context. The
// jal's delay slot re-writes sm[0x4a]=0 (idempotent, same value). The first jal sets its own ra
// (=0x801063EC), so we leave ra alone. sm[0x48] read as the CURRENT substate (0 here) then +1 = 1.
void Demo::s0() { Core* c = core;
  demo_log("s0", c);
  uint32_t sm = c->mem_r32(SM_PTR);
  c->mem_w8 (sm + 0x68, 0);                  // sb zero,0x68(v1)  sm[0x68] = 0
  uint16_t cur = c->mem_r16(sm + 0x48);      // lhu v0,0x48(v1)
  c->mem_w16(sm + 0x48, cur + 1);            // addiu v0,1 ; sh v0,0x48(v1)  sm[0x48]++  (0 -> 1)
  c->mem_w16(sm + 0x4a, 0);                  // (the jal-delay sh zero,0x4a will redo this; idempotent)
  c->r[4] = 0x80108f9cu;                     // a0 = loader-0 arg table (lui 0x8011 + addiu -28772)
  c->r[5] = 2;                               // a1 = 2
  c->r[6] = sm;                              // a2 = task (lw a2,0x138(s0))
  rec_coro_redirect(c, 0x801063E4u);         // first loader jal 0x80045080 (YIELDS) -> ... -> falls into s1
}

// s4 / s5 / s7 — an ENTRY-level (substate-body) override is NOT useful for any of these (deliberately):
// unlike s0, their substantive transition logic lives entirely AFTER their deep yield, so a body-level
// override could only set up args + coro-redirect straight into the guest yielder — a pure passthrough that
// OWNS NOTHING while adding task-0-death risk if a redirect target/ra is wrong. That is scaffolding, not
// ownership. (s7 IS owned, but NOT at its body 0x80106668 — at the phase machine 0x80106C24 it trampolines
// into, whose phase-selection prologue + phase2 teardown are ownable; see the s7 block below.)
//
// MECHANISM CONSTRAINT (verified in interp.cpp, this session): the address-keyed override table is
// consulted ONLY on `jal`/`j`/`jalr`/computed-`jr` CALL or JUMP targets (interp_flat lines 453-483).
// A `jr ra` RETURN does NOT consult it (line 477: `pc = tgt; continue`). So a "post-yield override" at
// the instruction AFTER a deep yielder returns is IMPOSSIBLE — that address is reached by `jr ra` and
// would never fire. (An earlier note proposing an override at s4's post-yield 0x8010658C was WRONG and
// is retracted.) This means:
//   - s4 0x80106580: its only engine logic is the sm[0x6b] branch at 0x8010658C, reached by `jr ra`
//     from the deep yielder 0x8007bf20 -> UNREACHABLE by an override. s4 STAYS GUEST (final, not a TODO).
//   - s5 0x801065DC: its ENTIRE body is `jal 0x80052078(2)` (LEAVE DEMO / task-restart) + the tail yield
//     — no engine logic to own. STAYS GUEST.
//   - s7 0x80106668: OWNED (later-208, ov_demo_s7_phase, registered) — its trampoline `jal 0x80106C24` IS an
//     override-checked jal target, and the phase machine 0x80106C24's phase SELECTION prologue (reads
//     sm[0x4a]) is PRE-yield, plus phase2's teardown (0x80106dfc: jal 0x80074bc4 SYNC ; *0x1f80019a=0 ;
//     sm[0x48]=0) is all-SYNC. We own phase selection + phase2 native, coro-redirect phase0/phase1 (which
//     yield) into the guest body. REACH (the prior "confirm a menu option" assumption was WRONG): s7 is the
//     ATTRACT-demo auto-launch — `tap 4008` (title s2->s3) then run ~455 frames so s3's intro timer sm[0x5a]
//     (450) expires -> sm[0x48]->7 auto. Verified fires at all phases + A/B 0-diff at steady phase1 (only the
//     coro saved-ra slot differs) + phase2 restart clean (poke sm[0x4a]=2). See ov_demo_s7_phase + later-208.
// s0 (above) is owned because it has genuine pre-yield engine state to own (init writes + substate sel).

// ===========================================================================================
// s7 PHASE MACHINE 0x80106C24 (later-186) — owned via the jal-target override + coro-redirect handshake.
//
// s7's body 0x80106668 is just a trampoline: `jal 0x80106C24` (delay nop) then falls into TAIL_NONE
// (0x80106670: frame-ctr++ -> yield). The `jal 0x80106C24` is an override-checked jal target (interp
// fires overrides on jal), so we own the phase machine 0x80106C24 itself (NOT s7's trampoline body —
// s7's only engine logic IS this phase machine; the trampoline does nothing but call it and yield).
//
// 0x80106C24 is a 3-phase machine on sm[0x4a] (the phase index). Disasm (sm = *0x1f800138, s0=0x1f800000,
// s1=1 set by the prologue; --ram ram_menu.bin):
//   PROLOGUE+SELECT 0x80106c24..70:
//     addiu sp,-40; sw s0,24(sp); lui s0,0x1f80; lw a0,0x138(s0)=task; sw ra,32(sp); sw s1,28(sp);
//     lhu v1,0x4a(a0)=phase; addiu s1,1;
//     beq v1,1 -> phase1 @0x80106d4c ; slti v0,v1,2; beq v0,0 -> @0x80106c64 (phase>=2 path);
//     addiu v0,2; beq v1,0 -> phase0 @0x80106c74 ; addiu v0,900; j epilogue @0x80106e14 (1<phase<2 = none);
//     @0x80106c64: beq v1,2 -> phase2 @0x80106dfc ; else j epilogue @0x80106e14.
//   => phase0 body @0x80106c74, phase1 body @0x80106d4c, phase2 body @0x80106dfc, else epilogue @0x80106e14.
//   EPILOGUE 0x80106e14: lw ra,32(sp); lw s1,28(sp); lw s0,24(sp); jr ra; addiu sp,+40
//     -> returns to the trampoline's post-jal 0x8010666c (delay nop) -> falls into TAIL_NONE 0x80106670.
//
//   PHASE0 (launch, sm[0x4a]==0) @0x80106c74 — DEEP-YIELDS:
//     sh s1(=1),0x4a(a0) [sm[0x4a]=1]; jal 0x8007a8e0; sh v0,0x5a(a0) [sm[0x5a]=timer];
//     cursor table: a0=0x800452c0,a2=1,a3=2; lbu sm[0x6e]; sllv<<2 + table@0x8010770c; lbu -> *0x800bf870;
//     sb 4,*0x800bf89c; jal 0x80044bd4 (LOADER, YIELDS); reinit jals 0x8007b18c/0x800796dc/0x800263e8/
//     0x80072a78/0x80075240/0x800783dc/0x80078610; lbu a0,sm[0x6e]; jal 0x80079464; sb 1,*0x1f80019a;
//     sm[0x6e]++ (wrap <3 -> else sm[0x6e]=0); j epilogue.
//   PHASE1 (wait/animate, sm[0x4a]==1) @0x80106d4c — DEEP-YIELDS:
//     optional draw jal 0x80079374 gated by *0x1f80017c & 0x10; if *0x800bf870==3 jal 0x80106ee4 else
//     jal 0x80106e28 (per-state update, YIELDS); sm[0x5a]--; if !=0 jal 0x800524b4(0); on timeout/clear
//     sm[0x4a]++ (->2); j epilogue.
//   PHASE2 (teardown, sm[0x4a]==2) @0x80106dfc — ALL SYNC:
//     jal 0x80074bc4 (verified SYNC via yield_reach.py: it sets *0x1f80027e=0 then calls 0x8001cf2c/
//     0x80074b44/0x80074e48, no incoming-arg dependence); lw v1,0x138(s0)=sm; sb zero,*0x1f80019a;
//     sh zero,0x48(v1) [sm[0x48]=0, restart front-end]; falls into epilogue.
//
// OWNERSHIP:
//   - phase SELECTION (read sm[0x4a]) — owned native here (pre-yield, trivially ownable).
//   - phase2 — owned FULLY native: rec_dispatch the SYNC 0x80074bc4 in-context, then *0x1f80019a=0,
//     sm[0x48]=0. Then coro-redirect straight to the trampoline's TAIL_NONE 0x80106670 — we NEVER entered
//     the 0x80106c24 stack frame (no prologue push), so there is nothing for the guest epilogue to restore;
//     ra at entry is already 0x8010666c (set by the trampoline's jal). 0x80106670 re-establishes its own
//     v0/v1/a0 (frame-ctr++ then the single yield), independent of s0/s1/ra/sp — so this is correct WITHOUT
//     replicating the s7-frame epilogue. This mirrors s2/s3/s6 redirecting to a TAIL.
//   - phase0 / phase1 — DEEP-YIELD, so they MUST run IN-CONTEXT. We replicate the prologue's stack frame
//     (sp-=40; sw incoming ra@32, s1@28, s0@24) so the body's eventual `jr ra` epilogue restores the
//     incoming ra (0x8010666c) and returns to the trampoline correctly. We set s0=0x1f800000 and s1=1 (the
//     two regs the bodies read that the prologue established), set a0=task (the body reads sm via a0 in
//     phase0's prologue-tail; phase1 reloads s0 itself), then coro-redirect to the body entry so the
//     in-body yields longjmp/resume in-context. We do NOT own any phase0/phase1 sm writes natively — their
//     substantive logic is all post-yield; pre-yield there is only sm[0x4a]=1 (phase0, redone by the body's
//     own `sh s1,0x4a` at its head). Redirecting to the body's HEAD (0x80106c74 / 0x80106d4c) re-runs that
//     head instruction normally, so we own nothing redundantly and risk nothing.
//   - else (phase index not in {0,1,2}) — redirect to the epilogue path's effective destination: with no
//     frame pushed, that is the trampoline TAIL_NONE 0x80106670 (the epilogue would `jr ra`=0x8010666c ->
//     0x80106670). Same reasoning as phase2's return.
static const uint32_t S7_PHASE0_BODY = 0x80106c74u;
static const uint32_t S7_PHASE1_BODY = 0x80106d4cu;
void Demo::s7Phase() { Core* c = core;
  uint32_t sm = c->mem_r32(SM_PTR);
  uint16_t phase = c->mem_r16(sm + 0x4a);
  cfg_logf("demo", "s7_phase sm[0x4a]=%u sm[0x48]=%u sm[0x6e]=%u sm[0x5a]=%u (ra=0x%08X)",
           phase, c->mem_r16(sm + 0x48), c->mem_r8(sm + 0x6e), c->mem_r16(sm + 0x5a), c->r[31]);

  if (phase == 2) {
    // PHASE2 teardown — fully native, all SYNC. No s7 frame entered -> redirect to the trampoline TAIL.
    rec_dispatch(c, 0x80074bc4u);             // teardown (SYNC: verified yield-free)
    c->mem_w8 (0x1f80019au, 0);               // *0x1f80019a = 0
    c->mem_w16(c->mem_r32(SM_PTR) + 0x48, 0); // sm[0x48] = 0 (restart front-end); re-read sm in case it moved
    rec_coro_redirect(c, TAIL_NONE);          // == 0x80106670, the trampoline's post-jal flow (frame-ctr++ -> yield)
    return;
  }

  if (phase == 0 || phase == 1) {
    // PHASE0/PHASE1 deep-yield: replicate the 0x80106c24 prologue frame so the body's `jr ra` epilogue
    // restores the incoming ra (=0x8010666c) and returns to the trampoline, then redirect into the body.
    uint32_t ra = c->r[31];
    c->r[29] -= 40;                           // addiu sp,-40
    c->mem_w32(c->r[29] + 24, c->r[16]);      // sw s0,24(sp)  (save INCOMING s0)
    c->mem_w32(c->r[29] + 32, ra);            // sw ra,32(sp)
    c->mem_w32(c->r[29] + 28, c->r[17]);      // sw s1,28(sp)  (save INCOMING s1)
    c->r[16] = 0x1f800000u;                   // s0 = 0x1f800000 (bodies read 0x138(s0); phase1 reloads it too)
    c->r[17] = 1;                             // s1 = 1 (phase0 head does `sh s1,0x4a`)
    c->r[4]  = sm;                            // a0 = task (phase0's prologue-tail reads sm via a0)
    rec_coro_redirect(c, phase == 0 ? S7_PHASE0_BODY : S7_PHASE1_BODY);
    return;
  }

  // else: phase index not in {0,1,2} — the guest would fall straight to its epilogue (`jr ra` ->
  // 0x8010666c -> TAIL_NONE). With no frame pushed, redirect directly to the trampoline TAIL.
  rec_coro_redirect(c, TAIL_NONE);
}

// DEMO stage entry (0x801062E4) — own the prologue PC-native, then hand to the guest per-frame loop body
// @0x80106388 via the coro-redirect handshake. Mirrors ov_game_stage_main (engine.cpp). Called
// DIRECTLY from the native scheduler (native_boot.cpp) when task 0 enters stage 1 (DEMO); the override
// table that used to intercept 0x801062E4 is gone (2026-06-22). WHY this is needed: run as pure PSX, the
// DEMO entry's prologue drives the libetc/libcd display+CD init which busy-waits on the vblank-IRQ VSync
// count (never advances in our cooperative no-IRQ runtime) → infinite spin. We replicate the prologue's
// guest-RAM init exactly (faithful to the 0x801062E4 disasm) and run its two sub-calls in-context; the
// guest loop body + substate dispatch + the single FUN_80051f80 yield then run via the resumed coroutine.
//
// Prologue disasm (0x801062E4, from a live DEMO RAM dump):
//   sp-=0x30; stack desc @sp+0x10 = {u16[0]=0, u16[1]=0, u16[2]=320(width), u16[3]=512}; save s0..s3/ra;
//   a0=sp+0x10,a1=a2=a3=0; jal 0x800810F0 (UI/print-stream ctx init);
//   *0x800BE0E4(u8)=0; *0x800E7E68(u16)=0; *0x800ECF54(u16)=0; *0x800ECF56(u16)=0;
//   *0x1F80019A(u8)=0; *0x1F80019D(u8)=0; s0=0x1f800000; s2=1; s1=2; s3=3; v1=*0x1F800138;
//   sm[0x48](u16)=0; sm[0x6E](u8)=0; jal 0x8005082C (input/pad reset); fall into loop @0x80106388.
// Register state the loop body reads MUST hold on hand-off: s0=0x1f800000, s2=1, s1=2, s3=3.
void Demo::stageMain() { Core* c = core;
  uint32_t ra = c->r[31], sp = c->r[29];
  uint32_t s0_in = c->r[16], s1_in = c->r[17], s2_in = c->r[18], s3_in = c->r[19];
  c->r[29] = sp - 0x30;
  c->mem_w16(c->r[29] + 0x10, 0);              // stack desc: x
  c->mem_w16(c->r[29] + 0x12, 0);              //             y
  c->mem_w16(c->r[29] + 0x14, 320);            //             w = 320 (screen width)
  c->mem_w16(c->r[29] + 0x16, 512);            //             h-ish field = 512
  c->mem_w32(c->r[29] + 0x28, ra);             // sw ra,0x28(sp)  — return on stage exit
  c->mem_w32(c->r[29] + 0x24, s3_in);          // sw s3,0x24(sp)
  c->mem_w32(c->r[29] + 0x20, s2_in);          // sw s2,0x20(sp)
  c->mem_w32(c->r[29] + 0x1c, s1_in);          // sw s1,0x1c(sp)
  c->mem_w32(c->r[29] + 0x18, s0_in);          // sw s0,0x18(sp)
  c->r[4] = c->r[29] + 0x10; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0;
  rec_dispatch(c, 0x800810F0u);                // UI / print-stream context init (leaf — synchronous)
  c->mem_w8 (0x800be0e4u, 0);
  c->mem_w16(0x800e7e68u, 0);
  c->mem_w16(0x800ecf54u, 0);
  c->mem_w16(0x800ecf56u, 0);
  c->mem_w8 (0x1f80019au, 0);
  c->mem_w8 (0x1f80019du, 0);
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->mem_w16(sm + 0x48, 0);                    // sm[0x48] = 0 (start at substate 0)
  c->mem_w8 (sm + 0x6e, 0);                    // sm[0x6e] = 0
  eng(c).modeStateArm.arm();                    // native — was rec_dispatch 0x8005082C(0,0,0)
  // Slip #5: the DEMO loop body (recomp ov_demo_gen_801062E4:73) dispatches FUN_80044BD4 to spawn
  // the front-end task at 0x800CF858. Native replaces this by directly running Demo::stageMain +
  // per-frame substate dispatch instead of task-spawning — so the RNG advance from FUN_80044BD4
  // is skipped. Match it here.
  (void)rngOf(c).next();

  // SLIP #1 residual fix (docs/findings/sbs.md attack (a)): do NOT dispatch s0 here. The recomp coro
  // of 0x801062E4 spends its FRESH iteration running the prologue only (sets sm[0x48]=0), then yields
  // via FUN_80051F80. It dispatches s0 on its NEXT iteration. If stageMain runs the s0 body inline
  // on fresh, native does prologue+s0 in one tick vs coro's prologue-only — putting A one tick ahead
  // for the entire DEMO progression. Leave sm[0x48]=0 for the next scheduler tick's demo.frame() to
  // dispatch as case 0.
  // (The old comment claiming "ov_demo_stage_main runs ONCE (prologue + s0)" was wrong for pacing
  //  parity — the recomp body pattern says stageMain = prologue only.)
}

// ===== DEMO per-frame NATIVE dispatcher (replaces the guest root loop @0x80106388) ==============
// The DEMO stage is driven one frame at a time by the native scheduler (native_boot.cpp): each frame
// it calls ov_demo_frame, which dispatches the current substate sm[0x48] natively (the guest loop's
// jr-v0 table dispatch can no longer be intercepted — override table gone), then bumps the per-frame
// counter (every guest tail does this). Substates are SYNCHRONOUS (one frame each), so this needs no
// coroutine. s0 runs once at entry (ov_demo_stage_main); s2..s7 are the current frontier.

// s1's inner menu input machine (0x80106F80): an 8-way state machine on sm[0x4a]. 0x80106F80 is a real
// callable function (proper prologue + `jr ra` epilogue, the inline state handlers `j` to it), so we
// own only its TWO libcd states natively and rec_dispatch the CD-free ones. CD states (scan of
// 0x80106F80..0x801072E0): sm[0x4a]==0 (0x80106FF0, CdControlB Setmode 0x80 @0x8010701C — busy-loops
// until the CD acks, which our no-IRQ libcd never does) and sm[0x4a]==7 (0x80107280, CdControlB
// @0x801072B0). Our disc reads by LBA need no drive mode, so the Setmode is a native no-op.
static uint32_t demo_menu_machine(Core* c) {
  uint32_t sm = c->mem_r32(SM_PTR);
  uint32_t s4a = c->mem_r8(0x1f80019du) ? 7u : c->mem_r16(sm + 0x4a);   // prologue: skip-flag -> state 7
  if (s4a == 0) {                              // state 0 (0x80106FF0): Setmode then advance, returns 0
    c->mem_w8(sm + 0x69, 0);                   // sb s1,0x69 — s1 (the 0x80106F80 a0 arg) is 0 here
    c->mem_w16(sm + 0x4a, c->mem_r16(sm + 0x4a) + 1);                   // sm[0x4a]++ : 0 -> 1
    return 0;                                  // guest handler j's to 0x801072DC (v0 = 0)
  }
  // States 1..3 (0x80107034) are pure sm[0x4a]++ advance steps — CD-free, run the real function (a
  // proper jr-ra function; only state 0/4..7 differ). States 4..7 are the OPENING MOVIE sub-sequence:
  // 4 (0x80107054) start the load of `\MOVIE\OP.STR;1` (CdControlB 0x8008B8F0); 5 (0x801070A4) stream a
  // chunk via strNext 0x8010755C; 6 (0x80107144) post/decode-draw; 7 (0x80107280) teardown (CD CdlPause
  // 0x80089E1C busy-loop) then returns nonzero ⇒ s1 advances to s2. Run pure-PSX they drive the libcd/STR
  // streaming our no-IRQ runtime can't complete. Own the whole sub-sequence natively: play OP.STR with the
  // self-contained native .STR player (native_fmv.cpp — already plays LOGO/OP at boot), then replicate
  // state 7's teardown and return nonzero so s1 advances to s2 (the front-end/title proper).
  if (s4a >= 4) {
    int skip = cfg_on("PSXPORT_NO_FMV") || cfg_on("PSXPORT_VK_HEADLESS");
    const char* nf = cfg_str("PSXPORT_NO_FMV");
    if (nf && atoi(nf) == 0 && *nf) skip = 0;                     // explicit PSXPORT_NO_FMV=0 forces FMV on
    if (!skip) c->game->fmv.play("MOVIE/OP.STR");
    else if (cfg_dbg("demo")) { static int w=0; if(!w++) cfg_logf("demo",
      "OP.STR opening movie skipped (NO_FMV/headless); running teardown -> s2"); }
    // state 7 (0x80107280) teardown, faithful — the CdControlB(CdlPause) busy-loop becomes a native no-op
    // (no async CD read is running in our model). The other callees are synchronous libgs/SPU/cleanup.
    c->r[4] = 0; rec_dispatch(c, 0x8001cf00u);                    // SPU CD->mix off
    c->r[4] = 0; rec_dispatch(c, 0x8009c820u);                    // libgs reset
    c->r[4] = 0; rec_dispatch(c, 0x8009c8bcu);                    // libgs reset
    rec_dispatch(c, 0x8008cd40u);                                 // CD-event cleanup
    rec_dispatch(c, 0x8008cce0u);                                 // alloc/dispatch cleanup (= ov_8008CCE0)
    c->mem_w8(0x1f80019cu, 0);
    c->mem_w8(0x1f80019du, 0);
    return 1;                                                    // nonzero ⇒ s1 advances sm[0x48] 1->2 (s2)
  }
  c->r[4] = 0;                                 // a0 = 0 (s1 calls 0x80106F80(0))
  rec_dispatch(c, 0x80106f80u);                // states 1..3: run the real function (jr ra), advances sm[0x4a]
  return c->r[2];                              // v0 (nonzero => menu machine done)
}

// Substate s0 (0x801063C0) — run-once INIT / restart: load the menu page resources, advance to s1.
// Called at DEMO stage entry (ov_demo_stage_main) AND on attract restart (ov_demo_frame case 0, after
// the attract item replaced the menu area). PC-native + SYNCHRONOUS: run as pure PSX the loaders
// descend into the IRQ-driven async CD reader (FUN_8001D940 / the FUN_80044BD4 task spawn) our no-IRQ
// runtime can't complete → infinite spin in libcd's VSync-timed CD_cw. We reimplement s0's loads with
// the SAME synchronous primitives stage-0 uses (Cd::dc40Sync / preload_texgroup), then advance to s1.
// Faithful to the 0x801063C0 disasm:
//   a0=0x80108F9C (the overlay slot right after GAME.BIN); sm[0x68]=0; sm[0x48]++ (0->1); sm[0x4a]=0;
//   jal 0x80045080(a0, idx=2, task)  -> indexed file load (table 0x800BE118, stride 8) via FUN_8001DC40
//   jal 0x80044BD4(0x80044F58, a1=2, a2=0, phase=0) -> texgroup area-load (spawn+yield-wait, sync here)
//   jal 0x8007982C; jal 0x80075240; jal 0x8001CF00(1)  -> control-block / audio-attr / SPU-mix (SYNC)
// s0 pre-yield (Slip #4): substrate order in state 0 body up to the FUN_80044BD4 yield —
//   sm[0x68] = 0
//   sm[0x48]++     (0 → 1)  ← advanced EARLY, matching substrate
//   sm[0x4a] = 0
//   loader 0x80045080(dest=0x80108F9C, idx=2)     (leaf, synchronous)
//   FUN_80044BD4 prelude: latch a1→0x801fe0de, a2→0x801fe0dd, clear done_flag 0x1f80019b, spawn
//   task-1 with entry FUN_80044F58 (preload). Substrate then yields waiting for the callback.
void Demo::s0PreYield() { Core* c = core;
  uint32_t sm = c->mem_r32(SM_PTR);
  c->mem_w8 (sm + 0x68, 0);
  c->mem_w16(sm + 0x48, 1);                    // sm[0x48]++ : 0 -> 1  (mirrors substrate)
  c->mem_w16(sm + 0x4a, 0);
  { uint32_t tab = 0x800be118u + 2u * 8u;
    c->game->cd.dc40Sync(0x80108f9cu, c->mem_r32(tab), c->mem_r32(tab + 4));
  }
  // FUN_80044BD4 prelude: match the substrate exactly, then use native_task_spawn (scheduler.cpp) as
  // the real port of FUN_80051F14 — task-1 runs via the Coro-fiber stanza under pc_faithful so its
  // FUN_80044F58 preload substrate signals done_flag=1 itself, at the same tick B does.
  c->mem_w8(0x801fe0deu, 2);
  c->mem_w8(0x801fe0ddu, 0);
  c->mem_w8(0x1f80019bu, 0);                    // done_flag = 0
  native_task_spawn(c, 1, 0x80044F58u);         // spawn task-1: FUN_80051F14 native port
}

// s0 post-yield (Slip #4): substrate order in state 0 body after FUN_80044BD4 returns —
//   FUN_8007982C (zero+seed the 1524B control block @0x800BF870)
//   FUN_80075240 (native pool.reset75240)
//   FUN_8001CF00(1) (SpuSetCommonAttr CD→SPU mix on)
// Then the substrate falls through into state 1's body; native's normal frame() resumes with s1.
void Demo::s0PostYield() { Core* c = core;
  rec_dispatch(c, 0x8007982Cu);
  eng(c).pool.reset75240();
  c->r[4] = 1; rec_dispatch(c, 0x8001CF00u);
  cfg_logf("demo", "s0 post-yield: sm[0x48]=1, task-1 preload done "
           "(texgroup meta[0x800FB170]=%08X)", c->mem_r32(0x800fb170u));
}

// Inline s0 body: task-1 wouldn't run via fiber under normal PC play, so the preload runs
// synchronously inline. Runs both halves + fake done_flag. Shared by both fork branches — the
// faithful pre-yield/post-yield task-1 split is a pending SBS-verified logic change (see demo.h).
void Demo::s0Body() { Core* c = core;
  s0PreYield();
  eng(c).asset.preloadTexgroup(0, 2);       // synchronous FUN_80044F58 (task-1 callback body)
  c->mem_w8(0x1f80019bu, 1);                    // done_flag = 1 (as if task-1 signalled)
  s0PostYield();
}
void Demo::s0Skip()     { s0Body(); }
void Demo::s0Faithful() { s0Body(); }

// Substate s1 (0x8010641C) — wait/advance. Run the menu machine; on completion (v0!=0) reset sm[0x4a]
// and advance sm[0x48] (1->2); else on any pad edge set the skip-request flag. Always TAIL_NONE.
// pc_faithful flavor: every state runs the REAL 0x80106F80 — the state-0 CdControlB Setmode,
// the skip-flag jump to state 7, and the state-7 CdlPause teardown are all platform-acked now
// (core B runs this same machine per-frame; the "never acks / busy-loops forever" era is over).
// The OP.STR playback states 4..6 only run when nothing sets the 0x1F80019D skip flag — if a
// flow reaches them, the STR streaming becomes the next frontier (fail-fast will name it).
static uint32_t demo_menu_machine_faithful(Core* c) {
  c->r[4] = 0;
  c->r[31] = 0x80106424u;
  rec_dispatch(c, 0x80106f80u);
  return c->r[2];
}

static void demo_frame_s1(Core* c) {
  uint32_t v0 = (c->game && !c->game->pc_skip) ? demo_menu_machine_faithful(c)
                                               : demo_menu_machine(c);
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 != 0) {
    c->mem_w16(sm + 0x4a, 0);
    c->mem_w16(sm + 0x48, c->mem_r16(sm + 0x48) + 1);                   // 1 -> 2
  } else if (c->mem_r16(0x800e7e68u) != 0) {   // pad pressed-edges
    c->mem_w8(0x1f80019du, 1);                 // request skip
  }
}

// Substate s2 (0x80106464) — title/menu sub-machine 0x8010696C (sets the intro timer sm[0x5a]=450,
// runs the title input). Outcome 1 -> s7 (attract launch); outcome 2 -> cursor two-phase (first pass
// -> s3, second pass -> s4). All paths end with TAIL_REND (attract render 0x80075A80 — draws the
// title). Mirrors ov_demo_s2 but return-based + runs the tail render inline. (Faithful to 0x80106464.)
static void demo_tail_rend(Core* c);
static void demo_tail_cf2c(Core* c);

static void demo_frame_s2(Core* c) {
  c->r[31] = 0x8010646Cu;
  rec_dispatch(c, 0x8010696cu);                // title sub-machine (SYNC)
  uint32_t v0 = c->r[2];
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 == 1) {                               // -> s7 (attract)
    c->mem_w16(sm + 0x48, 7); c->mem_w16(sm + 0x4a, 0); c->mem_w16(sm + 0x4c, 0);
  } else if (v0 == 2) {                        // cursor two-phase
    if (c->mem_r8(sm + 0x68) == 0) { c->mem_w16(sm + 0x48, 3); c->mem_w8(sm + 0x68, 2); }
    else {
      c->mem_w8(sm + 0x68, 0); c->mem_w16(sm + 0x48, 4); c->mem_w16(sm + 0x50, 0); c->mem_w8(sm + 0x6b, 0);
      c->r[31] = 0x801064DCu;
      rec_dispatch(c, 0x8001cf2cu); c->mem_w8(0x800bf84au, 0);   // engine update (SYNC)
    }
    c->mem_w16(sm + 0x4a, 0);
  }
  demo_tail_rend(c);                                // 0x80075A80 — TAIL_REND (forked below)
}

// TAIL helpers (the guest loop's per-frame tail render, run inline after a substate's transition):
//   TAIL_REND 0x80106658 = attract render 0x80075A80.  TAIL_CF2C 0x80106650 = engine-update 0x8001CF2C
//   then the attract render.  TAIL_NONE = no render. (The per-frame counter is bumped by ov_demo_frame.)
// pc_faithful tail: dispatch the REAL 0x80075A80 (the per-frame AUDIO-COMMAND queue processor —
// drains the 24-slot queue at 0x800BE1F8 into libsnd). Library/audio-glue code stays substrate
// under faithful (user 2026-07-07: lib fallback to recomp); the native AreaSlots::updateTail
// port serves pc_skip.
static void demo_tail_75a80_faithful(Core* c) {
  c->r[31] = 0x80106660u;
  rec_dispatch(c, 0x80075a80u);
}
static void demo_tail_75a80(Core* c) {
  (c->game && !c->game->pc_skip) ? demo_tail_75a80_faithful(c) : eng(c).areaSlots.updateTail();
}
static void demo_tail_rend(Core* c) { demo_tail_75a80(c); }
static void demo_tail_cf2c(Core* c) {
  c->r[31] = 0x80106658u;
  rec_dispatch(c, 0x8001cf2cu);
  demo_tail_75a80(c);
}

// Substate s3 (0x801064E8) — main-menu sub-machine 0x80106AC4 (mirror of 0x8010696C). Return-based
// twin of ov_demo_s3. Outcome 1 -> s7 (attract); 2 -> phase on sm[0x68]: ==2 -> s5 (LEAVE DEMO/New
// Game) clearing *0x1F800134; else -> s6 (sub-page); 3 (back/cancel) -> s2. All paths end TAIL_REND.
static void demo_frame_s3(Core* c) {
  c->r[31] = 0x801064F0u;
  rec_dispatch(c, 0x80106ac4u);                // main-menu sub-machine (SYNC)
  uint32_t v0 = c->r[2];
  uint32_t sm = c->mem_r32(SM_PTR);
  if (v0 == 1) {                               // -> s7
    c->mem_w16(sm + 0x48, 7); c->mem_w16(sm + 0x4a, 0); c->mem_w16(sm + 0x4c, 0);
  } else if (v0 == 2) {
    if (c->mem_r8(sm + 0x68) == 2) {           // -> s5 (start GAME)
      c->mem_w16(sm + 0x48, 5); c->mem_w8(sm + 0x68, 0); c->mem_w8(0x1f800134u, 0);
    } else {                                   // -> s6 (sub-page)
      c->mem_w8(sm + 0x68, 0); c->mem_w16(sm + 0x48, 6); c->mem_w8(sm + 0x6b, 0); c->mem_w16(sm + 0x50, 0);
      c->r[31] = 0x8010655Cu;
      rec_dispatch(c, 0x800750d8u); c->mem_w8(0x800bf808u, 0);   // page close (SYNC)
    }
  } else if (v0 == 3) {                        // back -> s2
    c->mem_w16(sm + 0x48, 2); c->mem_w8(sm + 0x68, 0);
  }
  demo_tail_rend(c);
}

// Substate s5 (0x801065DC) — LEAVE DEMO: the body is `jal 0x80052078(2)` (switch task 0 to stage 2 =
// GAME). Native: call the stage transition; the scheduler detects the DEMO->GAME entry change and hands
// off to GAME next frame (native_boot.cpp). No tail render (we are leaving the front-end).
static void demo_frame_s5(Core* c) {
  // FRAME ALIGNMENT (compare-mode only, same shape as engine.cpp "start_bin_load"): the oracle
  // spends ~12 frames of CD-paced overlay load between choosing New Game and rewriting task-0's
  // entry to GAME; startStage(2) collapses that into this one call, which put the skip pane a
  // constant 12 frames AHEAD through the whole narration cutscene (every SOP beat fired 12 frames
  // early — [sbs-skiptick] evidence, docs/findings/scene.md). Hold s5 (no state advance) until the
  // sibling core's task-0 entry has flipped to GAME (entry low-half 0x62E4 DEMO -> 0x637C GAME,
  // monotonic for this transition). No-op outside SBS MODE=skip (game->sbs null / mode gate).
  if (c->game->sbs) {
    uint32_t task = c->mem_r32(0x1f800138u);
    if (!c->game->sbs->skipRendezvousReached(c, task + 0xcu, 0x637Cu, "demo_start_game"))
      return;                                  // idle this frame — sm stays 5, retry next tick
  }
  eng(c).startStage(2);                     // = FUN_80052078(2): load GAME overlay, restart task 0
}

// Substate s4 (0x80106580) — LOAD GAME. The body runs the load sub-machine 0x8007bf20(0,0) then routes
// on sm[0x6b]: ==7 -> s5 (load complete, start GAME, *0x1f800134=1); ==1/2 -> back to s2 (cancel/back =
// the TITLE); else stay in s4 (continue the slot browser). NB the root dispatcher 0x801062E4 prologue
// loads s2=1, s1=2, s3=3 — so 0x80106580's `sh s1,0x48` writes sm[0x48]=2 (TITLE), not 1 (which would
// replay the opening movie). All transition paths run TAIL_CF2C
// (engine update 0x8001cf2c + attract render 0x80075a80); stay-path runs TAIL_REND only.
//
// The load sub-machine 0x8007bf20 is reimplemented native+SYNC (load_machine_s4 below): its case-0 disc
// load (FUN_80045558(1) = the async indexed reader FUN_8001dc40 to 0x8018a000 — the LOAD-MENU overlay)
// would spin forever in our no-IRQ runtime, exactly like the s0 loaders. We do that read SYNC (cd_dc40
// + mark 0x1f80019b done), then rec_dispatch the resident UI driver FUN_8007be18 (which calls the
// just-loaded overlay's slot browser FUN_8018fa88/fbcc). The memcard reads inside the browser are
// already sync+instant via the BIOS B0/A0 card HLE (memcard.cpp), so the browser does not yield.
static void load_machine_s4(Core* c) {
  uint8_t st = c->mem_r8(0x800bf84au);
  uint32_t sm;
  switch (st) {
    case 0:
      rec_dispatch(c, 0x8001cf2cu);                          // engine update
      { // FUN_80045558(1) = FUN_80045080(0x8018a000, idx=1) = FUN_8001dc40(dest, lba, size) — SYNC read
        uint32_t tab = 0x800be118u + 1u * 8u;               // indexed file table, stride 8 {lba,size}
        c->game->cd.dc40Sync(0x8018a000u, c->mem_r32(tab), c->mem_r32(tab + 4));
      }
      c->mem_w8(0x1f80019bu, 1);                              // mark the load complete (case 3 reads this)
      c->mem_w8(0x800bf84au, 1);
      c->r[4] = 0x0c; c->r[5] = 1; rec_dispatch(c, 0x800750d8u);   // FUN_800750d8(0xc, 1): page open
      break;
    case 1:
      c->r[4] = 0; rec_dispatch(c, 0x8007be18u);             // overlay slot browser (param2==0 path)
      sm = c->mem_r32(SM_PTR);
      if (c->mem_r16(sm + 0x50) > 1) c->mem_w8(0x800bf84au, 3);   // user picked/cancelled -> poll done
      break;
    case 2:                                                   // (param2==0 skips this; sync = always done)
      c->mem_w8(0x800bf84au, 3);
      break;
    case 3:
      if (c->mem_r8(0x1f80019bu) != 0) c->mem_w8(0x800bf84au, 4);
      break;
    case 4:
      c->r[4] = 0; rec_dispatch(c, 0x8007be18u);             // run the chosen action (sets sm[0x6b])
      c->mem_w8(0x800bf84au, 0);
      break;
    default:
      c->mem_w8(0x800bf84au, 0);
      break;
  }
}

static void demo_frame_s4(Core* c) {
  load_machine_s4(c);                            // = jal 0x8007bf20(0,0), native+sync
  uint32_t sm = c->mem_r32(SM_PTR);
  uint8_t s6b = c->mem_r8(sm + 0x6b);
  if (s6b == 1)      { c->mem_w16(sm + 0x48, 2); c->mem_w8(sm + 0x68, 1); demo_tail_cf2c(c); }  // back -> title
  else if (s6b == 2) { c->mem_w16(sm + 0x48, 2); c->mem_w8(sm + 0x68, 0); demo_tail_cf2c(c); }  // back -> title
  else if (s6b == 7) { c->mem_w16(sm + 0x48, 5); c->mem_w8(0x1f800134u, 1); demo_tail_cf2c(c); }// load ok -> GAME
  else               { demo_tail_rend(c); }
}

// Substate s6 (0x801065EC) — page sub-machine 0x8007B45C; if sm[0x50]==3 fire the commit pair
// 0x80106824(1,1)+0x80106690(1). Then on sm[0x6b]: ==1 -> s3 (sm[0x68]=3); ==2 -> s3 (sm[0x68]=2); both
// end TAIL_CF2C; else stay -> TAIL_REND. Return-based twin of ov_demo_s6.
static void demo_frame_s6(Core* c) {
  c->r[31] = 0x801065F4u;
  rec_dispatch(c, 0x8007b45cu);                // page sub-machine (SYNC)
  uint32_t sm = c->mem_r32(SM_PTR);
  if (c->mem_r16(sm + 0x50) == 3) {
    c->r[4] = 1; c->r[5] = 1; c->r[31] = 0x80106618u; rec_dispatch(c, 0x80106824u);   // commit (a0=1,a1=1)
    c->r[4] = 1;             c->r[31] = 0x80106620u; rec_dispatch(c, 0x80106690u);    // commit2 (a0=1)
    sm = c->mem_r32(SM_PTR);
  }
  uint8_t s6b = c->mem_r8(sm + 0x6b);
  if (s6b == 1) {       c->mem_w16(sm + 0x48, 3); c->mem_w8(sm + 0x68, 3); demo_tail_cf2c(c); }
  else if (s6b == 2) {  c->mem_w16(sm + 0x48, 3); c->mem_w8(sm + 0x68, 2); demo_tail_cf2c(c); }
  else                  demo_tail_rend(c);
}

// Substate s7 (trampoline 0x80106668 -> phase machine 0x80106C24) — the ATTRACT DEMO. A 3-phase
// machine on sm[0x4a]: phase0 launches an attract ITEM (load its area + reinit subsystems), phase1
// runs the item one frame at a time (the full game-engine per-frame update + the attract render),
// counting down sm[0x5a]; phase2 tears down and restarts the front-end at substate 0. The attract
// cycles items via sm[0x6e] through the cursor table @0x8010770c = {0,1,3} (stride 4, byte[0]); the
// selected item byte goes to *0x800bf870, which phase1 reads (==3 -> the type-3 item 0x80106ee4,
// else the gameplay item 0x80106e28). Faithful to 0x80106C24 (title_ram disasm). The deep area-load
// yield in phase0 is owned native+sync via native_transition_area_load — the SAME 0x800452c0 callback
// body the GAME-stage transition uses (the callback selects the area from sm[0x6d]/sm[0x6e], NOT from
// the 0x80044bd4 latched a1/a2, so a2=1-vs-0 is immaterial to the load). The per-frame item updates
// (0x80106e28 / 0x80106ee4) are return-based (one frame each, no yield) and run via rec_dispatch.
static void demo_frame_s7(Core* c) {
  uint32_t sm = c->mem_r32(SM_PTR);
  uint16_t phase = c->mem_r16(sm + 0x4a);
  cfg_logf("demo", "s7 attract phase=%u sm[0x6e]=%u sm[0x5a]=%u bf870=%u sm56=%02x",
           phase, c->mem_r8(sm + 0x6e), c->mem_r16(sm + 0x5a), c->mem_r8(0x800bf870u),
           c->mem_r8(sm + 0x56u));
  if (phase == 0) {
    // FRAME ALIGNMENT (SBS-harness-only, same shape as demo_frame_s5's "demo_start_game" barrier
    // above and engine.cpp's "start_bin_load"/"seqvab_build"): this collapses FUN_80044bd4(
    // 0x800452c0, entry, 1, 2)'s cooperative multi-frame spawn-and-wait into one synchronous call
    // (transitionAreaLoad below) — on the oracle sibling (SBS core B, the pure recomp substrate)
    // the equivalent load runs on task SLOT 1 across (usually) 1-2 real scheduler ticks, and its
    // completion only becomes visible on this core's OWN clock. Hold phase0 (no state advance —
    // gate BEFORE the phase0 body's first write) until the sibling core's OWN load-done flag lands.
    //
    // Key: *0x1F80019A, NOT the task-slot done_flag 0x1F80019B FUN_80044bd4 itself owns. done_flag
    // is REUSED by every cooperative slot-1 spawn (texgroup preload, the SEQ/VAB build, this area
    // load) and is never cleared back to 0 between them on the timescale this fork runs at — a
    // `skipRendezvousReached(0x1F80019B, 1, …)` gate would false-pass instantly on the VAB build's
    // stale 1 (confirmed live: PSXPORT_DEBUG=f465probe traced core B's 0x1F80019B sitting at 1
    // continuously from f0 through past f1500, never observably 0 at frame granularity — the
    // clear+reload+reset all land inside one scheduler tick). 0x1F80019A is DEMO-EXCLUSIVE: RE'd
    // (Ghidra decomp of a LIVE title-screen RAM capture, since this code lives in the DEMO overlay
    // and isn't resident in a mid-game dump) FUN_80106c24 phase0's tail write `sb s1(=1),0x1f80019a`
    // (0x80106d1c, mirrored at line ~890 below) is the ONLY writer of a nonzero value anywhere in the
    // call graph reachable from this fork; it is zeroed only by THIS SAME phase machine's phase2
    // teardown (0x80106dfc) and by DEMO's own init (s0/s0PostYield) — never by texgroup/VAB. Traced
    // live over 2 full attract cycles (PSXPORT_DEBUG=f465probe, f0..f1500): 0x1F80019A cleanly
    // toggles 0 (before each spawn) -> 1 (right after that spawn's own tail completes) -> 0 (that
    // item's teardown), with no unrelated writer observed. (Residual theoretical risk: if core A
    // ever ran a FULL ~900-frame attract cycle ahead of core B, B's flag could still read stale-1
    // from the PRIOR cycle; the observed cross-core skew on this fork is low single-digit frames,
    // nowhere near that, and the 3600-frame rendezvous deadlock-abort is the fail-fast backstop if
    // it ever were.) No-op outside SBS MODE=skip (game->sbs null / mode gate).
    //
    // width8=true is REQUIRED here, not cosmetic: skipRendezvousReached defaults to a 16-bit
    // (mem_r16) read, and 0x1F80019A's adjacent byte is 0x1F80019B — the globally-reused
    // cooperative-spawn done_flag, which sits near 1 almost permanently (see above). A 16-bit read
    // at 0x1F80019A folds that neighbor in (value = 19A | (19B<<8) on this little-endian host), so
    // it always reads >=256 and the gate would false-pass on frame one — caught live: the FIRST cut
    // of this fix (plain skipRendezvousReached, no width8) showed the fork-site audit's
    // 'demo_area_load' label at checks=17 stalls=0 maxWait=0 for an entire AUTO_SKIP run (never once
    // actually waited), and the f465 [AUDIO fx_area_ptrs] divergence was UNCHANGED — proof the
    // barrier was a no-op, not that the fork was already aligned. Fixed by adding the width8 param
    // to skipRendezvousReached itself (sbs.h/sbs.cpp) rather than picking a different, less-precise
    // key — the byte IS the correct signal, the API's fixed read-width was the actual bug.
    if (c->game->sbs &&
        !c->game->sbs->skipRendezvousReached(c, 0x1F80019Au, 1u, "demo_area_load", /*width8=*/true))
      return;                                    // idle this frame — sm[0x4a] stays 0, retry next tick
    // PHASE0 0x80106c74 — launch: advance to phase1, set the item timer, select+load the attract area.
    c->mem_w16(sm + 0x4a, 1);                                       // sh s1(=1),0x4a -> phase1
    // sm[0x5a] = 900: the phase-machine prologue loads v0=900 in the `beq v1,zero,phase0` DELAY SLOT
    // (0x80106c58 addiu v0,zero,0x384), and phase0's `sh v0,0x5a` sits in the `jal 0x8007a8e0` DELAY
    // SLOT — so it stores that 900, NOT 0x8007a8e0's return (0x8007a8e0 is called only for side effects:
    // it clears 0x1f80017c and seeds the control block). The item plays 900 frames (~15s) then times out.
    rec_dispatch(c, 0x8007a8e0u);                                   // side effects (clears 0x1f80017c)
    c->mem_w16(sm + 0x5a, 900);                                     // sh v0(=900),0x5a (delay-slot const)
    uint8_t cur   = c->mem_r8(sm + 0x6e);
    uint8_t entry = c->mem_r8(0x8010770cu + (uint32_t)cur * 4);     // cursor table {0,1,3}, byte[0]
    c->mem_w8(0x800bf870u, entry);                                  // sb v1,0x800bf870 (phase1 selector)
    c->mem_w8(0x800bf89cu, 4);                                      // sb 4,0x800bf89c (load mode)
    // area-load FUN_80044bd4(0x800452c0, entry, 1, 2): latch + run the callback body synchronously.
    c->mem_w8(0x801fe0deu, entry);
    c->mem_w8(0x801fe0ddu, 1);
    c->mem_w8(0x1f80019bu, 0);
    // FUN_80044bd4's a3==2 TAIL — shared helper PcScheduler::bd4Tail (game/core/pc_scheduler.cpp;
    // see its doc comment + docs/findings/scene.md "pc_skip FUN_80044BD4-collapse INCOMPLETENESS
    // class"): stores the RNG stamp (FUN_8009A450's RETURN VALUE, not a literal — the earlier fix
    // here wrote a literal 1, which is the #53 value bug) as a HALFWORD at sm+0x56, then since
    // a3==2 bumps the 16-bit wait counter at 0x1f800198 and dispatches FUN_8007fd54 (item-launch
    // icon/label placement; takes no incoming a0..a3, derives x/y/cell from 0x1f800198). Ordering
    // is load-bearing: this tail must run BEFORE SV_CHECK(...transitionAreaLoad()) below, not
    // after — running it after would observe 0x1f80019b already ==1 (the callback we just
    // finished) and take the guest's OTHER branch (the stamp store still fires, but the
    // counter-bump + FUN_8007fd54 dispatch is skipped entirely).
    c->game->pcSched.bd4Tail(sm, /*flag=*/2);
    SV_CHECK(c, 0x800452C0u, eng(c).sop.transitionAreaLoad(), rec_dispatch(c, 0x800452C0u));   // = sync 0x800452c0; sets 1f80019b=1 (observable-gated)
    // reinit subsystems (all SYNC; no incoming args / self-args)
    eng(c).pool.init();       // 0x8007B18C — native (via LIVE gated entry)
    eng(c).pool.resetControlBlock();           // 0x800796DC — native
    eng(c).pool.seedAreaObjects();           // 0x800263E8 — native
    eng(c).placement.placeAreaObjects();       // 0x80072A78 — native (field object-placement driver)
    eng(c).pool.reset75240();           // 0x80075240 — native
    eng(c).pool.setupViewScroll();           // 0x800783DC — native
    eng(c).pool.finalViewInit();           // 0x80078610 — native
    sm = c->mem_r32(SM_PTR);
    c->r[4] = c->mem_r8(sm + 0x6e); rec_dispatch(c, 0x80079464u);   // jal 0x80079464(sm[0x6e])
    c->mem_w8(0x1f80019au, 1);                                      // sb s1(=1),0x1f80019a
    uint8_t ne = (uint8_t)((c->mem_r8(sm + 0x6e) + 1) & 0xff);
    c->mem_w8(sm + 0x6e, ne < 3 ? ne : 0);                          // sm[0x6e]++ wrap<3 -> {0,1,2}
  } else if (phase == 1) {
    // PHASE1 0x80106d4c — run one frame of the attract item, then count down sm[0x5a].
    if (c->mem_r16(0x1f80017cu) & 0x10) rec_dispatch(c, 0x80079374u);   // optional draw (gated)
    if (c->mem_r8(0x800bf870u) == 3) rec_dispatch(c, 0x80106ee4u);      // type-3 item update+render
    else                              rec_dispatch(c, 0x80106e28u);      // gameplay item update+render
    sm = c->mem_r32(SM_PTR);
    uint16_t t = (uint16_t)((c->mem_r16(sm + 0x5a) - 1) & 0xffff);
    c->mem_w16(sm + 0x5a, t);                                       // sm[0x5a]--
    int advance = 0;
    if (t != 0) {
      c->r[4] = 0; rec_dispatch(c, 0x800524b4u);                    // skip/abort check (v0!=0 => skip)
      if (c->r[2] != 0) advance = 1;
    } else advance = 1;                                             // timer expired
    if (advance) c->mem_w16(sm + 0x4a, c->mem_r16(sm + 0x4a) + 1);  // sm[0x4a]++ -> phase2
  } else if (phase == 2) {
    // PHASE2 0x80106dfc — teardown + restart the front-end (sm[0x48]=0 -> s0 reinit -> title).
    rec_dispatch(c, 0x80074bc4u);                                   // teardown (SYNC, verified yield-free)
    sm = c->mem_r32(SM_PTR);
    c->mem_w8 (0x1f80019au, 0);
    c->mem_w16(sm + 0x48, 0);                                       // sm[0x48]=0 (restart)
  } else {
    // phase index out of range — match the guest epilogue (no-op) and don't spin.
    cfg_logf("demo", "s7 attract: phase=%u out of range (no-op)", phase);
  }
}

// Called once per frame by the native scheduler for the DEMO task.
void Demo::frame() { Core* c = core;
  uint32_t sm = c->mem_r32(SM_PTR);
  uint16_t s48 = c->mem_r16(sm + 0x48);
  switch (s48) {
    case 0: (c->game && !c->game->pc_skip) ? s0Faithful() : s0Skip(); break;   // restart (attract -> title): reload menu resources, -> s1
    case 1: demo_frame_s1(c); break;
    case 2: demo_frame_s2(c); break;
    case 3: demo_frame_s3(c); break;
    case 4: demo_frame_s4(c); break;    // LOAD GAME (memcard slot browser)
    case 5: demo_frame_s5(c); return;   // left DEMO -> GAME (no frame-ctr bump; we're gone)
    case 6: demo_frame_s6(c); break;
    case 7: demo_frame_s7(c); break;    // attract demo (idle timeout)
    default:
      if (cfg_dbg("demo")) { static int w = 0; if (!w++) cfg_logf("demo",
        "ov_demo_frame: sm[0x48]=%u not yet native (frontier — s4 needs conversion)", s48); }
      break;
  }
  c->mem_w16(0x1f800198u, c->mem_r16(0x1f800198u) + 1);   // per-frame counter (every guest tail bumps it)
}

// pc_faithful DEMO stage body (fiber task; see demo.h). Byte shape: ov_demo_gen_801062E4.
// r16..r19 carry the substrate's live callee-saved constants (0x1F800000 / 2 / 1 / 3) across the
// loop — deeper callees spill them, so they must match core B's at every dispatch boundary.
void Demo::stageBodyFaithful() { Core* c = core;
  c->r[29] -= 48;
  const uint32_t sp = c->r[29];
  c->mem_w16(sp + 16, 0);                       // draw-env rect local {x,y,w,h-ish}
  c->mem_w16(sp + 18, 0);
  c->mem_w16(sp + 20, 320);
  c->mem_w16(sp + 22, 512);
  c->mem_w32(sp + 40, c->r[31]);
  c->mem_w32(sp + 36, c->r[19]);
  c->mem_w32(sp + 32, c->r[18]);
  c->mem_w32(sp + 28, c->r[17]);
  c->mem_w32(sp + 24, c->r[16]);
  c->r[4] = sp + 16; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0;
  c->r[31] = 0x80106328u;
  rec_dispatch(c, 0x800810F0u);                 // UI / print-stream context init
  c->r[16] = 0x1F800000u;                       // s0..s3 loop constants (live for deep spills)
  c->r[18] = 1; c->r[17] = 2; c->r[19] = 3;
  uint32_t sm = c->mem_r32(SM_PTR);
  c->mem_w8 (0x800be0e4u, 0);
  c->mem_w16(0x800e7e68u, 0);
  c->mem_w16(0x800ecf54u, 0);
  c->mem_w16(0x800ecf56u, 0);
  c->mem_w8 (0x1f80019au, 0);
  c->mem_w8 (0x1f80019du, 0);
  c->mem_w16(sm + 0x48, 0);
  c->r[4] = 0; c->r[5] = 0; c->r[6] = 0;
  c->r[31] = 0x80106388u;
  c->mem_w8 (sm + 0x6e, 0);
  rec_dispatch(c, 0x8005082Cu);                 // mode-state arm (substrate leaf)
  for (;;) {
    sm = c->mem_r32(SM_PTR);
    uint16_t sub = c->mem_r16(sm + 0x48);
    switch (sub < 8 ? sub : 8) {
      case 0: {                                 // L_801063C0 — init loaders; falls into s1
        c->r[4] = 0x80108F9Cu;                  // loader-0 arg table
        c->mem_w8(sm + 0x68, 0);
        uint16_t s48 = c->mem_r16(sm + 0x48);
        c->r[5] = 2;
        c->r[6] = c->mem_r32(SM_PTR);
        c->mem_w16(sm + 0x48, s48 + 1);
        c->r[31] = 0x801063ECu;
        c->mem_w16(sm + 0x4a, 0);               // jal delay slot: sh zero,0x4a(a2)
        rec_dispatch(c, 0x80045080u);           // indexed file load (platform-sync)
        c->r[4] = 0x80044F58u; c->r[5] = 2; c->r[6] = 0; c->r[7] = 0;
        c->r[31] = 0x80106404u;
        rec_dispatch(c, 0x80044BD4u);           // spawn-and-wait (registered override -> native prim)
        c->r[31] = 0x8010640Cu; rec_dispatch(c, 0x8007982Cu);
        c->r[31] = 0x80106414u; rec_dispatch(c, 0x80075240u);
        c->r[4] = 1;
        c->r[31] = 0x8010641Cu; rec_dispatch(c, 0x8001CF00u);
      } [[fallthrough]];
      case 1: demo_frame_s1(c); break;
      case 2: demo_frame_s2(c); break;
      case 3: demo_frame_s3(c); break;
      case 4: demo_frame_s4(c); break;
      case 5:                                   // L_801065DC — LEAVE DEMO: stage swap parks the fiber
        c->r[4] = 2;
        c->r[31] = 0x801065E4u;
        rec_dispatch(c, 0x80052078u);           // does not return (entry rewrite + yield)
        break;
      case 6: demo_frame_s6(c); break;
      case 7:                                   // L_80106668 — ATTRACT: run the real phase
        c->r[31] = 0x80106670u;                 // machine; its deep loads yield on the fiber
        rec_dispatch(c, 0x80106C24u);
        break;
      default: break;                           // sub >= 8: straight to the tail
    }
    c->mem_w16(0x1f800198u, c->mem_r16(0x1f800198u) + 1);   // L_80106674 frame counter
    c->r[4] = 1;
    c->r[31] = 0x80106688u;
    rec_dispatch(c, 0x80051F80u);               // loop-tail yield (registered override -> yieldPrim)
  }
}

// Register the DEMO substate overrides when the just-loaded overlay is the DEMO overlay at the stage
// base. Distinguish from GAME.BIN (which aliases the same base) by the root-dispatcher prologue:
// DEMO 0x801062E4 = `addiu sp,sp,-48` (0x27bdffd0); GAME's entry is 0x8010637C = 0x27bdffe0 (and at
// DEMO's 0x8010637C sits a `sh zero,0x48(v1)` 0xa4600048, so GAME's stage_scan signature won't match
// DEMO either). AUTO so the overrides flush when the overlay unloads and the base is reused.
