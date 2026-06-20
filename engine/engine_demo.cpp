// engine_demo.cpp — PC-native ownership of the DEMO / front-end MENU stage state machine (the
// title/attract/menu front-end), mirroring engine_stage.cpp's GAME-stage pattern. Boundary: the
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
#include "cfg.h"
#include <stdio.h>

static const uint32_t SM_PTR   = 0x1f800138u;
static const uint32_t TAIL_CF2C = 0x80106650u;  // jal 0x8001cf2c (engine update) -> attract render -> yield
static const uint32_t TAIL_REND = 0x80106658u;  // jal 0x80075a80 (attract render) -> yield
static const uint32_t TAIL_NONE = 0x80106670u;  // frame-ctr++ -> yield (no render)

static void demo_log(const char* who, Core* c) {
  if (!cfg_dbg("demo")) return;
  uint32_t sm = c->mem_r32(SM_PTR);
  fprintf(stderr, "[demo] %s sm[0x48]=%u 0x4a=%u 0x68=%u 0x6b=%u 0x50=%u\n", who,
          c->mem_r16(sm + 0x48), c->mem_r16(sm + 0x4a), c->mem_r8(sm + 0x68),
          c->mem_r8(sm + 0x6b), c->mem_r16(sm + 0x50));
}

// s1 0x8010641C — wait/advance: v0 = inner menu input machine 0x80106f80(0). If v0 != 0 the page is
// done -> sm[0x4a]=0, sm[0x48] += 1 (1 -> 2). Else if any pad edge (*0x800E7E68) request a skip
// (*0x1F80019D = 1). Always -> TAIL_NONE. (Faithful to 0x8010641C.)
static void ov_demo_s1(Core* c) {
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
static void ov_demo_s2(Core* c) {
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
static void ov_demo_s3(Core* c) {
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

// s6 0x801065EC — page sub-machine 0x8007b45c(); if sm[0x50]==3 fire the commit pair 0x80106824(1,1)
// + 0x80106690(1). Then on sm[0x6b]: ==1 -> s3 (sm[0x48]=3, sm[0x68]=3) + TAIL_CF2C; ==2 -> s3
// (sm[0x48]=3, sm[0x68]=2) + TAIL_CF2C; else stay -> TAIL_REND.
static void ov_demo_s6(Core* c) {
  demo_log("s6", c);
  rec_dispatch(c, 0x8007b45cu);             // page sub-machine (SYNC)
  uint32_t sm = c->mem_r32(SM_PTR);
  if (c->mem_r16(sm + 0x50) == 3) {
    c->r[4] = 1; c->r[5] = 1; rec_dispatch(c, 0x80106824u);   // commit (a0=1,a1=1)
    c->r[4] = 1;             rec_dispatch(c, 0x80106690u);    // commit2 (a0=1)
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
// They use the coro-redirect-INTO-the-yielder handshake (ov_game_s4c shape, engine_stage.cpp):
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
static void ov_demo_s0(Core* c) {
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

// s4 / s5 / s7 — NOT owned at the substate-ENTRY level (deliberately). Unlike s0, their substantive
// transition logic lives entirely AFTER their deep yield, so an entry-level override could only set up
// args + coro-redirect straight into the guest yielder — a pure passthrough that OWNS NOTHING while
// adding override-table indirection + task-0-death risk if a redirect target/ra is wrong. That is
// scaffolding, not ownership, so we don't do it. The CORRECT ownership is a POST-YIELD override:
//   - s4 0x80106580: let the guest run `jal 0x8007bf20(0,0)` (it sets ra=0x8010658C), then own the
//     sm[0x6b] branch at 0x8010658C natively (v1==1 -> sm[0x48]=2,sm[0x68]=1; ==2 -> sm[0x48]=2,
//     sm[0x68]=0; ==7 -> sm[0x48]=5,*0x1f800134=1; else stay) and coro-redirect to TAIL_CF2C/TAIL_REND.
//   - s7 0x80106668 -> phase machine 0x80106C24: own the phase SELECTION (sm[0x4a]) prologue + phase2's
//     all-SYNC teardown (0x80106dfc: jal 0x80074bc4 ; *0x1f80019a=0 ; sm[0x48]=0) natively; phase0/1
//     still yield mid-phase, so redirect those.
//   - s5 0x801065DC: its ENTIRE body is `jal 0x80052078(2)` (LEAVE DEMO / task-restart) + the tail yield
//     — there is no engine logic to own; it stays guest (owning it would be the same empty passthrough).
// Tracked as the next frontier step in docs/port-progress.md. s0 (above) is owned because it has genuine
// pre-yield engine state to own (the init field writes + substate selection).

// Register the DEMO substate overrides when the just-loaded overlay is the DEMO overlay at the stage
// base. Distinguish from GAME.BIN (which aliases the same base) by the root-dispatcher prologue:
// DEMO 0x801062E4 = `addiu sp,sp,-48` (0x27bdffd0); GAME's entry is 0x8010637C = 0x27bdffe0 (and at
// DEMO's 0x8010637C sits a `sh zero,0x48(v1)` 0xa4600048, so GAME's stage_scan signature won't match
// DEMO either). AUTO so the overrides flush when the overlay unloads and the base is reused.
void demo_scan_overlay(Core* c, uint32_t base, uint32_t size) {
  if (base != 0x80106228u) return;
  if (c->mem_r32(0x801062E4u) != 0x27bdffd0u) return;   // DEMO root dispatcher prologue: addiu sp,-48
  // NB: owning the ROOT dispatcher prologue (ov_demo_root) was attempted (later-182b) and REVERTED:
  // its one-time setup call 0x800810f0 builds the PSX draw-environment GP0 packets via an INDIRECT
  // libgpu call (jalr v0[8] off the double-buffer struct *0x800a5998), and running 0x800810f0 as a
  // nested rec_dispatch from the override is NOT equivalent to running it in-context — it left a 5-byte
  // divergence in the env-packet pools (0x800A59xx/0x800EA0xx, GP0 cmd byte 0xE0 vs 0x00) while the
  // substate sub-fns dispatched 0-diff. The prologue is low-value PSX disp-env plumbing (the boundary
  // says the engine shouldn't reproduce it); owning it cleanly would need to run 0x800810f0 in-context,
  // which conflicts with its two-sync-call structure. Left to the guest. See docs/journal.md later-182b.
  rec_set_interp_override_auto(0x801063C0u, ov_demo_s0);   // run-once INIT (owns field writes + selection)
  rec_set_interp_override_auto(0x8010641Cu, ov_demo_s1);
  rec_set_interp_override_auto(0x80106464u, ov_demo_s2);
  rec_set_interp_override_auto(0x801064E8u, ov_demo_s3);
  rec_set_interp_override_auto(0x801065ECu, ov_demo_s6);
  if (cfg_dbg("demo"))
    fprintf(stderr, "[demo] own DEMO substates s0/s1/s2/s3/s6 (0x801063C0/0x8010641C/0x80106464/"
                    "0x801064E8/0x801065EC) in load 0x%08X+0x%X\n", base, size);
}
