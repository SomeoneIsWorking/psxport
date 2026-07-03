// engine_stage.cpp — PC-native ownership of the GAME stage state machine (the per-area scene/update
// driver), the engine's top-level "run the game" sequencer. Boundary: the ENGINE owns the scene-state
// machine (which state runs, what fields reset on each transition); the actual per-state system work
// (asset load, fade, render, gameplay sub-machines) stays dispatched to the retained PSX content/system
// code. Realizes "the engine runs the game; PSX only drives content" for the stage driver — incrementally.
//
// The GAME stage is overlay \BIN\GAME.BIN (LBA 1882), loaded RAW to base 0x80106228; task-0 entry =
// 0x8010637C, which runs as a COOPERATIVE TASK (it loops forever, yielding once per frame via
// FUN_80051f80). The current task object ptr is *0x1f800138 (== task-0 obj 0x801fe000); its state fields:
//   sm[0x48] top state, sm[0x4a] running sub-mode, sm[0x4c] area machine, sm[0x5c] intro timer.
// Three-level nested machine (all handlers in GAME.BIN; full RE in docs/engine_re.md):
//   sm[0x48]: 0 -> 0x801086e0 (area INIT)   1 -> 0x80108720 (RESUME-INIT)   2 -> 0x80108784 (RUNNING)
//   sm[0x4a] (in 0x80108784, 6-way @0x8010631c): 0..5 -> sub-handler (mode 2 = the 9-state sm[0x4c] area
//            load/intro/play machine at 0x80106478, jump table @0x8010622c)
//
// OWNED HERE: all three sm[0x48] handlers. The area-INIT pair (==0 / ==1) are clean `jr ra` functions
// whose callees are SYNCHRONOUS, so a native override that mirrors their guest writes and dispatches their
// resident setup fns is faithful (verified RAM 0-diff @ a field frame, later-168). The RUNNING dispatcher
// (==2, 0x80108784) owns the 6-way running-sub-mode (sm[0x4a]) selection, but its sub-handlers YIELD DEEP
// (they call resident 0x8007xxxx fns that wait across frames). It can NOT be rec_dispatch'd (that nests a
// rec_interp with its own CORO_SENTINEL; the deep yield's longjmp destroys that C frame and the resume
// mis-reads the return as task-end, killing task 0 — later-168). It uses the cooperative-yield handshake
// (rec_coro_redirect, later-169): native dispatch, then hand to the handler IN-CONTEXT so the yield is the
// scheduler returning, not a nested sentinel. Same mechanism unlocks owning FUN_80052078/FUN_800499e8.

#include "core.h"
#include "cfg.h"
#include "engine.h"                 // class Engine — GAME/STAGE driver + per-frame method impls below
#include "render.h"                 // class Render — c->mRender->frame() / frameX() (per-frame render driver)
#include "placement.h"              // ov_place_objects — native field object-placement driver (game/world)
#include "pool.h"                    // ov_pool_init_run — native object-pool init (game/world)
#include "c_subsys.h"                // disc_find_file — native ISO9660 resolver (native_task0_bootstrap/ov_start_bin_stage)
#include "asset.h"                   // class Asset — preloadTexgroup / preloadStage1 (native_stage0_sm)
#include "math/rng.h"                // class Rng — Slip #5 RNG cadence gate under SBS
#include <stdio.h>

// dispatch a still-recomp leaf with up to 3 args set (helpers for the native stage machines).
// later-238 BACKDROP ATTRIBUTION (PSXPORT_BDTAG): record each ov_field_frame call's pool-write span so the
// gp0 OT-walk classifier (gpu_native.cpp) can attribute a DEFERRED prim (e.g. the tp(576,256) sea backdrop)
// to the call that BUILT it — reliable where per-pass tags / WWATCH-pc / pool-node-addresses are not. The
// span table persists across the present (which classifies the prior frame's OT) because it is reset only at
// the TOP of the next ov_field_frame. `ffspan_lookup(addr)` returns the builder name (latest-span-wins).
// g_pkt_track/lo/hi retired 2026-07-02 — per-Core Render::mPktTrack/mPktLo/mPktHi (reached below).
#include "dualview_snapshot.h"    // c->mRender->dualviewSnapshot.capturePre/restorePre
// (g_render_psx + g_dualview both retired 2026-07-02 — reach as c->mRender->mode.psxRender() / dualview())
struct FFSpan { const char* name; uint32_t lo, hi; };
static FFSpan s_ffspan[40]; static int s_ffspan_n = 0; static int s_bdtag = -1;
static inline int bdtag_on() { if (s_bdtag < 0) s_bdtag = cfg_str("PSXPORT_BDTAG") ? 1 : 0; return s_bdtag; }
extern "C" const char* ffspan_lookup(uint32_t a) {
  // EARLIEST-first = INNERMOST-wins: an inner bracket ends (is recorded) before its outer, and outer spans
  // merge in their children, so the first containing span in record order is the tightest (real) builder.
  for (int i = 0; i < s_ffspan_n; i++) if (a >= s_ffspan[i].lo && a < s_ffspan[i].hi) return s_ffspan[i].name;
  return "(unattributed)";
}
// Coarse top-level phase bracketing (called from native_step_frame): reset at frame top, then begin/end
// around each frame phase to find WHICH phase builds the deferred backdrop packet (the gp0 classify runs
// later in the same frame's DrawOTag). Nestable via a small stack of PktSpan snapshots.
static PktSpan::Snapshot s_ff_stk[8]; static int s_ff_sp = 0;
extern "C" void ffspan_reset_frame(void) { if (bdtag_on()) { s_ffspan_n = 0; s_ff_sp = 0; } }
extern "C" void ffspan_begin(Core* c) {       // NESTABLE: save outer, open a fresh empty session
  if (!bdtag_on() || s_ff_sp >= 8) return;
  PktSpan& ps = c->mRender->pktSpan;
  s_ff_stk[s_ff_sp++] = ps.save();
  ps.open();
}
extern "C" void ffspan_end(Core* c, const char* nm) {
  if (!bdtag_on() || s_ff_sp <= 0) return;
  PktSpan& ps = c->mRender->pktSpan;
  uint32_t mlo, mhi;
  bool captured = ps.current(&mlo, &mhi);
  if (captured && s_ffspan_n < 40) { s_ffspan[s_ffspan_n].name = nm;
    s_ffspan[s_ffspan_n].lo = mlo; s_ffspan[s_ffspan_n].hi = mhi; s_ffspan_n++; }
  ps.restoreMerge(s_ff_stk[--s_ff_sp], captured ? mlo : 0xFFFFFFFFu, captured ? mhi : 0);
}
extern "C" void ffspan_dump(uint32_t a) {   // one-time: show the recorded spans vs an unattributed address
  static int done = 0; if (done) return; done = 1;
  fprintf(stderr, "[ffspan] addr %08x NOT in any of %d field-frame spans:\n", a, s_ffspan_n);
  for (int i = 0; i < s_ffspan_n; i++)
    fprintf(stderr, "[ffspan]   %-12s [%08x .. %08x)\n", s_ffspan[i].name, s_ffspan[i].lo, s_ffspan[i].hi);
}
// FFS: nested span tracker. c must be a Core* in scope. Same shape as ffspan_begin/end inlined.
#define FFS(nm, call) do { \
  if (bdtag_on()) { PktSpan& _ps = c->mRender->pktSpan; \
    PktSpan::Snapshot _outer = _ps.save(); _ps.open(); call; \
    uint32_t _mlo, _mhi; bool _captured = _ps.current(&_mlo, &_mhi); \
    if (_captured && s_ffspan_n < 40) { s_ffspan[s_ffspan_n].name = nm; \
      s_ffspan[s_ffspan_n].lo = _mlo; s_ffspan[s_ffspan_n].hi = _mhi; s_ffspan_n++; } \
    _ps.restoreMerge(_outer, _captured ? _mlo : 0xFFFFFFFFu, _captured ? _mhi : 0); } \
  else { call; } } while (0)

static inline void d0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static inline void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4]=a0; rec_dispatch(c, fn); }
static inline void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static inline void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn);
}

// sm[0x48] == 0 — area INIT: advance to running (sm[0x48]=2), reset the sub-machine state, run the per-area
// setup fns. (GAME.BIN 0x801086e0) Verified runtime-exercised + RAM 0-diff.
void Engine::s48_0() { Core* c = core;
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  c->mem_w16(sm + 0x48, 2);          // sm[0x48] = 2 (running)
  c->mem_w16(sm + 0x4a, 0);          // sm[0x4a] = 0
  c->mem_w16(sm + 0x4c, 0);          // sm[0x4c] = 0
  c->mem_w8 (sm + 0x69, 0);          // sm[0x69] = 0
  rec_dispatch(c, 0x8007a8e0u);      // per-area setup (resident system, synchronous)
  rec_dispatch(c, 0x8007b38cu);      // per-area setup (resident system, synchronous)
  c->r[29] = sp; c->r[31] = ra;      // mirror epilogue: addiu sp,+0x18; jr ra
}

// sm[0x48] == 1 — area RESUME-INIT (re-enter a running area, sub-mode 1): like init but sm[0x4a]=1 plus
// flag resets. (GAME.BIN 0x80108720) Faithful transcription of the disasm; the field-intro path used to
// verify (later-168) does not hit sm[0x48]==1, so this handler is not yet runtime-exercised — its callee
// FUN_8007b3f4 is synchronous like the init pair, so it is registered alongside ov_game_s48_0.
void Engine::s48_1() { Core* c = core;
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  c->mem_w16(sm + 0x48, 2);          // sm[0x48] = 2 (running)
  c->mem_w16(sm + 0x4a, 1);          // sm[0x4a] = 1
  c->mem_w16(sm + 0x4c, 0);          // sm[0x4c] = 0
  c->mem_w8 (sm + 0x69, 0);          // sm[0x69] = 0
  c->mem_w8 (0x1f8001ff, 0xff);      // DAT_1f8001ff = 0xff
  c->mem_w16(0x1f800278, 0);         // DAT_1f800278 = 0 (16-bit; delay-slot before the setup call)
  rec_dispatch(c, 0x8007b3f4u);      // per-area setup (resident system, synchronous)
  c->mem_w8 (0x1f800206, 0);         // display flags cleared after setup
  c->mem_w8 (0x1f800236, 0);
  c->mem_w8 (0x1f800234, 0);
  c->r[29] = sp; c->r[31] = ra;      // mirror epilogue: addiu sp,+0x18; jr ra
}

// sm[0x48] == 2 — RUNNING: dispatch the running sub-mode sm[0x4a] (0..5); >=6 is a no-op. (GAME.BIN
// 0x80108784) The engine OWNS this 6-way running-sub-mode selection. The selected sub-handler is retained
// PSX content/system code that can YIELD DEEP (it calls resident 0x8007xxxx fns that wait across frames),
// so it must NOT be rec_dispatch'd: that nests a rec_interp with its own CORO_SENTINEL, and the deep
// yield's longjmp destroys that C frame -> the resume mis-reads the return as task-end and kills task 0
// (later-168). Instead we use the cooperative-yield handshake (rec_coro_redirect, later-169): do the
// dispatch natively, then continue the flat interp at the handler IN-CONTEXT (same task run). A deep yield
// then longjmps to the scheduler and resumes correctly; the handler returns through the guest trampoline
// `j 0x8010881c` -> epilogue -> the task loop, exactly as the PSX path would.
//
// Faithful to 0x80108784: prologue `addiu sp,-0x18; sw ra,0x10(sp)`; read sm[0x4a]; if <6, set ra to the
// handler's guest return (the `jal handler`'s ra = jal_addr+8 = the trampoline's `j 0x8010881c`, byte-
// identical so the handler's saved-ra on its stack matches the reference) and redirect to the handler;
// else redirect to the guest epilogue 0x8010881c (which restores ra from 0x10(sp) and returns).
void Engine::s48_2() { Core* c = core;
  static const uint32_t handler[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  uint32_t ra = c->r[31];
  c->r[29] -= 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4a = c->mem_r16(sm + 0x4a);
  if (cfg_dbg("stage")) {
    static uint16_t last4a = 0xffff, last4c = 0xffff;
    uint16_t s4c = c->mem_r16(sm + 0x4c);
    if (s4a != last4a || s4c != last4c) {
      fprintf(stderr, "[stage] running: sm[0x4a]=%u sm[0x4c]=%u\n", s4a, s4c);
      last4a = s4a; last4c = s4c;
    }
  }
  if (s4a >= 6) { rec_coro_redirect(c, 0x8010881Cu); return; }   // out of range -> guest epilogue
  c->r[31] = 0x801087CCu + (uint32_t)s4a * 0x10u;       // handler's guest return (`j 0x8010881c` trampoline)
  rec_coro_redirect(c, handler[s4a]);                  // run the handler IN-CONTEXT (survives a deep yield)
}

// sm[0x4c] == the AREA machine (the 9-state load/intro/play scene state machine), GAME.BIN 0x80106478,
// reached as ov_game_s48_2's sub-mode 2 (the area LOAD/TRANSITION path). STAGED, NOT REGISTERED: it owns the
// 9-way sm[0x4c] state selection via the same coro-redirect handshake (the per-state bodies yield deep and
// run IN-CONTEXT), but the headless idle-field gate never reaches s4a==2, so it can't be A/B-verified yet —
// see the registration site. Faithful to 0x80106478:
//   prologue: addiu sp,-0x18; sw ra,0x14(sp); sw s0,0x10(sp)   (s0 saved in the jal's delay slot)
//   jal 0x80075a80  — a per-frame area update BEFORE the dispatch. It is SYNCHRONOUS (a transitive
//     jal-graph scan over its 57 reachable fns finds no yield FUN_80051f80; and the RAM 0-diff gate would
//     catastrophically collapse — task 0 dies — if it ever yield-crossed this nested rec_dispatch), so it
//     is dispatched synchronously here.
//   read sm[0x4c]; if <9 jump-table @0x8010622c -> state; else fall to the shared epilogue.
//   each state runs then `j 0x80106a14` (shared epilogue: lw ra,0x14(sp); lw s0,0x10(sp); jr ra).
// The states are reached by `jr v0` (computed), so they run with ra UNCHANGED (= this fn's caller ra, which
// rec_dispatch restores) and return only via `j 0x80106a14` — so we leave r[31] alone and just redirect.
void Engine::s4c() { Core* c = core;
  static const uint32_t state[9] = {
    0x801064c4u, 0x80106510u, 0x80106580u, 0x801065b8u, 0x801066b8u,
    0x80106830u, 0x80106930u, 0x8010694cu, 0x801069b4u,
  };
  uint32_t ra = c->r[31];
  if (cfg_dbg("stage")) {
    uint32_t sm0 = c->mem_r32(0x1f800138);
    fprintf(stderr, "[stage] ov_game_s4c ENTER sm[0x4c]=%u (caller ra=0x%08X)\n",
            c->mem_r16(sm0 + 0x4c), ra);
  }
  c->r[29] -= 0x18;
  c->mem_w32(c->r[29] + 0x14, ra);              // sw ra,0x14(sp)
  c->mem_w32(c->r[29] + 0x10, c->r[16]);        // sw s0,0x10(sp)
  c->engine.areaUpdateTail();                  // 0x80075a80 NATIVE (synchronous — verified yield-free)
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c >= 9) { rec_coro_redirect(c, 0x80106a14u); return; }   // out of range -> shared epilogue
  rec_coro_redirect(c, state[s4c]);             // run the state IN-CONTEXT (it `j`s to the epilogue itself)
}

// ---- NATIVE PER-FRAME GAME LOOP (game_native path, mirrors DEMO demo_native) ------------------------
// The GAME stage is owned as a native per-frame dispatcher instead of coro-redirecting into the guest
// loop 0x801063F4: each frame ov_game_frame runs ONE loop iteration natively (dispatch the sm[0x48]
// handler + bump the frame counter), and "yield" = return. This re-wires the previously-orphaned native
// handlers and descends ownership into gameplay (the SOP field-mode machine). Prereq landed (later-217b):
// the SOP area load is native+synchronous, so SOP state-0 never yields.

// (ov_sop_field_mode moved to Sop::fieldMode — c->engine.sop.fieldMode())
#include "sop.h"                              // class Sop — transitionAreaLoad (sync FIELD transition load)
#include "game.h"                             // class Game — Game::sbs (Slip #3 faithful/PC-mode gate)
#include "render/screen_fade/screen_fade.h"   // class ScreenFade — the single fade driver
#include "camera/cutscene_camera.h"           // class CutsceneCamera — resident driver 0x8006EC44 (native)
void ov_game_func_801084F8(Core*);                          // generated/ov_game_disp.c — still-recomp: draw pause
                                                             // menu + cursor/page-transition handling

// FUN_8010810C page-1 dim-fade branch (task+0x6B == 1, "draw main pause menu" — see game/ui/menu.cpp's RE
// of the same dispatcher). Disasm (generated/ov_game_shard_0.c:1312-1320, label L_8010829C): while the
// pause-menu page-1 handler is selected, EVERY frame it runs an UNCONDITIONAL, NON-RAMPING flat-gray dim
// (FUN_8007E9C8(0x00808080, a1=0, weight=4) -> engine_fade_set) then falls through to FUN_801084F8 (menu
// draw + cursor/page nav, still recomp). This is NOT the reference per-node fade SM (no ramp counter, no
// node state) — own JUST this page's shape here; the other 11 pages + the dispatcher's bounds-check/table
// jump stay recomp via d0(c, 0x8010810cu) (own-caller-before-callee: the caller (ov_field_frame et al.) is
// already native, but the callee's other pages are unexplored, so full transcription is out of scope).
void Engine::submitPage810c() { Core* c = core;
  uint32_t task = c->mem_r32(0x1F800138u);
  if (task && c->mem_r8(task + 0x6Bu) == 1) {
    c->screenFade.set(ScreenFade::SUBTRACTIVE, 0x80, 0x80, 0x80);   // pause-menu dim: flat gray, held each frame page-1 handler runs
    ov_game_func_801084F8(c);              // still recomp: menu draw + cursor/page transitions
    return;
  }
  d0(c, 0x8010810cu);
}
// (ov_objwalk moved to ObjectList::walkAll — c->engine.objectList.walkAll())
// (ov_disp_26c88 moved to ObjectTable::dispatch — c->engine.objectTable.dispatch())
// (ov_list_walk_69b28 moved to ObjectList::walkAux — c->engine.objectList.walkAux())
// (ov_arr8_dispatch_26368 moved to Array8Dispatch::tick — c->engine.array8Dispatch.tick())
// (submode0 / submode1 are now Engine methods — Engine::submode0() / Engine::submode1())
// (Engine::fieldTransition + workers are defined in this TU; forward declared via engine.h.)

// sm[0x48]==2 RUNNING, per-frame variant: dispatch sm[0x4a] handler. handler[0] = the GAME->SOP bridge
// 0x8010882c (owned native, ov_game_submode0); the others stay rec_dispatch leaves (synchronous; a
// not-yet-sync leaf that yields is contained by the scheduler setjmp = frame-done).
void Engine::s48_2_frame() { Core* c = core;
  static const uint32_t handler[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4a = c->mem_r16(sm + 0x4a);
  if (s4a >= 6) return;
  if (s4a == 0) { ffspan_begin(c); c->engine.submode0(); ffspan_end(c, "submode0"); return; }
  if (s4a == 1) { ffspan_begin(c); c->engine.submode1(); ffspan_end(c, "submode1"); return; }
  if (s4a == 5) { ffspan_begin(c); c->engine.fieldTransition(); ffspan_end(c, "transition"); return; }  // native FUN_80108a60
  ffspan_begin(c); rec_dispatch(c, handler[s4a]); ffspan_end(c, "s48_2_handler");
}

// GAME sub-mode-0 bridge 0x8010882c (sm[0x4c]/sm[0x4e] dispatch) — native. Faithful to the disasm:
// sm[0x4c]==0 & sm[0x4e]==0 -> input-reset 0x8005082c (sync leaf) + sm[0x50]=0, sm[0x4e]=1; sm[0x4e]==1
// -> run the native SOP field-mode machine; sm[0x4c]==1 -> sm[0x4c]=0, sm[0x4a]++.
void Engine::submode0() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c == 0) {
    uint16_t s4e = c->mem_r16(sm + 0x4e);
    if (s4e == 0) {
      c->r[4] = 0; c->r[5] = 0; c->r[6] = 0;
      rec_dispatch(c, 0x8005082cu);          // input reset (leaf, no yield)
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x50, 0);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
    } else if (s4e == 1) {
      // 0x80109450 is the loaded MODE overlay's field-mode fn. Our native machine is SOP-specific, so
      // only use it when SOP is actually loaded (signature = its first insn `lui v0,0x1f80` = 0x3C021F80);
      // for any other mode/field overlay, dispatch the guest fn (until that overlay is owned natively too).
      if (c->mem_r32(0x80109450u) == 0x3C021F80u) c->engine.sop.fieldMode();   // native SOP
      else rec_dispatch(c, 0x80109450u);                                   // other overlay -> guest
    }
  } else if (s4c == 1) {
    uint16_t s4a = c->mem_r16(sm + 0x4a);
    c->mem_w16(sm + 0x4c, 0);
    c->mem_w16(sm + 0x4a, (uint16_t)(s4a + 1));
  }
}

// GAME sm[0x4a]==1 handler 0x801088d8 — the FIELD area machine (the actual walkable field, loaded
// AFTER the SOP intro). Native: a jump-table switch on sm[0x4c] (table @0x80106334, 7 states). Faithful
// to the disasm:
//   state 0 (0x80108918): FUN_8005245c() (sound/CD setup, sync leaf), then the area-DATA load
//     FUN_80044bd4(0x800452c0, *0x800bf870, 0, 2) — the COOPERATIVE spawn-and-wait. We own it INLINE
//     and SYNCHRONOUS via native_transition_area_load (no task spawn, no yield) — the prereq for the
//     per-frame model (re-entering a yielding state-0 would re-spawn the load forever).
//   state 1 (0x8010893c): 0x1f800234=0; sm[0x4c] = *(u8*)(0x80108f60 + *0x800bf870)  (next-state table)
//   states 2..6 (0x8010896c/7c/8c/9c/ac): jal the field RUNNING sub-machine handler:
//     2->0x80106b98  3->0x801070b4  4->0x80107230  5->0x8010766c  6->0x80107790
//     These are YIELD-FREE (transitive jal-graph scan: no FUN_80051f80 once CD/audio-busy are sync
//     leaves), so they rec_dispatch synchronously per-frame and return = the frame's gameplay work.
//   sm[0x4c] >= 7 -> no-op (falls to the epilogue).

// Native FUN_80025588 — the field EVENT/COMMAND-QUEUE state machine (struct @0x800ed058). A 3-state top
// switch on base[2]: state 0 ARMS it (base[2]=1, base[9]=0, snapshot list head 0x800ecf58 into base[0x3c],
// clear 0x800bfa5c, run setup 0x80024e00) then FALLS THROUGH into the active body; state 1 is the active
// body; state >=2 is a no-op. Active body drains a small FIFO when base[0x14]==0 && base[0x15]!=0: entry 0
// is dispatched via 0x80040aa4(id,kind) then 0x80074bf8(kind==0 ? 2 : 3, only for kind 0/1); the two
// parallel byte arrays base[0x16] (id) / base[0x1c] (kind) shift down one; base[0x15]--, base[0x14]++.
// Then 0x80024f18 always, and per the GAME phase 0x800bf870 either 0x800251f0 (default) or a light-toggle
// of base[8] (phase 2/7, gated on 0x800bf816==0 && (0x800e7e68 & 0x0c00)); phases 3/20 do neither. Always
// ends with 0x80077b5c. All leaf callees stay substrate; only the control flow is native. Faithful to the
// recomp body; a direct child of ov_field_frame (was `d0(c, 0x80025588)`).
void Engine::sceneEventFifo() { Core* c = core;
  const uint32_t B = 0x800ed058u;
  uint8_t st = c->mem_r8(B + 2);
  if (st == 0) {
    c->mem_w8(B + 2, 1);
    c->mem_w8(B + 9, 0);
    uint32_t head = c->mem_r32(0x800ecf58u);
    c->mem_w8(0x800bfa5cu, 0);
    c->mem_w32(B + 0x3c, head);
    d1(c, 0x80024e00u, B);
    // fall through into the active body
  } else if (st != 1) {
    return;                      // st >= 2: guest jumps straight to the epilogue
  }
  if (c->mem_r8(B + 0x14) == 0 && c->mem_r8(B + 0x15) != 0) {
    d2(c, 0x80040aa4u, c->mem_r8(B + 0x16), c->mem_r8(B + 0x1c));
    uint8_t kind = c->mem_r8(B + 0x1c);
    if (kind == 0)      d1(c, 0x80074bf8u, 2);
    else if (kind == 1) d1(c, 0x80074bf8u, 3);
    int n = (int)c->mem_r8(B + 0x15) - 1;                   // shift the FIFO down by one (drop entry 0)
    for (int i = 0; i < n; i++) {
      c->mem_w8(B + 0x16 + i, c->mem_r8(B + 0x16 + i + 1));
      c->mem_w8(B + 0x1c + i, c->mem_r8(B + 0x1c + i + 1));
    }
    c->mem_w8(B + 0x15, (uint8_t)(c->mem_r8(B + 0x15) - 1));
    c->mem_w8(B + 0x14, (uint8_t)(c->mem_r8(B + 0x14) + 1));
  }
  d1(c, 0x80024f18u, B);
  uint8_t phase = c->mem_r8(0x800bf870u);
  if (phase == 3 || phase == 20) {
    // neither the light-toggle nor 0x800251f0
  } else if (phase == 2 || phase == 7) {
    if (c->mem_r8(0x800bf816u) == 0 && (c->mem_r16(0x800e7e68u) & 0x0c00) != 0)
      c->mem_w8(B + 8, (uint8_t)(1 - c->mem_r8(B + 8)));
  } else {
    d1(c, 0x800251f0u, B);
  }
  d1(c, 0x80077b5cu, B);
}

// Native FUN_8004FE84 — a 2-phase scene/render-list builder driver (struct @0x800bf548). base[0] is the
// phase: 0 -> ARM (snapshot list ptr 0x800ecf64 into base+0x2b0, +0x2b4 = ptr+0x10, +0x2b8 = ptr+0x10 +
// (*(u16)ptr << 1); base[1]=0, base[0]=1); 1 -> run sub-state base[1] (0->0x8004f430, 1->0x8004f474,
// 2->0x8004f514, 3->0x8004f6d0, >=4 none). After the sub-state, set bit0 of flag byte @0x800bf822 when
// (base[1]!=0 || base[0x0a]!=0) else clear it. phase>=2 is a no-op. Leaf callees stay substrate. Faithful
// to the recomp body; a direct child of ov_field_frame (was `d0(c, 0x8004fe84)`).
void Engine::sceneRenderListBuilder() { Core* c = core;
  const uint32_t B = 0x800bf548u;
  uint8_t phase = c->mem_r8(B + 0);
  if (phase == 0) {
    uint32_t p = c->mem_r32(0x800ecf64u);
    c->mem_w32(B + 0x2b0, p);
    c->mem_w32(B + 0x2b4, p + 0x10);
    uint16_t h = c->mem_r16(p);
    c->mem_w8(B + 1, 0);
    c->mem_w8(B + 0, 1);
    c->mem_w32(B + 0x2b8, (p + 0x10) + ((uint32_t)h << 1));
    return;
  }
  if (phase != 1) return;         // phase >= 2: epilogue only
  switch (c->mem_r8(B + 1)) {
    case 0: d1(c, 0x8004f430u, B); break;
    case 1: d1(c, 0x8004f474u, B); break;
    case 2: d1(c, 0x8004f514u, B); break;
    case 3: d1(c, 0x8004f6d0u, B); break;
    default: break;               // sub >= 4: no sub-handler (guest j 8004ff60)
  }
  uint32_t flag = 0x800bf822u;
  uint8_t v = c->mem_r8(flag);
  if (c->mem_r8(B + 1) != 0 || c->mem_r16s(B + 0x0a) != 0)
    c->mem_w8(flag, (uint8_t)(v | 1));
  else
    c->mem_w8(flag, (uint8_t)(v & 0xfe));
}

// FIELD PER-FRAME UPDATE 0x80108b0c — native control flow (the field frame's work driver, called by
// the running states of the sm[0x4e] machine). Faithful to the disasm: bump *0x1f80017c and *0x800bf878;
// if NOT paused (*0x1f800136==0) run the 11-call gameplay-update block; if *0x1f800136 < 2 run 0x8003f9a8;
// then always the render-submit 0x8010810c + 0x80077d8c + per-frame area update 0x80075a80. Yield-free
// (transitive jal scan, 1021 fns). The object-walk 0x8007a904 and display 0x80026c88 now run as the
// NATIVE ov_objwalk / ov_disp_26c88 (direct C calls — the previously-orphan bodies wired into the live
// field frame); the remaining callees stay rec_dispatch leaves until owned in turn.
void Engine::fieldFrame() { Core* c = core;
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: full gameplay update
    FFS("ff_59d28", c->engine.frameStartTick()); FFS("ff_69b28", c->engine.objectList.walkAux());    // 0x80059d28/0x80069b28 NATIVE
    FFS("ff_26368", c->engine.array8Dispatch.tick()); FFS("ff_objwalk", c->engine.objectList.walkAll());     // 0x80026368/0x8007a904 NATIVE
    FFS("ff_25588", c->engine.sceneEventFifo()); FFS("ff_4fe84", c->engine.sceneRenderListBuilder());   // 0x80025588/0x8004fe84 NATIVE (Engine methods)
    FFS("ff_disp26c88", c->engine.objectTable.dispatch());                                            // 0x80026c88 NATIVE
    FFS("ff_22a80", c->engine.modePerFrameDispatch());                                // 0x80022a80 NATIVE (Engine::modePerFrameDispatch)
    FFS("ff_6ec44", CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update());         // 0x8006ec44 NATIVE (CutsceneCamera::update)
    FFS("ff_50de4", c->engine.sceneStateStep());                 // 0x80050de4 NATIVE (Engine::sceneStateStep)
    FFS("ff_1cac0", c->engine.areaModeDispatch());               // 0x8001cac0 NATIVE (Engine::areaModeDispatch)
  }
  // DUAL-VIEW: snapshot the post-gameplay / pre-render state so the side-by-side PSX render pass can run
  // from it (the native render below consumes per-frame queues, so it is not re-runnable). No-op unless on.
  c->mRender->dualviewSnapshot.capturePre(c);
  if (c->mem_r8(0x1f800136u) < 2) c->mRender->frame();   // 0x8003f9a8 — NATIVE render orchestrator + walk
  // RENDER GUEST-MEMORY DECOUPLING (user 2026-06-24: the native renderer must leave NO guest-memory side
  // effect — only native GAMEPLAY may write guest memory). The native render above scribbles guest
  // scratchpad/OT (e.g. the RotMatrix SVECTOR at 0x1F8000xx) as PSX-GTE transform workspace, which
  // still-recomp content reads back wrong (the regression the DUALCORE harness pinned to scratchpad
  // 0x1F8000C0). dualviewSnapshot.restorePre rewinds the post-gameplay guest state captured by
  // capturePre above — undoing EVERY guest write the native render made.
  //
  // ORDER (2026-07-03, later-292 finding — docs/findings/sbs.md 0x800BF81E RENDER-mode divergence):
  // restorePre runs RIGHT AFTER Render::frame(), BEFORE submitPage810c(). submitPage810c is the game's
  // frame-submit leaf (not native render orchestration) and writes gameplay-relevant state (e.g. the
  // 0x800BF81E clear read by later gameplay via `param_1[0x147]=DAT_800bf81f>>4`). Putting restore
  // AFTER submitPage810c (the earlier order) rewound that legit clear on the native-render side and
  // dropped it, while the PSX-render side kept it — exactly the byte the RENDER-mode SBS surfaced. Now
  // only the RENDER path's side effects are undone; submitPage810c writes stick on both cores.
  //
  // later-284: we USED to also re-run the PSX render (d0(0x8003f9a8)+d0(0x8010810c)) here "to leave guest
  // OT/packets PSX-correct." That was REDUNDANT and HARMFUL: restorePre already yields the exact
  // post-gameplay guest state, and NOTHING consumes the PSX-built OT/packets (the native display,
  // ov_scene_native/ov_draw_otag, re-derives its transforms from node/entity data, not leftover OT). Proven:
  // with the re-render removed, oraclediff stayed convergent native-vs-oracle through the whole opening incl.
  // free-roam onset (only the benign baseline bytes differ). The re-render was ALSO the intro-cutscene FREEZE
  // + red-diagonal corruption: 0x8003f9a8 recurses deep (A00 object render chain) on task0's ~2KB guest stack
  // and overflowed into the task table, clobbering sm[0x48]=17 -> ov_game_frame ret 0 -> game_coop yield-loop
  // freeze (+ the game_coop r29 SP leak that grew the red corruption). Removing it fixed all of it at once.
  // The TRUE end-state (make ov_render_frame write ZERO guest memory so capturePre/restorePre can go too) is a
  // separate perf/architecture follow-up; the rewind correctly enforces the invariant meanwhile.
  if (!c->mRender->mode.psxRender() && !c->mRender->mode.dualview() && c->mem_r8(0x1f800136u) < 2) {
    c->mRender->dualviewSnapshot.restorePre(c);
  }
  FFS("ff_submit810c", c->engine.submitPage810c()); // render submit (page-1 dim-fade owned; other pages recomp)
  FFS("ff_77d8c", c->engine.postRenderTick());   // 0x80077d8c NATIVE (Engine::postRenderTick)
  FFS("ff_area75a80", c->engine.areaUpdateTail());   // 0x80075a80 NATIVE (Engine::areaUpdateTail)
}

// ov_scene_fade_seq — GAME-overlay a0l screen-fade sequencer (guest FUN_8010957C / ov_a0l_gen_8010957C,
// generated/ov_a0l_shard_1.c:146-310), reached from ov_field_run's sm[0x4e]==0xb via the FIXED global node
// 0x800E8008 (not a walked object). This is a DISTINCT state machine from the a06 door/sub-scene fade
// (ov_transition_main above) — it only shares the low-level screen-fade primitive engine_fade_set
// (FUN_8007E9C8), not the SM shape: an outer 0/1 gate (node+2) wrapping a bounds-checked 6-step jump table
// on an inner index (node+3), a ~20-tick delay counter (node+104), and a fade "level" ramp (node+106,
// re-seeded to 31 on entry/reset).
//
// OPEN ISSUE (not yet resolved — flag before trusting this live): FUN_8007E9C8's real signature is
// (color, a1, a2); every OTHER known call site (ov_transition_main, door fades) always passed a2==4, which
// engine_fade_set's 2-arg (color, a1) signature already assumed away. THIS function's guest calls use
// a2==0 (state-0 init) and a2==1 (every ramp step) instead — a distinct pattern engine_fade_set has no
// parameter for. What guest a2 actually controls is unresolved; wiring onto engine_fade_set as-is silently
// drops that distinction, so the fade blend mode here may not match the PSX reference until that's dug up.
//
// Callees NOT decoded (left as rec_dispatch leaves, contiguity-correct — port only after this leaf is
// verified live): FUN_8005082C(0,0,0), FUN_8001D71C(11) [state-0 init, purpose unknown]; ov_a0l_func_8010D030
// (a0=node) [generic per-node init helper]; ov_a0l_func_8010CC68(a0) [polled "sub-step done" helper,
// return gates steps 0/3 by return value, steps 1/4 by fade-level reaching 0 instead].
//
// 0x800BF839/0x800BF83A are the SAME globals ov_field_run (below) already reads to drive its own state
// (case 1 checks 0x800bf839==3, case 8 rewrites both) — this function and ov_field_run communicate through
// guest memory, not a call, which is fine (guest-memory-direct is an accepted PC-native pattern here) but
// worth knowing at both ends.
void Engine::fadeSequencer(uint32_t node) { Core* c = core;
  uint8_t outer = c->mem_r8(node + 2);

  if (outer == 0) {
    d3(c, 0x8005082cu, 0, 0, 0);             // FUN_8005082C(0,0,0) -- not yet decoded
    d1(c, 0x8001d71cu, 11);                  // FUN_8001D71C(11)    -- not yet decoded
    c->mem_w8(0x800bfa55u, 0);
    c->mem_w8(node + 3, 0);
    c->mem_w8(node + 2, (uint8_t)(outer + 1));   // -> outer state 1
    d1(c, 0x8010d030u, node);                // ov_a0l_func_8010D030(node) -- not yet decoded
    c->mem_w16(node + 106, 31);
    c->screenFade.applyLeafCall(0x00ffffffu, 0);   // = guest FUN_8007e9c8(0xffffff, 0, ?): full black; NOTE guest a2 was 0 (see OPEN ISSUE)
    return;
  }

  if (outer != 1) return;   // any other outer value: permanent no-op (matches reference: no default handler)

  uint8_t step = c->mem_r8(node + 3);
  if (step >= 6) return;    // bounds check -- once step reaches 6 this function is inert forever

  auto ramp_level = [&](int32_t sign) -> uint32_t {
    // v = (level << 3) [negated if sign<0] & 0xFF, replicated into R/G/B (same "gray" packing shape as the
    // a06 reference's state-2, but here the source is level<<3, not the raw level byte).
    int16_t level = c->mem_r16s(node + 106);
    uint32_t v = (uint32_t)((sign < 0) ? -(level << 3) : (level << 3)) & 0xffu;
    return (v << 16) | (v << 8) | v;
  };
  auto decrement_level_clamped = [&]() {
    int16_t level = c->mem_r16s(node + 106);
    if (level != 0) c->mem_w16(node + 106, (uint16_t)(level - 1));
  };
  auto advance_step = [&]() {
    uint8_t s = c->mem_r8(node + 3);
    c->mem_w8(node + 3, (uint8_t)(s + 1));
  };

  switch (step) {
    case 0: {                                    // ramp UP, gated by helper return value
      c->screenFade.applyLeafCall(ramp_level(+1), 1);      // = guest FUN_8007e9c8(color, 1, ?) additive; NOTE guest a2 was 1 (see OPEN ISSUE)
      decrement_level_clamped();
      d1(c, 0x8010cc68u, 0);                          // ov_a0l_func_8010CC68(0) -> result in c->r[2]
      if (c->r[2] == 0) return;                       // not done yet
      c->mem_w16(node + 106, 31);
      advance_step();
      return;
    }
    case 1: {                                    // ramp DOWN, gated by fade LEVEL reaching 0
      c->screenFade.applyLeafCall(ramp_level(-1), 1);      // = guest FUN_8007e9c8(color, 1, ?) additive; NOTE guest a2 was 1 (see OPEN ISSUE)
      decrement_level_clamped();
      d1(c, 0x8010cc68u, 0);                          // result unused this branch
      if (c->mem_r16s(node + 106) != 0) return;
      advance_step();
      c->mem_w16(node + 104, 20);                     // arm the step-2 delay counter
      return;
    }
    case 2: {                                    // plain ~20-tick delay, then reset level + advance
      uint16_t d = (uint16_t)(c->mem_r16(node + 104) - 1);
      c->mem_w16(node + 104, d);
      if (d != 0) return;
      c->mem_w16(node + 106, 31);
      advance_step();
      return;
    }
    case 3: {                                    // same as case 0 but helper called with a0=1
      c->screenFade.applyLeafCall(ramp_level(+1), 1);
      decrement_level_clamped();
      d1(c, 0x8010cc68u, 1);
      if (c->r[2] == 0) return;
      c->mem_w16(node + 106, 31);
      advance_step();
      return;
    }
    case 4: {                                    // same as case 1 but helper called with a0=1;
                                                  // on completion does NOT reset the level to 31
      c->screenFade.applyLeafCall(ramp_level(-1), 1);
      decrement_level_clamped();
      d1(c, 0x8010cc68u, 1);
      if (c->mem_r16s(node + 106) != 0) return;
      advance_step();
      return;
    }
    case 5: {                                    // completion tail: poke the shared "sm" struct + the
                                                  // 0x800BF839/0x800BF83A control globals, advance
      uint32_t sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 74, 1);
      c->mem_w16(sm + 76, 2);
      c->mem_w16(sm + 78, 6);
      c->mem_w8(0x800bf839u, 3);
      c->mem_w16(0x800bf83au, 0x1501);
      advance_step();
      return;
    }
  }
}

// FIELD RUNNING sub-machine 0x80106b98 — native control flow + state bodies (decomp:
// scratch/decomp/game/80106b98.c). A 12-way switch on sm[0x4e]; the running states call the native
// ov_field_frame (0x80108b0c) and the heavy leaf callees rec_dispatch. NB the guest fall-throughs are
// faithful: case 2 -> 3, case 4 -> 1 (no break). sm[0x4e] >= 12 = no-op. This anchors the field frame
// natively; the leaf callees (object-placement FUN_80072a78 etc.) are the next descent.
void Engine::fieldRun() { Core* c = core;
  uint32_t sm  = c->mem_r32(0x1f800138u);
  uint16_t s4e = c->mem_r16(sm + 0x4e);
  switch (s4e) {
    case 0:
      c->engine.pool.init();   // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x8007b18c)
      c->engine.pool.resetControlBlock();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800796dc)
      c->engine.pool.seedAreaObjects();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800263e8)
      c->engine.placement.placeAreaObjects();   // OWNED native (game/world/placement.cpp) — replaces rec_dispatch(0x80072a78)
      c->engine.pool.reset75240();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x80075240)
      c->engine.pool.setupViewScroll();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800783dc)
      c->engine.pool.finalViewInit();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x80078610)
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      c->mem_w8(sm + 0x6b, 0);
      if (c->mem_r8(0x800bf89cu) == 2) { c->mem_w16(sm + 0x4e, 9); }
      else if (c->mem_r8(0x800bf870u) == 8) { d0(c, 0x80114b90u); }
      else if (c->mem_r32(0x800bf870u) == 0x15) { c->mem_w16(sm + 0x4e, 0xb); return; }
      c->engine.pool.selectStateIndex(c->mem_r8(0x800bf870u));   // OWNED native — replaces d1(0x80074f24, area)
      break;
    case 2:
      d2(c, 0x80058304u, 0x800e7e80u, 0xc);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      /* fallthrough */
    case 3:
      d0(c, 0x80074bc4u);
      if (c->mem_r8(0x800bf870u) == 8) d0(c, 0x80114b90u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 2);
      c->mem_w16(sm + 0x4c, 0);
      c->mem_w16(sm + 0x4e, 0);
      d3(c, 0x8005082cu, 0, 0, 0);
      break;
    case 4:
      d0(c, 0x8006c7c4u); d0(c, 0x800508a8u);
      c->mem_w16(c->mem_r32(0x1f800138u) + 0x4e, 1);
      /* fallthrough */
    case 1: {
      ffspan_begin(c); c->engine.fieldFrame(); ffspan_end(c, "fieldframe");   // native field per-frame update (0x80108b0c)
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf80du) == 3) {        // (signed byte) special mode 3
        if (c->mem_r8(0x800bf80fu) == 0) {
          d0(c, 0x80074bc4u);
          sm = c->mem_r32(0x1f800138u);
          if (c->mem_r32(0x800e7feeu) == 0) {   // _DAT_800e7fee (read as int per decomp)
            c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));    // LAB_80106f48
          } else {
            c->mem_w8(0x800bf880u, 1);
            c->mem_w32(0x1f800194u, c->mem_r32(0x800e7feeu));
            c->mem_w16(sm + 0x4e, 0);
          }
        }
      } else if (c->mem_r8(0x800bf839u) != 0 && c->mem_r8(0x800bf80fu) == 0) {
        if (c->mem_r8(0x800bf839u) == 8) {
          c->mem_w16(sm + 0x4a, 3); c->mem_w16(sm + 0x4c, 0); c->mem_w16(sm + 0x4e, 0);
        } else {
          if (c->mem_r8(0x1f800236u) > 4) d1(c, 0x80050894u, 0);    // LAB_80106fac join
          sm = c->mem_r32(0x1f800138u);
          c->mem_w16(sm + 0x4a, 1); c->mem_w16(sm + 0x4c, 2); c->mem_w16(sm + 0x4e, 6);
        }
      }
      break;
    }
    case 5:
      if (c->mem_r8(0x800bf870u) == 7) { d0(c, 0x801128bcu); d0(c, 0x800508a8u); }
      c->mem_w16(c->mem_r32(0x1f800138u) + 0x4e, 1);
      c->engine.fieldFrame();
      break;
    case 6: {
      if (c->mem_r32(0x800e7feeu) != 0) { c->mem_w8(0x800bf880u, 1); c->mem_w32(0x1f800194u, c->mem_r32(0x800e7feeu)); }
      d0(c, 0x80074bc4u);
      // _DAT_800bf870 = CONCAT11(...) & 0x3f1f  — the decomp's byte-swap-and-mask of *0x800bf83a into bf870
      uint16_t b83a = c->mem_r16(0x800bf83au);
      uint32_t v = (((uint32_t)(b83a & 0xff) << 8) | (b83a >> 8)) & 0x3f1f;
      c->mem_w32(0x800bf870u, v);
      if (c->mem_r8(0x800bf839u) == 7) { d0(c, 0x80114b90u); c->mem_w8(0x800bf839u, 3); }
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf839u) == 3) {
        d0(c, 0x8005245cu);
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1); c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
      } else {
        c->mem_w16(sm + 0x48, 2);
        uint8_t b = c->mem_r8(0x1f800236u);
        c->mem_w16(sm + 0x4a, 5); c->mem_w16(sm + 0x4e, 0); c->mem_w16(sm + 0x4c, b);
      }
      break;
    }
    case 7: {
      d1(c, 0x80045580u, 1);
      if (c->r[2] == 0) return;
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));    // goto LAB_80106f48
      break;
    }
    case 8:
      if (c->mem_r8(0x1f80019bu) == 0) return;
      c->mem_w8(0x800bf89cu, 4);
      c->mem_w8(0x1f800236u, 0);
      c->mem_w8(0x800bf839u, 3);
      c->mem_w16(0x800bf83au, 0);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 1); c->mem_w16(sm + 0x4c, 2); c->mem_w16(sm + 0x4e, 6);   // LAB_80106fac
      break;
    case 9:
      c->engine.fieldFrame();
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf89cu) == 2 && (c->mem_r32(0x800e7e68u) & 8) != 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->mem_w8(0x800bf809u, 1);
        c->mem_w8(sm + 0x6e, 0x1f);
      }
      break;
    case 10: {
      c->engine.fieldFrame();
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6e) * (uint32_t)-8) & 0xff;
      c->screenFade.applyLeafCall((u << 16) | (u << 8) | u, 0);   // = guest FUN_8007e9c8(color, 0, 4): area-transition subtractive fade-out ramp
      uint8_t nv = (uint8_t)(c->mem_r8(sm + 0x6e) - 1);
      c->mem_w8(sm + 0x6e, nv);
      if (nv == 0) {
        c->mem_w16(sm + 0x4e, 7);
        d0(c, 0x8001cf2cu);
        // NOTE: no persistent hold_black on fade-out exit — see sop.cpp state 3 note (regression 2026-07-01).
      }
      break;
    }
    case 0xb:
      c->engine.fadeSequencer(0x800e8008u);   // OWNED native — replaces d1(0x8010957c, node) (a0l fade sequencer)
      break;
    default: break;
  }
}

// FIELD PER-FRAME UPDATE VARIANT 0x80108be4 — the mid-TRANSITION field frame, used by the state-3
// running sub-machine while the screen is faded for an area change. Lighter than ov_field_frame
// (0x80108b0c): a reduced gameplay-update set (the state-transition object update 0x8007b04c instead
// of the full Tomba+object walk 0x8007a904) then the SAME render-submit 0x8010810c. Faithful to disasm.
// Owned so the transition path is native+traceable (the door freeze lives below here): the heavy
// callees stay rec_dispatch leaves to descend into next — esp. 0x8007b04c (the per-object update that
// must, but currently does not, tick the screen-transition sequencer FUN_80026ad0 to completion).
void Engine::fieldFrameX() { Core* c = core;
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: reduced gameplay update
    c->engine.frameStartTick(); c->engine.objectList.walkAux(); d0(c, 0x80026368u); c->engine.transitionState3.walkOnce();   // 0x80059d28/0x80069b28/0x8007b04c NATIVE
    c->engine.sceneEventFifo(); c->engine.sceneRenderListBuilder(); c->engine.objectTable.dispatch(); c->engine.modePerFrameDispatch();   // 25588/4fe84/26c88/22a80 NATIVE
    CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update();   // 0x8006ec44 NATIVE (CutsceneCamera::update)
  }
  if (c->mem_r8(0x1f800136u) < 2) c->mRender->frameX(); // 0x8003fa44 — NATIVE render orchestrator twin
  c->engine.submitPage810c();                      // render submit (page-1 dim-fade owned; other pages recomp)
  c->engine.postRenderTick();                   // 0x80077D8C NATIVE (was d0)
  c->engine.areaUpdateTail();                   // 0x80075a80 NATIVE
}

// ---- NATIVE SUB-SCENE / DOOR TRANSITION (FUN_80108a60 + its 4 workers) -----------------------------
// The sm[0x4a]==5 handler is the field's sub-scene / door transition machine (fade-out -> reload the
// area-DATA for the new sub-scene -> fade-in). The user enters a hut here: walking into the door sets the
// zone-change marker, ov_field_run state 1 routes to sm[0x4a]==5, and THIS machine performs the swap.
// Two PSX leaves are owned here instead of dispatched: (a) the SCREEN FADE FUN_8007e9c8(color,a1,4) ->
// engine_fade_set (the PC-native engine fade — fixes the "fade missing on hut entry" bug; the recomp fade
// built a PSX OT rect the native renderer no longer draws); (b) the cooperative area-load
// FUN_80044bd4(0x800452c0, area, mode, 1) -> native_transition_area_load (sync, no task-spawn/yield —
// required by the native per-frame model, mirrors ov_game_submode1 state 0). Everything else is faithful
// control flow + sm field writes with the remaining leaves rec_dispatched. Decomp: scratch/decomp/game/
// transition.c (FUN_80108a60/80107afc/80107d3c/80107e20/80107f3c) + scratch/decomp/bd4.c (FUN_80044bd4:
// arg2->sm[0x6e]=area, arg3->sm[0x6d]=mode, clears 1f80019b, spawns slot-1 task 0x800452c0).

// Native replacement for FUN_80044bd4(0x800452c0, area, mode, 1): seed the sm fields the spawned load
// reads, clear the load-done flag, run the load SYNCHRONOUSLY (native_transition_area_load sets
// 1f80019b=1). The PSX phase-1 wait-loop is gone (sync runtime). Faithful to bd4.c's pre-spawn writes.
static void native_area_load_bd4(Core* c, uint32_t area, uint32_t mode) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->mem_w8(sm + 0x6e, (uint8_t)area);     // DAT_801fe0de = arg2 (area) == sm[0x6e]
  c->mem_w8(sm + 0x6d, (uint8_t)mode);     // DAT_801fe0dd = arg3 (mode) == sm[0x6d]
  c->mem_w8(0x1f80019bu, 0);               // DAT_1f80019b = 0 (load-done flag, set back to 1 by the load)
  (void)c->rng.next();                // Slip #5: this replaces a rec_dispatch(0x80044BD4).
  c->engine.sop.transitionAreaLoad();      // = the slot-1 task body 0x800452c0, run inline+sync
}

// FUN_80107afc — the MAIN door/sub-scene transition (sm[0x4c]==1..4). sm[0x4e]: 0 teardown+fade-clear+load,
// 1 FADE-OUT (to black), 2 await load, 3 FADE-IN, 4 done->return to field. Cases 1/2/3 run the per-frame
// update tail (fade frames keep the world ticking). Faithful to transition.c.
void Engine::transitionMain() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  switch (st) {
    case 0:
      c->mem_w8(0x1f800234u, 1);
      d0(c, 0x8007a810u); d0(c, 0x800798f8u); d0(c, 0x8007b0f0u); d0(c, 0x801079acu);   // scene teardown
      sm = c->mem_r32(0x1f800138u);
      c->mem_w8(sm + 0x6b, 0x1f);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      c->screenFade.applyLeafCall(0xffffffu, 0);                 // = guest FUN_8007e9c8(0xffffff, 0, 4): full black held (one-frame state)
      native_area_load_bd4(c, c->mem_r8(0x800bf870u), 0);        // FUN_80044bd4(0x800452c0,bf870,0,1)
      return;
    case 1: {                                                    // FADE-OUT — subtractive ramp
      uint32_t u = (uint32_t)c->mem_r8(sm + 0x6b) & 0x1f;
      c->screenFade.applyLeafCall((u << 19) | (u << 11) | (u << 3), 0);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6b) - 1);
      c->mem_w8(sm + 0x6b, v);
      if (v == 0) c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      break;
    }
    case 2:                                                      // await sync load (1f80019b set by load)
      // NOTE: no fade call this state. On PSX slot 4 goes empty each frame -> scene shows the newly
      // loaded content darkened only by whatever the prior fade-out left in VRAM. Matches PSX.
      if (c->mem_r8(0x1f80019bu) != 0) {
        c->mem_w8(sm + 0x6b, 0x1f);
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      }
      break;
    case 3: {                                                    // FADE-IN — additive ramp
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6b) * (uint32_t)-8) & 0xff;
      c->screenFade.applyLeafCall((u << 16) | (u << 8) | u, 1);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6b) - 1);
      c->mem_w8(sm + 0x6b, v);
      if (v == 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        d1(c, 0x80050894u, 0);
      }
      break;
    }
    case 4:                                                      // done -> back to the field area machine
      d0(c, 0x80074e48u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
      return;
    default: return;
  }
  d0(c, 0x80059c60u); d0(c, 0x8006ef38u); d0(c, 0x8007b008u); d0(c, 0x8003fa1cu);   // per-frame update (fade frames)
}

// FUN_80107d3c — transition variant (sm[0x4c]==5/6). sm[0x4e]: 0 load, 1 effect 0x8003fb84, 2 await->done.
void Engine::transitionD3c() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st == 1) {
    d0(c, 0x8003fb84u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
  } else if (st == 0) {
    c->mem_w8(0x1f800234u, 1);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
    native_area_load_bd4(c, c->mem_r8(0x800bf870u), 0);          // FUN_80044bd4(0x800452c0,bf870,0,1)
  } else if (st == 2) {
    if (c->mem_r8(0x1f80019bu) == 0) {
      d0(c, 0x8003ea88u);
    } else {
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
    }
  }
}

// FUN_80107e20 — transition variant (sm[0x4c]==7). sm[0x4e]: 0 setup+load, 1 effect 0x8003e264, 2 await->done.
void Engine::transitionE20() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st == 1) {
    d0(c, 0x8003e264u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
  } else if (st == 0) {
    d1(c, 0x80074bf8u, 9);
    d0(c, 0x8003e264u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
    c->mem_w8(0x1f800234u, 1);
    native_area_load_bd4(c, c->mem_r8(0x800bf870u), 0);          // FUN_80044bd4(0x800452c0,bf870,0,1)
  } else if (st == 2) {
    d0(c, 0x8003e894u);                                          // always runs, then checks the flag
    if (c->mem_r8(0x1f80019bu) != 0) {
      d0(c, 0x80074e48u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
    }
  }
}

// FUN_80107f3c — transition variant (sm[0x4c]==8), a 7-state machine. NB case 0 uses a DIFFERENT loader:
// FUN_80044bd4(0x80044f58, (DAT_1f800240+0x1a)&0xff, 0) where 0x80044f58 = ov_load_texgroup (a SYNC leaf,
// also called directly in sop.cpp). So we run it inline + set 1f80019b=1 (faithful to the cooperative
// spawn's net effect: texgroup loaded, load-done flag raised). Case 4 uses the normal 0x800452c0 loader.
void Engine::transitionF3c() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  switch (st) {
    case 0:
      d1(c, 0x80074bf8u, 9);
      d0(c, 0x8003e264u);
      c->mem_w8(0x1f800234u, 1);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      // FUN_80044bd4(0x80044f58, (DAT_1f800240+0x1a)&0xff, 0): spawn ov_load_texgroup (sync) -> run inline.
      c->mem_w8(0x1f80019bu, 0);
      d1(c, 0x80044f58u, (uint32_t)((c->mem_r32(0x1f800240u) + 0x1a) & 0xff));
      c->mem_w8(0x1f80019bu, 1);
      break;
    case 1:
    case 5:
      d0(c, 0x8003e264u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));   // LAB_8010809c
      break;
    case 2:
      d0(c, 0x8003e894u);
      if (c->mem_r8(0x1f80019bu) != 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        d0(c, 0x80074e48u);
        d1(c, 0x8001d71cu, 9);
        d0(c, 0x8003fb94u);
      }
      break;
    case 3:
      d0(c, 0x8003ebe0u);
      if (c->r[2] == 0) return;                                   // still running -> stay
      d0(c, 0x8001cf2cu);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));   // LAB_8010809c
      break;
    case 4:
      d1(c, 0x80074bf8u, 8);
      d0(c, 0x8003e264u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      native_area_load_bd4(c, c->mem_r8(0x800bf870u), 0);          // FUN_80044bd4(0x800452c0,bf870,0,1)
      break;
    case 6:
      d0(c, 0x8003e894u);
      if (c->mem_r8(0x1f80019bu) != 0) {
        d0(c, 0x80074e48u);
        c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
        c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
      }
      break;
    default: return;
  }
}

// FUN_80108a60 — sm[0x4a]==5 transition dispatcher on sm[0x4c]. 0/9 = done (return to the field area
// machine: sm[0x48]=2, sm[0x4a]=1, sm[0x4c]=0, sm[0x4e]=0); 1-4 main; 5/6 d3c; 7 e20; 8 f3c.
void Engine::fieldTransition() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  switch (s4c) {
    case 0: case 9:
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 0); c->mem_w16(sm + 0x4e, 0);
      break;
    case 1: case 2: case 3: case 4: c->engine.transitionMain(); break;
    case 5: case 6:                 c->engine.transitionD3c();  break;
    case 7:                         c->engine.transitionE20();  break;
    case 8:                         c->engine.transitionF3c();  break;
    default: break;
  }
}

// FIELD RUNNING sub-machine VARIANT 0x801070b4 (sm[0x4c]==3, the mid-transition running handler reached
// when a door/edge sets up an area change). A switch on sm[0x4e]: state 0 = init (scene reset + input
// reset) then fall into state 1; state 1 = run ov_field_frame_x and check the mode-3 / area-change exit
// conditions to hand back to the normal running handler (sm[0x4c]=2); state 2 = bump to 1. Faithful to
// the disasm (hand-decompiled from the field overlay).
void Engine::fieldRunX() { Core* c = core;
  uint32_t sm  = c->mem_r32(0x1f800138u);
  uint16_t s4e = c->mem_r16(sm + 0x4e);
  if (s4e >= 2) {
    if (s4e == 2) c->mem_w16(sm + 0x4e, 1);     // 0x8010721c: re-arm to running
    return;
  }
  if (s4e == 0) {                                // 0x80107100: init
    c->mem_w16(sm + 0x4e, 1);
    d0(c, 0x8006c77cu);
    d3(c, 0x8005082cu, 0, 0, 0);                 // input reset
    // fall through to state 1
  }
  c->engine.fieldFrameX();                           // 0x80108be4 per-frame (state 1, 0x80107118)
  if (c->mem_r8(0x800bf80du) == 3) {             // mode-3 exit (0x80107138)
    if (c->mem_r8(0x800bf80fu) != 0) return;
    d0(c, 0x80074bc4u);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4c, 2);                     // back to normal running handler
    int16_t  e_s = c->mem_r16s(0x800e7feeu);
    uint16_t e_u = c->mem_r16(0x800e7feeu);
    if (e_s != 0) {
      c->mem_w8(0x800bf880u, 1);
      c->mem_w16(0x1f800194u, e_u);              // sh (16-bit per disasm)
      c->mem_w16(sm + 0x4e, 0);
    } else {
      c->mem_w16(sm + 0x4e, 2);
    }
    return;
  }
  // not mode-3 (0x80107194): area-change request via bf839
  uint8_t bf839 = c->mem_r8(0x800bf839u);
  if (bf839 == 0) return;
  if (c->mem_r8(0x800bf80fu) != 0) return;
  if (bf839 == 8) {                              // 0x801071bc
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4a, 1); c->mem_w16(sm + 0x4c, 2); c->mem_w16(sm + 0x4e, 3);
    return;
  }
  if (c->mem_r8(0x1f800236u) >= 5) d1(c, 0x80050894u, 0);   // 0x801071f0
  sm = c->mem_r32(0x1f800138u);
  c->mem_w16(sm + 0x4a, 1); c->mem_w16(sm + 0x4c, 2); c->mem_w16(sm + 0x4e, 6);
}

void Engine::submode1() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (cfg_dbg("stage") && s4c <= 1)
    fprintf(stderr, "[stage] submode1 case %u: bf870=%u nexttab[bf870]=%u\n",
            s4c, c->mem_r8(0x800bf870u), c->mem_r8(0x80108f60u + c->mem_r8(0x800bf870u)));
  switch (s4c) {
    case 0:
      // Slip #3 (docs/findings/sbs.md): the recomp body of 0x801088D8 case 0 calls FUN_80044BD4 which
      // yields inside (task spawn-and-wait), so case 1's fall-through runs on the FOLLOWING tick when
      // the coro resumes. Native's transitionAreaLoad is synchronous — if we fall through here, sm[0x4c]
      // reaches 2 one tick earlier than the coro. That accumulates a +1 skew on the 0x1F80017C frame
      // counter over the cutscene-skip window (95 vs 94 by gameplay-start), producing the 0x800EE0DD
      // periodic-flag divergence (FUN_8004B374's `& 0x1F` gate samples out-of-phase at f217).
      //
      // PSX_FAITHFUL MODE (mIsFaithful=true, e.g. under SBS): split case 0 across two ticks to match
      // coro cadence — run the load on tick 1 (return without falling through), consume the deferral
      // flag on tick 2. sm[0x4c] stays 0 on tick 1, matching the recomp view where the coro is
      // suspended inside FUN_80044BD4 with the sm[0x4c] write not yet reached.
      //
      // PC MODE (mIsFaithful=false, default): the native engine can skip the coro yield outright —
      // fall through in one tick, no per-frame cost. The faithful/PC split is by design: substrate
      // parity demands cadence match, live gameplay does not, and the two modes must not converge
      // here since the recomp side lives with a real yield we don't want to inherit.
      if (c->game && c->game->mIsFaithful) {
        if (!c->engine.mSubmode1LoadDeferred) {
          rec_dispatch(c, 0x8005245cu);          // FUN_8005245c (sound/CD setup, sync leaf)
          (void)c->rng.next();               // Slip #5: this replaces a rec_dispatch(0x80044BD4).
          c->engine.sop.transitionAreaLoad();     // INLINE sync load (replaces FUN_80044bd4) -> 1f80019b=1
          c->engine.mSubmode1LoadDeferred = true;
          return;                                 // yield: match recomp coro yield inside FUN_80044BD4
        }
        c->engine.mSubmode1LoadDeferred = false;  // second tick — consume the defer, fall through
      } else {
        rec_dispatch(c, 0x8005245cu);
        c->engine.sop.transitionAreaLoad();
      }
      /* fallthrough */
    case 1: {
      c->mem_w8(0x1f800234u, 0);
      uint8_t area = c->mem_r8(0x800bf870u);
      uint8_t next = c->mem_r8(0x80108f60u + area);
      c->mem_w16(sm + 0x4c, next);
      break;
    }
    case 2: ffspan_begin(c); c->engine.fieldRun(); ffspan_end(c, "fieldrun"); break;   // field RUNNING sub-machine (sm[0x4e]) — native
    case 3: c->engine.fieldRunX(); break;              // mid-transition running sub-machine 0x801070b4 — native
    case 4: rec_dispatch(c, 0x80107230u); break;
    case 5: rec_dispatch(c, 0x8010766cu); break;
    case 6: rec_dispatch(c, 0x80107790u); break;
    default: break;                                 // >=7: no-op
  }
}

// One native loop iteration of the guest body 0x801063F4: dispatch sm[0x48] handler, bump frame counter.
// Returns 1 if handled natively, 0 if the current state is NOT yet owned and the task must hand back to
// the cooperative guest loop. OWNED so far: sm[0x48] area-init (0/1) + the RUNNING SOP-intro path
// (sm[0x48]==2, sm[0x4a]==0, SOP loaded). The transition sub-modes (sm[0x4a]!=0), the area machine
// (sm[0x4c] 0x80106478), and the non-SOP field overlays YIELD DEEP and aren't owned yet — own them (RE
// in scratch/gameplay_start_flow_re.md) to extend native ownership and shrink the cooperative fallback.
// Returning 0 keeps the field REACHABLE (no derail) until those are owned.
int Engine::frame() { Core* c = core;
  // Screen fade: reset at the top of every logic frame (PSX-faithful — OT slot 4 empties each frame,
  // so a frame with no NATIVE fade caller = no fade rect. Native SMs push after this via
  // c->screenFade.applyLeafCall / set. Still-recomp SMs' fade calls don't reach the class yet — those
  // are the top-down port frontier for closing coverage.
  c->screenFade.frameStart();
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s48 = c->mem_r16(sm + 0x48);
  if (s48 == 2) {
    uint16_t s4a = c->mem_r16(sm + 0x4a);
    // OWNED running sub-modes: 0 = SOP-intro (SOP overlay must be loaded); 1 = field area machine
    // (0x801088d8, the walkable field — its load is sync via native_transition_area_load, its running
    // states are yield-free). Other sub-modes (2..5, the area-machine variants) aren't owned yet.
    if (s4a == 0) {
      if (c->mem_r32(0x80109450u) != 0x3C021F80u) { if (cfg_dbg("gframe")) fprintf(stderr, "[gframe] ret0 s48=2 s4a=0 SOP-not-loaded ov=%08X sm@%08X\n", c->mem_r32(0x80109450u), sm); return 0; } // SOP not loaded -> cooperative
    } else if (s4a != 1) {
      if (cfg_dbg("gframe")) fprintf(stderr, "[gframe] ret0 s48=2 s4a=%u unowned-submode sm@%08X\n", s4a, sm); return 0; // unowned running sub-mode
    }
    c->engine.s48_2_frame();
  } else if (s48 == 0) {
    ffspan_begin(c); c->engine.s48_0(); ffspan_end(c, "s48_0");
  } else if (s48 == 1) {
    ffspan_begin(c); c->engine.s48_1(); ffspan_end(c, "s48_1");
  } else {
    if (cfg_dbg("gframe")) fprintf(stderr, "[gframe] ret0 unknown s48=%u sm@%08X\n", s48, sm); return 0; // unknown top state -> cooperative
  }
  c->mem_w16(0x1f800198u, (uint16_t)(c->mem_r16(0x1f800198u) + 1));   // loop tail 0x8010645c
  return 1;
}

// GAME stage TOP-LEVEL ENTRY 0x8010637C — task-0's stage driver: a one-time PROLOGUE then an infinite
// per-frame loop {dispatch sm[0x48] handler; bump frame counter DAT_1f800198; yield FUN_80051f80(1)}. The
// engine OWNS the prologue PC-native here: it is pure register/memory init of the stage state machine (no
// yield, no jal), so it is safe to reimplement and hand to the guest loop body IN-CONTEXT via the coro-
// redirect handshake — exactly like the sub-handlers. This is the entry point for owning the loop itself;
// the loop body (0x801063F4) + its handler dispatch (all 3 handlers already native) + the FUN_80051f80
// yield still run via the resumed task-0 coroutine, so deep yields/resumes are unchanged. Registered AUTO
// (it lives in GAME.BIN, flushed on overlay unload). Faithful to 0x8010637C prologue (regs the loop body
// reads MUST be set: s0=s1=0x1f800000, s2=1, sp=sp-0x20, ra saved at 0x1c(sp)):
//   sp-=0x20; sw s1,0x14; s1=0x1f800000; sw s2,0x18; s2=1; sw s0,0x10; s0=0x1f800000; (saves INCOMING regs)
//   0x1f800206=0; 0x1f800236=0; 0x1f800234=0; 0x1f80019a=2; v1=mem[0x1f800138]; a0=mem_r8(0x1f800134);
//   sw ra,0x1c; 0x1f800198=0; 0x800be0e4=0; sm[0x48]=a0; sm[0x4a]=sm[0x4c]=sm[0x4e]=sm[0x50]=0.
// Non-static: called directly from the native scheduler (native_boot.cpp) — the ONE remaining native
// task entry after the override system was removed (2026-06-22).
void Engine::stagePrologue() { Core* c = core;
  uint32_t ra = c->r[31], sp = c->r[29];
  uint32_t s0_in = c->r[16], s1_in = c->r[17], s2_in = c->r[18];
  c->r[29] = sp - 0x20;
  c->mem_w32(c->r[29] + 0x14, s1_in);          // sw s1,0x14(sp)  — save INCOMING callee-saved regs
  c->mem_w32(c->r[29] + 0x18, s2_in);          // sw s2,0x18(sp)
  c->mem_w32(c->r[29] + 0x10, s0_in);          // sw s0,0x10(sp)
  c->r[16] = 0x1f800000u;                      // s0 = 0x1f800000  (loop reads 0x198(s0))
  c->r[17] = 0x1f800000u;                      // s1 = 0x1f800000  (loop reads 0x138(s1))
  c->r[18] = 1;                                // s2 = 1           (loop compares sm[0x48]==s2)
  c->mem_w8(0x1f800206u, 0);
  c->mem_w8(0x1f800236u, 0);
  c->mem_w8(0x1f800234u, 0);
  c->mem_w8(0x1f80019Au, 2);
  uint32_t task = c->mem_r32(0x1f800138u);
  uint8_t  init48 = c->mem_r8(0x1f800134u);    // initial sm[0x48] = boot/resume selector
  c->mem_w32(c->r[29] + 0x1c, ra);             // sw ra,0x1c(sp)
  c->mem_w16(0x1f800198u, 0);                  // sh zero,0x198(s0) — frame counter reset
  c->mem_w8(0x800be0e4u, 0);
  c->mem_w16(task + 0x48, init48);             // sm[0x48] = mem_r8(0x1f800134)
  c->mem_w16(task + 0x4a, 0);
  c->mem_w16(task + 0x4c, 0);
  c->mem_w16(task + 0x4e, 0);
  c->mem_w16(task + 0x50, 0);
  if (cfg_dbg("stage")) fprintf(stderr, "[stage] ov_game_stage_prologue run, sm[0x48]=%u\n", init48);
}

// OLD guest-loop entry (prologue + coro-redirect into the guest loop 0x801063F4). SUPERSEDED by the
// native per-frame path (game_native in native_scheduler_step calls c->engine.stagePrologue +
// c->engine.frame). Retained as a reference / fallback; not on the live path.
void Engine::stageMain() { Core* c = core;
  stagePrologue();
  rec_coro_redirect(c, 0x801063F4u);
}

// Engine::areaModeDispatch — the 22-way area-mode dispatcher at guest 0x8001CAC0. See engine.h.
// The 22-entry jump table lives in MAIN.EXE .text at 0x80010000 (extracted from MAIN.EXE below);
// each valid entry is a small resident stub (0x8001CB00, 0x8001CB10, ..., 0x8001CB90) that
// `jal <overlay-handler>` then falls through to the shared epilogue 0x8001CB98. Modes 1,2,3,8,9,
// 12,14,16,17,18,19,20 point to the epilogue directly (no-op for that mode). We skip the stub
// hop and rec_dispatch the overlay handler directly. a0 = 0x800ED018 (fixed arg the dispatcher
// sets before the indirect jr, kept identical).
void Engine::areaModeDispatch() {
  Core* c = core;
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx >= 22) return;
  static const uint32_t handlers[22] = {
    /* 0*/ 0x8011534Cu, /* 1*/ 0,           /* 2*/ 0,           /* 3*/ 0,
    /* 4*/ 0x8013EE84u, /* 5*/ 0x80136CDCu, /* 6*/ 0x8014189Cu, /* 7*/ 0x8012F6ECu,
    /* 8*/ 0,           /* 9*/ 0,           /*10*/ 0x801140D0u, /*11*/ 0x80113F94u,
    /*12*/ 0,           /*13*/ 0x80116980u, /*14*/ 0,           /*15*/ 0x80116560u,
    /*16*/ 0,           /*17*/ 0,           /*18*/ 0,           /*19*/ 0,
    /*20*/ 0,           /*21*/ 0x8010B918u,
  };
  uint32_t target = handlers[idx];
  if (!target) return;
  c->r[4] = 0x800ED018u;
  rec_dispatch(c, target);
}

// Engine::sceneStateStep — the SCENE-INIT / SCENE-RUN state machine at guest 0x80050DE4. See engine.h.
// Two 22-entry overlay-handler tables (extracted verbatim from MAIN.EXE .text @0x80015A40 init,
// @0x80015A98 run, both keyed by 0x800BF870 = the area render-mode byte). Handlers take a0 = the
// scene-state base 0x800F2418; the INIT branch transitions phase 0 -> 1 after dispatch (default
// idx 9 sets phase=1 too — the recomp's L_80050F90 label). All non-INIT/RUN phases no-op.
void Engine::sceneStateStep() {
  Core* c = core;
  static constexpr uint32_t SCENE_STATE = 0x800F2418u;
  int8_t phase = c->mem_r8s(SCENE_STATE);

  if (phase == 1) {
    // RUN table (@0x80015A98). Idx 9 = default 0x80051118 = no-op return.
    uint8_t idx = c->mem_r8(0x800BF870u);
    if (idx >= 22) return;
    static const uint32_t run[22] = {
      /* 0*/ 0x8013EFA8u, /* 1*/ 0x8012EE14u, /* 2*/ 0x80123E1Cu, /* 3*/ 0x8010E964u,
      /* 4*/ 0x80116CCCu, /* 5*/ 0x80136488u, /* 6*/ 0x8013D6D8u, /* 7*/ 0x8012E2F4u,
      /* 8*/ 0x8012AB30u, /* 9*/ 0,           /*10*/ 0x80110A14u, /*11*/ 0x80113770u,
      /*12*/ 0x801144D4u, /*13*/ 0x80113D68u, /*14*/ 0x80114A78u, /*15*/ 0x80115DECu,
      /*16*/ 0x8010C9FCu, /*17*/ 0x8010BCDCu, /*18*/ 0x8010C160u, /*19*/ 0x8010B140u,
      /*20*/ 0x80116B9Cu, /*21*/ 0x8010B200u,
    };
    uint32_t target = run[idx];
    if (!target) return;
    c->r[4] = SCENE_STATE;
    rec_dispatch(c, target);
    return;
  }
  if (phase != 0) return;   // < 0 or >= 2 -> no-op (signed slti 2 + neq 0)

  // INIT table (@0x80015A40). Idx 9 = default 0x80050F90 (no-op body, then set phase=1).
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx >= 22) return;
  static const uint32_t init[22] = {
    /* 0*/ 0x8013FB4Cu, /* 1*/ 0x8012F89Cu, /* 2*/ 0x80124678u, /* 3*/ 0x8010F174u,
    /* 4*/ 0x801175D0u, /* 5*/ 0x80136CB0u, /* 6*/ 0x8013E144u, /* 7*/ 0x8012EB50u,
    /* 8*/ 0x8012B3E8u, /* 9*/ 0,           /*10*/ 0x80111238u, /*11*/ 0x80113F68u,
    /*12*/ 0x80114CCCu, /*13*/ 0x80114560u, /*14*/ 0x80115270u, /*15*/ 0x80116534u,
    /*16*/ 0x8010D21Cu, /*17*/ 0x8010C4FCu, /*18*/ 0x8010C980u, /*19*/ 0x8010B960u,
    /*20*/ 0x801173A8u, /*21*/ 0x8010B8ECu,
  };
  uint32_t target = init[idx];
  if (target) {
    c->r[4] = SCENE_STATE;
    rec_dispatch(c, target);
  }
  c->mem_w8(SCENE_STATE, 1);   // advance to RUN (all L_80050F94 paths, incl. the default at idx 9)
}

// Engine::modePerFrameDispatch — the mode-keyed per-frame overlay handler at guest 0x80022A80.
// Faithful to the disasm: skip mode 3 (A00 village) explicitly, then read the fn-pointer table
// at 0x8009D1D4 (MAIN.EXE .rodata, indexed by 0x800BF870) and dispatch the current overlay's
// entry. NO bounds check in the guest — the render-mode byte is bounded by the writer sites, not
// here. No a0 setup (the guest jalr inherits whatever a0 the caller had; ov_field_frame doesn't
// touch a0 before the call, so handlers that read a0 are dead code in this path — none observed).
void Engine::modePerFrameDispatch() {
  Core* c = core;
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx == 3) return;
  uint32_t target = c->mem_r32(0x8009D1D4u + (uint32_t)idx * 4u);
  if (!target) return;
  rec_dispatch(c, target);
}

// Engine::postRenderTick — 3-state fx-trigger + countdown on byte 0x800BF842 at guest 0x80077D8C.
// Faithful to the disasm: low 7 bits select (== 1: fire FX 41, set b42 = 0x87), (== 2: fire FX 42,
// clear b42), other/nonzero: decrement b42. Zero = no-op. FX 41/42 leaf FUN_80074590 stays substrate.
void Engine::postRenderTick() {
  Core* c = core;
  uint8_t b = c->mem_r8(0x800BF842u);
  if (b == 0) return;
  uint8_t low = (uint8_t)(b & 0x7F);
  if (low == 1) {
    c->engine.sfx.trigger(41, 2, -65);      // FUN_80074590 (native; pitchBend -65)
    c->mem_w8(0x800BF842u, 0x87);
    return;
  }
  if (low == 2) {
    c->engine.sfx.trigger(42, 2, -65);      // FUN_80074590 (native; pitchBend -65)
    c->mem_w8(0x800BF842u, 0);
    return;
  }
  c->mem_w8(0x800BF842u, (uint8_t)(b - 1));
}

// Engine::frameStartTick — per-frame prologue at guest 0x80059D28 (FIRST call in ov_field_frame's
// gameplay-update block). Faithful port of the disasm; see engine.h for the step-by-step contract.
// Callees kept substrate: the mode-keyed overlay handler (branches at (e)), FUN_8005950C default,
// and the rand LFSR advance at 0x8009A450 (the top recdep hit — 86 calls/frame — a future target).
void Engine::frameStartTick() {
  Core* c = core;
  static constexpr uint32_t G = 0x800E7E80u;   // master G block base (== s0 in the guest)

  // (a) counter@0x800BF819: if nonzero, decrement + mask two 12-bit heading fields.
  uint8_t cnt = c->mem_r8(0x800BF819u);
  if (cnt != 0) {
    c->mem_w8(0x800BF819u, (uint8_t)(cnt - 1));
    c->mem_w16(0x800ECF54u, (uint16_t)(c->mem_r16(0x800ECF54u) & 0x0FFFu));
    c->mem_w16(0x800E7E68u, (uint16_t)(c->mem_r16(0x800E7E68u) & 0x0FFFu));
  }
  // (b) zero frame-scoped flag bank.
  c->mem_w8(G + 0x177u, 0);
  c->mem_w8(G + 0x179u, 0);
  c->mem_w8(G + 0x17Au, 0);
  c->mem_w8(G + 0x17Bu, 0);
  c->mem_w8(0x1F80027Au, 0);
  // (c) per-frame stamp++.
  c->mem_w8(0x1F800247u, (uint8_t)(c->mem_r8(0x1F800247u) + 1u));

  // (d) if 0x800BF841 == 0: mode-keyed per-frame handler dispatch, clear 0x1F800230.
  if (c->mem_r8(0x800BF841u) == 0) {
    uint8_t mode = c->mem_r8(0x800BF870u);
    uint32_t target;
    switch (mode) {
      case 2:  target = 0x8010F63Cu; break;
      case 3:  target = 0x80109024u; break;
      case 7:  target = 0x80112220u; break;
      case 20: target = 0x8010F654u; break;
      default: target = 0x8005950Cu; break;
    }
    c->r[4] = G;
    rec_dispatch(c, target);
    c->mem_w8(0x1F800230u, 0);
  }

  // (e) master position + heading -> scratchpad (for projection/cull).
  c->mem_w16(0x1F800160u, c->mem_r16(G + 0x2Eu));
  c->mem_w16(0x1F800162u, c->mem_r16(G + 0x32u));
  c->mem_w16(0x1F800164u, c->mem_r16(G + 0x36u));
  c->mem_w16(0x1F80016Au, c->mem_r16(G + 0x58u));
  c->mem_w16(0x1F800168u, c->mem_r16(G + 0x56u));   // written unconditionally (delay slot)
  // (f) latch 0x800BF81E = 1 when 0x800BF9C3 & 0x80.
  if (c->mem_r8(0x800BF9C3u) & 0x80u) c->mem_w8(0x800BF81Eu, 1);

  // (g) tick sub-counter G+0x180 when 0x1F800137 (pause) == 0.
  if (c->mem_r8(0x1F800137u) == 0) {
    uint8_t v = c->mem_r8(G + 0x180u);
    if (v != 0) c->mem_w8(G + 0x180u, (uint8_t)(v - 1));
  }
  // (h) advance rand LFSR (native class Rng — shared seed at 0x80105EE8 with substrate callers).
  (void)c->rng.next();
}

// Engine::areaUpdateTail — the last direct child of ov_field_frame at guest 0x80075A80.
// Slot-table state machine over the 24-entry × 12-byte area at 0x800BE238; see engine.h for the
// per-arm contract. All 8 callees stay substrate (FUN_800998E4 buf-fill, FUN_80092660 action leaf,
// FUN_80098F90 mask-drain, FUN_80075824 + FUN_80099490 common tail, FUN_8008E0C0 key2 probe,
// FUN_80074BF8 / FUN_80074E48 sub-obj tails). Guest allocates 88 bytes of stack — the buffer address
// is passed to FUN_800998E4 and the action leaf's 4 stacked args, so we mirror the sp adjust.
void Engine::areaUpdateTail() {
  Core* c = core;
  uint32_t sp_save = c->r[29];
  uint32_t ra_save = c->r[31];
  c->r[29] = sp_save - 88u;                    // addiu sp, -88
  const uint32_t sp = c->r[29];
  const uint32_t S5 = 0x800BE1F8u;
  const uint32_t buf_addr = sp + 0x20u;

  // (1) Fill the 24-byte per-slot state buffer for this frame.
  c->r[4] = buf_addr;
  rec_dispatch(c, 0x800998E4u);

  // (2) Slot loop over 24 entries at 0x800BE238 (12 bytes each), starting at the counter at 0x800BED78.
  int32_t s2  = (int32_t)c->mem_r32(0x800BED78u);
  uint32_t s1 = 0x800BE238u + (uint32_t)s2 * 12u;
  for (; s2 < 24; s2++, s1 += 12u) {
    const uint32_t s0 = s1 + 1u;
    uint8_t kind = c->mem_r8(s1);
    if (kind == 0xFF) {
      // Action arm — hword and a3 pick by top bit of slot[3]. slot[2..7] fill a2/a3 + 4 stack args.
      uint8_t s3b = c->mem_r8(s0 + 2u);
      int16_t hword;
      uint32_t a3;
      if (s3b & 0x80u) {
        hword = c->mem_r16s(0x800A4F7Eu);
        a3    = (uint32_t)(s3b & 0x0Fu);
      } else {
        hword = c->mem_r16s(0x800BED84u);
        a3    = (uint32_t)s3b;
      }
      c->mem_w32(sp + 0x10u, (uint32_t)c->mem_r8(s0 + 3u));
      c->mem_w32(sp + 0x14u, (uint32_t)c->mem_r8(s0 + 4u));
      c->mem_w32(sp + 0x18u, (uint32_t)c->mem_r8(s0 + 5u));
      c->mem_w32(sp + 0x1Cu, (uint32_t)c->mem_r8(s0 + 6u));
      c->r[4] = (uint32_t)(int32_t)(int16_t)s2;
      c->r[5] = (uint32_t)(int32_t)hword;
      c->r[6] = (uint32_t)c->mem_r8(s0);           // slot[2]
      c->r[7] = a3;
      rec_dispatch(c, 0x80092660u);
      uint32_t mask = c->mem_r32(0x800BE358u);     // clear bit s2 in the arm-mask
      mask &= ~(1u << (uint32_t)s2);
      c->mem_w32(0x800BE358u, mask);
      c->mem_w8(s1, (uint8_t)(kind - 1u));         // kind -= 1 (0xFF -> 0xFE)
      continue;                                     // skip buf post-check (guest goto 0x80075C14)
    }
    if (kind != 0) {
      // Other-kind arm — always decrement kind; if slot[8]==4 and it hit 0, set the arm-mask bit and
      // zero slot[2]+slot[3] (guest 0x80075BE4/0x80075BEC — both writes fire via the jr delay slot).
      uint8_t s8b    = c->mem_r8(s0 + 7u);
      uint8_t newkind = (uint8_t)(kind - 1u);
      c->mem_w8(s1, newkind);
      if (s8b == 4 && newkind == 0) {
        uint32_t mask = c->mem_r32(0x800BE358u);
        mask |= (1u << (uint32_t)s2);
        c->mem_w32(0x800BE358u, mask);
        c->mem_w8(s1 + 3u, 0);                     // slot[3]
        c->mem_w8(s1 + 2u, 0);                     // slot[2]
      }
    }
    // Buf post-check: buf[s2] in {0,3} -> zero slot[1]. Reached for kind==0 and the other-kind arm.
    uint8_t bv = c->mem_r8(buf_addr + (uint32_t)s2);
    if (bv == 0 || bv == 3) c->mem_w8(s1 + 1u, 0);
  }

  // (3) Mask-drain: if any bit set, call FUN_80098F90(0) then clear the mask.
  if (c->mem_r32(0x800BE358u) != 0) {
    c->r[4] = 0;
    rec_dispatch(c, 0x80098F90u);
    c->mem_w32(0x800BE358u, 0);
  }

  // (4) Common tail leaves — both take a0 = 0x800BE1F8.
  // Was: c->engine.musicCoord.musicFadeIn() — WRONG. FUN_80075824 was misnamed as musicFadeIn;
  // the RE (2026-07-03, ghidra) shows it is the per-voice VOLUME MIXER tick, not a fade snap.
  // SBS gameplay mode surfaced the divergence at 0x800BE208/A the moment we replaced the recomp
  // dispatch with musicFadeIn; the proper port is voiceMixTick(0x800BE1F8).
  c->engine.musicCoord.voiceMixTick(S5);            // FUN_80075824 (native)
  c->r[4] = S5; rec_dispatch(c, 0x80099490u);

  // (5) Key2 branch: if the s16 at 0x800BED80 != -1, look up the entry hword and probe with FUN_8008E0C0.
  int16_t key2 = c->mem_r16s(0x800BED80u);
  if (key2 != -1) {
    c->mem_w32(S5, 0);
    uint32_t entry_addr = 0x800BE368u + (uint32_t)((int32_t)key2 * 8);
    int16_t entry_hw = c->mem_r16s(entry_addr);
    c->r[4] = (uint32_t)(int32_t)entry_hw;
    c->r[5] = 0;
    rec_dispatch(c, 0x8008E0C0u);
    if ((c->r[2] & 0xFFFFu) == 0) {
      // Guest checked (return << 16) != 0 == (return & 0xFFFF) != 0.
      uint8_t subid = c->mem_r8(0x800BE22Au);
      if (subid == 0) {
        rec_dispatch(c, 0x80074E48u);
      } else {
        c->r[4] = (uint32_t)subid;
        rec_dispatch(c, 0x80074BF8u);
        c->mem_w16(0x800BED80u, 0);
        c->mem_w8(0x800BE22Au, 0);
      }
    }
  }

  c->r[29] = sp_save;
  c->r[31] = ra_save;
}

// Engine::areaSlotAckIfMatch — FUN_80074AF0 body. Pure 21-instruction primitive over the same
// slot table (0x800BE238, 12-byte stride) + armed-mask (0x800BE358) that areaUpdateTail iterates,
// but scoped to a SINGLE ack event: the caller passes an arg encoding {entryIdx: arg & 0xFF,
// signature: arg & 0xFFFFFF00}; if the signature matches the u32 stored at slot[idx].w0's high
// 3 bytes, set the armed bit AND clear the slot's trigger-pending byte at +1. Signature mismatch
// is a silent no-op (the recomp's `bne v0, a0, 0x80074B3C` short-circuits directly to jr ra).
// RE'd verbatim from disas 0x80074AF0..0x80074B40.
void Engine::areaSlotAckIfMatch(uint32_t arg) {
  Core* c = core;
  uint32_t idx = arg & 0xFFu;
  uint32_t entry = 0x800BE238u + idx * 12u;
  uint32_t stored_hi3 = c->mem_r32(entry) & 0xFFFFFF00u;
  uint32_t arg_hi3    = arg & 0xFFFFFF00u;
  if (stored_hi3 != arg_hi3) return;
  uint32_t mask = c->mem_r32(0x800BE358u);
  c->mem_w32(0x800BE358u, mask | (1u << idx));       // set armed bit `idx`
  c->mem_w8 (entry + 1, 0);                            // clear trigger-pending byte
}

// Register the GAME-stage area-init overrides when this just-loaded overlay is GAME.BIN at the stage base.
// Detect by the fixed entry + handler signatures (START.BIN/DEMO.BIN are smaller and hold stale bytes at
// these addresses, so they never match). Called from the overlay-load scan (engine_submit.cpp); registered
// AUTO so it is flushed when GAME.BIN unloads and another overlay reuses the base (mirrors the M3 scan).
// ===== Stage-0/START.BIN task-switch + preload state machine (moved from native_boot.cpp, 2026-07
// restructure). Same stage-machine-orchestration domain this file already owns for the GAME stage; this
// cluster owns task-0's stage SWITCH (native_start_stage/native_load_overlay), the START.BIN file-table
// builder (ov_start_bin_stage), and the stage-0 preload SM (native_stage0_sm), which hands off to
// native_start_stage(c,1) to enter DEMO.

#include "scheduler.h"    // CUR_TASK + switch (native_boot.cpp scheduler; scheduler.cpp)

// --- PC-native task-0 bootstrap: own the START.BIN resolve + stage-0 overlay load top-down ----------
// Replaces the FUN_800499e8 -> FUN_80052078 -> FUN_800450bc CD subtree, which (run as a pure-PSX leaf
// now that the CD overrides are unregistered) busy-waits forever on the libcd Init/Read handshake.
// Behavioural reference (read via tools/disas.py):
//   FUN_800499e8 : CdSearchFile("\BIN\START.BIN") -> {MSF,size}; store {LBA,size} at 0x800be1e0/e4;
//                  FUN_80052078(0).
//   FUN_80052078 : FUN_800450bc(task+0xc, 0); task.state=3; task[0x6f]=0; a few libgpu/BIOS resets.
//   FUN_800450bc : FUN_8001db8c(0x80106228, LBA, size) [= cd_loadfile]; entry = STAGE_ENTRY[0]
//                  (0x8010649c); task+0xc = task+0x10 = entry.
// The per-stage {LBA,size} table lives at 0x800be1e0 (stride 8); the stage-entry table at 0x800a3ecc.
static const uint32_t STAGE_ENTRY_TBL = 0x800a3ecc;  // [0]=0x8010649c [1]=0x801062e4 [2]=0x8010637c
static const uint32_t STAGE_FILE_TBL  = 0x800be1e0;  // {LBA,size} per stage, stride 8

void cd_loadfile_native(Core* c, uint32_t dest, uint32_t lba, uint32_t size);   // cd_override.cpp (sync 0x8001DB8C/DC40)

// FUN_800450bc: load the stage overlay (if any) and point the task's restart entry at the stage code.
static void native_load_overlay(Core* c, uint32_t taskfields, uint32_t stage) {
  uint32_t entry;
  if (stage == 3) {
    entry = c->mem_r32(STAGE_ENTRY_TBL + 3 * 4);     // stage 3 is already resident: no overlay load
  } else {
    uint32_t lba  = c->mem_r32(STAGE_FILE_TBL + stage * 8);
    uint32_t size = c->mem_r32(STAGE_FILE_TBL + stage * 8 + 4);
    cd_loadfile_native(c, 0x80106228, lba, size);    // = FUN_8001db8c / cd_loadfile
    // FUN_80051f80(1) cooperative yield is a no-op with the native scheduler — skipped.
    entry = c->mem_r32(STAGE_ENTRY_TBL + stage * 4);
  }
  c->mem_w32(taskfields, entry);                     // task+0xc = restart PC
  c->mem_w32(taskfields + 4, entry);                 // task+0x10
}

// FUN_80052078: switch task 0 to the given stage (load overlay + reset the display/BIOS bits).
// Public entry: called by DEMO's LEAVE-to-GAME substate (engine_demo.cpp s5), by task0Bootstrap after
// the START.BIN file-table build, by native_stage0_sm (the boot-time inline preload), and by
// stage0Advance's final step (native scheduler tick that enters DEMO). Was `native_start_stage`
// (static) + `demo_start_stage` (public wrapper).
void Engine::startStage(uint32_t stage) {
  Core* c = core;
  uint32_t task = c->mem_r32(0x1f800138);            // current task (= task 0, 0x801fe000)
  native_load_overlay(c, task + 0xc, stage);
  c->mem_w16(task, 3);                               // task state = 3 (active)
  c->mem_w8(task + 0x6f, 0);
  rec_dispatch(c, 0x80080890u);                      // EnterCriticalSection (BIOS leaf)
  c->r[4] = c->mem_r32(task + 4);
  rec_dispatch(c, 0x80080870u);                      // B(0Fh) reset (BIOS leaf)
  rec_dispatch(c, 0x800808a0u);                      // ExitCriticalSection (BIOS leaf)
  c->r[4] = 0xff000000u;
  scheduler_yield(c);                                // ChangeThread — native scheduler yield
}

// FUN_800499e8: resolve \BIN\START.BIN natively, record its {LBA,size}, switch task 0 to stage 0.
// Called once from native_boot.cpp's game_init (the boot-init prefix). Was `native_task0_bootstrap`.
void Engine::task0Bootstrap() {
  Core* c = core;
  uint32_t lba = 0, size = 0;
  if (!disc_find_file("\\BIN\\START.BIN", &lba, &size)) {
    fprintf(stderr, "[native_boot] FATAL: cannot resolve \\BIN\\START.BIN on disc\n");
    return;
  }
  c->mem_w32(STAGE_FILE_TBL, lba);                   // 0x800be1e0 = START.BIN LBA
  c->mem_w32(STAGE_FILE_TBL + 4, size);              // 0x800be1e4 = START.BIN size
  fprintf(stderr, "[native_boot] START.BIN resolved: LBA %u, %u bytes\n", lba, size);
  startStage(0);
}

// Read a NUL-terminated guest string into `out` (bounded). Used to pull the START.BIN filename
// tables (which live in the loaded overlay) for native resolution.
static void read_guest_str(Core* c, uint32_t addr, char* out, int cap) {
  int k = 0;
  for (; k < cap - 1; k++) { char ch = (char)c->mem_r8(addr + k); out[k] = ch; if (!ch) break; }
  out[k] = 0;
}

// preload_texgroup / preload_cel / preload_build_vram / preload_stage1 (the stage-0/stage-1
// area/asset PRELOAD chain) live in game/core/asset.cpp (2026-07 restructure) — same
// asset-loading domain as ov_load_texgroup/ov_unpack_group. See asset.h for the two
// cross-TU-callable entry points used below (preload_texgroup, preload_stage1).

// Stage-0 START.BIN state machine (overlay 0x80106728), PC-native. Recomp body of 0x8010649C is a
// per-iteration yield loop over 4 sm[0x48] states (see decomp of ov_start_gen_8010649C in
// generated/ov_start_shard_0.c). We previously ran all states inline in ONE tick — that collapsed
// ~7 recomp ticks into 1, producing the Slip #1 residual (docs/findings/sbs.md). Now split via
// Engine::stage0Advance() which is called by the scheduler on each subsequent tick with a per-task
// step counter, yielding after each step so native matches coro cadence.
//
// This static wrapper stays for the sole remaining inline path (native_task0_bootstrap in the
// early boot init — before the scheduler is spinning); the scheduler path now uses stage0Advance.
static void native_stage0_sm(Core* c) {
  uint32_t task = c->mem_r32(CUR_TASK);
  c->mem_w16(task + 0x48, 0);
  c->mem_w16(task + 0x4a, 0);
  c->engine.asset.preloadTexgroup(0, 0);
  c->engine.asset.preloadStage1();
  c->engine.startStage(1);
  scheduler_yield(c);
}

// Stage-0 START.BIN entry (0x8010649C): own the file-table BUILDER PC-native. Original substrate is
// a sequence of CdSearchFile loops that resolve ~36 disc filenames and record each {LBA,size}.
// Reference: overlay disas of 0x8010649c..0x80106728 (later-211).
//
// Two build strategies, selected by c->game->mIsFaithful:
//   PC mode  (default) — bypass libcd; walk the native ISO9660 directory with disc_find_file.
//                        Faster; no dir-cache side effects. This is the shipping-game path.
//   FAITHFUL mode      — call the substrate's CdSearchFile (0x8008B8F0) per name so libcd's dir
//                        cache is populated as a side effect. Matches the substrate byte-for-byte
//                        at DAT_800AC2D4 + the entry cache (0x80102768..). Selected on SBS compare
//                        cores so the port's task0 side effects mirror the substrate.
//
// The three filename tables and one XA-singleton table below are baked into the START.BIN overlay
// (0x8010649C body); their addresses and shapes are fixed by the game.
struct StartBinLoop { uint32_t name_table, dest_table, count; };
static constexpr StartBinLoop kStartBinLoops[] = {
  { 0x80106808u, 0x800BE118u, 25 },   // \BIN\OPN/CRD/SOP/A00..A0L  -> per-overlay {LBA,size} array
  { 0x8010686Cu, 0x800BE1E0u,  3 },   // \BIN\START/DEMO/GAME.BIN   -> per-stage entry-word table
  { 0x801067F4u, 0x800BE0F0u,  5 },   // \CD\TOMBA2.IDX/IMG/DAT/SND + SWDATA.BIN
};

struct StartBinXa { uint32_t name_ptr, lba_dest; };
static constexpr StartBinXa kStartBinXa[] = {
  { 0x8010646Cu, 0x1F80021Cu },   // \CD\VOICE.XA -> scratchpad LBA
  { 0x8010647Cu, 0x1F800220u },   // \CD\DEMO.XA
  { 0x8010648Cu, 0x1F800224u },   // \CD\BGM.XA
};

// Guest addresses of the libcd primitives dispatched under FAITHFUL mode.
static constexpr uint32_t kGuestCdSearchFile = 0x8008B8F0u;   // libcd CdSearchFile(&CdlFILE, name)
static constexpr uint32_t kGuestCdPosToInt   = 0x8008A110u;   // libcd CdPosToInt(&CdlFILE) -> LBA

// PC-mode resolver: bypass libcd, walk the native ISO9660 directory.
static bool resolve_pc(Core* c, uint32_t name_ptr, uint32_t* lba, uint32_t* size) {
  char name[80]; read_guest_str(c, name_ptr, name, sizeof name);
  if (disc_find_file(name, lba, size)) return true;
  fprintf(stderr, "[start.bin] not found: %s\n", name);
  return false;
}

// FAITHFUL-mode resolver: rec_dispatch the substrate's CdSearchFile so libcd's dir cache is
// populated as a side effect. Uses a caller-owned buffer in guest RAM for the 24-byte CdlFILE
// record (pos[4] + size[4] + name[16]).
static bool resolve_faithful(Core* c, uint32_t name_ptr, uint32_t buf,
                             uint32_t* lba, uint32_t* size) {
  c->r[4] = buf; c->r[5] = name_ptr;
  rec_dispatch(c, kGuestCdSearchFile);
  if (c->r[2] == 0) return false;                    // not found — matches PSX error path
  c->r[4] = buf;
  rec_dispatch(c, kGuestCdPosToInt);
  *lba  = c->r[2];
  *size = c->mem_r32(buf + 4);
  return true;
}

// FUN_80044BD4 side-effect fulfillment for FAITHFUL mode. The primitive spawns a task, waits on a
// done flag, and RNG-stamps the caller's SM slot. Under our sync port the "wait" resolves in-place
// (the caller has already done task 1's work), so we reproduce the residual writes so B and A match
// byte-for-byte at the yield boundary. Semantics from Ghidra decomp of 0x80044BD4.
//
// Writes done DIRECTLY (not via `rec_dispatch(c, 0x80051F14)`):
//   Dispatching FUN_80051F14 leaves task 1 with state=2 (runnable), the scheduler then runs its body
//   0x80044F58 (a CD-loader that only ends by calling FUN_80051FB4 → ChangeThread yield). That yield
//   longjmps back to the scheduler with r31 mid-body (0x80052000) — the next resume dispatches to
//   that PC and misses. FUN_80051F14 also calls kernel syscalls (EnterCriticalSection / OpenTh /
//   ExitCriticalSection) that lack HLE. Reproducing the same slot-state directly (with state=0
//   because task 1's work was already done natively) skips both issues.
static constexpr uint32_t kDoneFlagAddr    = 0x1F80019Bu;   // libcd load-complete flag byte
static constexpr uint32_t kTaskFlag2Global = 0x801FE0DDu;   // FUN_80044BD4 stashes param_3 here
static constexpr uint32_t kTaskFlag3Global = 0x801FE0DEu;   // FUN_80044BD4 stashes param_2 here
static constexpr uint32_t kBiosTcbHandle   = 0xFF000000u;   // OpenTh return for slot 1 (observed
                                                             // from substrate — BIOS TCB handle format)
static constexpr uint32_t kTaskSlotStride  = 0x70u;         // task table stride (scheduler.h)
static constexpr uint32_t kTaskTableBase   = 0x801FE000u;   // task table base

void Engine::fulfillTaskSpawnAndWait(uint32_t caller_task, uint32_t task1_entry,
                                     uint8_t flag2, uint8_t flag3) {
  Core* c = core;
  const uint32_t task1 = kTaskTableBase + 1 * kTaskSlotStride;
  // (a) clear done flag; (b) stash flag globals; (c) reproduce FUN_80051F14's slot writes with
  // state=0 since task 1's body has effectively "already run" (its work was preloadTexgroup); (d)
  // RNG-stamp caller's SM slot; (e) mark done so the wait loop resolves.
  c->mem_w8(kDoneFlagAddr,    0);
  c->mem_w8(kTaskFlag2Global, flag3);
  c->mem_w8(kTaskFlag3Global, flag2);
  c->mem_w16(task1 + 0x00, 0);            // state = 0 (ENDED — scheduler skips this slot)
  c->mem_w32(task1 + 0x04, kBiosTcbHandle);
  c->mem_w32(task1 + 0x0C, task1_entry);
  c->mem_w32(task1 + 0x10, c->r[28]);     // FUN_80080930 returns gp
  c->mem_w8 (task1 + 0x6F, 0);
  c->mem_w16(caller_task + 0x56, (uint16_t)c->rng.next());
  c->mem_w8(kDoneFlagAddr, 1);
}

void Engine::startBinStage() { Core* c = core;
  const bool faithful = c->game && c->game->mIsFaithful;

  // Reserve a 32-byte guest-RAM buffer on the task stack for the CdlFILE record (24 bytes,
  // aligned). Matches the substrate's use of the task stack for its per-iteration scratch;
  // restored below so the task's saved SP is unchanged.
  const uint32_t saved_sp = c->r[29];
  c->r[29] -= 32;
  const uint32_t cdlfile_buf = c->r[29] + 8;

  auto resolve = [&](uint32_t name_ptr, uint32_t* lba, uint32_t* size) {
    return faithful ? resolve_faithful(c, name_ptr, cdlfile_buf, lba, size)
                    : resolve_pc     (c, name_ptr,             lba, size);
  };

  for (const auto& L : kStartBinLoops) {
    for (uint32_t i = 0; i < L.count; i++) {
      uint32_t lba = 0, size = 0;
      resolve(c->mem_r32(L.name_table + i * 4), &lba, &size);
      c->mem_w32(L.dest_table + i * 8,     lba);
      c->mem_w32(L.dest_table + i * 8 + 4, size);
    }
  }
  for (const auto& X : kStartBinXa) {
    uint32_t lba = 0, size = 0;
    resolve(X.name_ptr, &lba, &size);
    c->mem_w32(X.lba_dest, lba);
  }

  c->r[29] = saved_sp;

  uint32_t task = c->mem_r32(CUR_TASK);
  c->mem_w16(task + 0x4a, 0);

  // Slip #5: PC mode skips FUN_80044BD4 entirely, so we compensate with a single manual RNG advance
  // to match B's boot cadence. FAITHFUL mode reproduces FUN_80044BD4 for real via
  // fulfillTaskSpawnAndWait below (which advances the RNG once as part of its RNG stamp write) —
  // no extra bump needed there.
  if (!faithful) (void)c->rng.next();

  // Preload cadence — see Slip #1 (docs/findings/sbs.md) + stage0Advance below.
  //
  // PC mode: seed sm[0x48]=0, let stage0Advance step 0 do the first preloadTexgroup one tick later.
  // FAITHFUL mode: substrate's fresh tick reaches the SM state-0 dispatch which calls
  //   FUN_80044BD4(entry=0x80044F58, 0, 0, 0). That primitive:
  //     (a) clears the done flag DAT_1F80019B
  //     (b) writes param_2/param_3 to fixed bytes DAT_801FE0DD/DE
  //     (c) spawns task 1 via FUN_80051F14 with the given entry
  //     (d) RNG-stamps task0+0x56 with FUN_8009A450 output
  //     (e) yields until task 1 sets DAT_1F80019B
  //   Under our sync port, task 1's body (the CD-loader FUN_80044F58) is what preloadTexgroup
  //   already does natively; the yield loop resolves immediately when we mark done. Reproduce the
  //   substrate's side effects here so the task-slot state + task0+0x56 match B byte-for-byte.
  if (faithful) {
    asset.preloadTexgroup(0, 0);                        // task 1's "body" — done inline
    fulfillTaskSpawnAndWait(task, /*task1_entry=*/0x80044F58u, /*flag2=*/0, /*flag3=*/0);
    c->mem_w16(task + 0x48, 1);
  } else {
    c->mem_w16(task + 0x48, 0);
  }
  fprintf(stderr, "[start.bin] file table built (%s); preload SM stepped across ticks via stage0Advance\n",
          faithful ? "faithful/libcd" : "pc/iso9660");
}

// stage0Advance: one step of the STAGE-0 preload SM. Called by the scheduler on each tick after the
// file-table build. Matches recomp body of 0x8010649C's per-iteration yield loop (4 sm[0x48] states,
// each preceded by FUN_80051F80). Native's preloadTexgroup / preloadStage1 run instantly, so we pace
// with dummy yields to match B's coro cadence: measured B path = 8 ticks total (fresh + 7 advance).
// Sizing landed empirically from stagetrace: fresh tick + 6 advances left A one tick short (entered
// DEMO at f7 vs B at f8), so 7 advances (steps 0..6) gives an 8-tick native path matching coro.
int Engine::stage0Advance(uint8_t& step) { Core* c = core;
  // Slip #5 candidate site — see startBinStage's slip5 comment for status.
  // Matches the recomp gen_func_8010649C state loop: reads task[+0x48], writes NEXT state, yields.
  // B's cadence is spread across 8 wall-clock frames because between task-0 state ticks, task-1
  // (the FUN_80044BD4-spawned asset loader) runs its own yield ticks. Preserving the empirical
  // 7-step + fresh calibration (see docs/findings/sbs.md Slip #1 — put A at DEMO f8 matching B).
  //   step 0 = recomp state 0 tick: preloadTexgroup + sm[0x48]=1
  //   step 1 = task-1 asset-load slot (no state advance)
  //   step 2 = recomp state 1 tick: preloadStage1 + sm[0x48]=2
  //   step 3 = task-1 asset-load slot
  //   step 4 = recomp state 2 tick: sm[0x48]=3
  //   step 5 = task-1 slot (extra padding to match measured coro total)
  //   step 6 = recomp state 3 tick: startStage(1)
  uint32_t task = c->mem_r32(CUR_TASK);
  const bool faithful = c->game && c->game->mIsFaithful;
  switch (step) {
    // State 0's work (preloadTexgroup + sm[0x48]:=1) — done inline by FAITHFUL startBinStage above, so
    // this step is a no-op there. PC mode does it here to preserve the 8-tick pacing.
    case 0:
      if (!faithful) { c->engine.asset.preloadTexgroup(0, 0); c->mem_w16(task + 0x48, 1); }
      break;
    case 1:
      /* Slip #5: recomp gen_func_8010649C hits its second FUN_80044BD4 spawn around this scheduler
       * tick. */
      (void)c->rng.next();
      break;
    case 2: c->engine.asset.preloadStage1();       c->mem_w16(task + 0x48, 2); break;   // state 1 -> 2
    case 3: break;
    case 4:                                        c->mem_w16(task + 0x48, 3); break;   // state 2 -> 3
    case 5: /* task-1 slot: extra padding to match measured 8-tick coro total */ break;
    case 6:                                                                              // state 3
      startStage(1);                   // swap task0 to DEMO — rewrites task+0xc, sets state=3
      scheduler_yield(c);              // never returns; longjmp to scheduler
      return 0;                        // unreachable
  }
  step++;
  return 1;   // more steps remain
}
