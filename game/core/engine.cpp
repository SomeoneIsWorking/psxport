// engine.cpp — PC-native ownership of the GAME stage state machine (the per-area scene/update
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
#include "game_ctx.h"
#include "cfg.h"
#include "guest_abi.h"               // GuestFrame/GuestFrameSpill/guest_dispatch — ABI vocabulary
#include "engine.h"                 // class Engine — GAME/STAGE driver + per-frame method impls below
#include "render.h"                 // class Render — rend(c)->frame() / frameX() (per-frame render driver)
#include "placement.h"              // ov_place_objects — native field object-placement driver (game/world)
#include "pool.h"                    // ov_pool_init_run — native object-pool init (game/world)
#include "c_subsys.h"                // disc_find_file — native ISO9660 resolver (native_task0_bootstrap/ov_start_bin_stage)
#include "asset.h"                   // class Asset — preloadTexgroup / preloadStage1 (native_stage0_sm)
#include "cd/libcd_native.h"         // class LibcdNative — pc_skip=false faithful libcd (SBS byte-exact gate)
#include "math/rng.h"                // class Rng — Slip #5 RNG cadence gate under SBS
#include <stdio.h>

// dispatch a still-recomp leaf with up to 3 args set (helpers for the native stage machines).
// later-238 BACKDROP ATTRIBUTION (PSXPORT_BDTAG): record each ov_field_frame call's pool-write span so the
// gp0 OT-walk classifier (gpu_native.cpp) can attribute a DEFERRED prim (e.g. the tp(576,256) sea backdrop)
// to the call that BUILT it — reliable where per-pass tags / WWATCH-pc / pool-node-addresses are not. The
// span table persists across the present (which classifies the prior frame's OT) because it is reset only at
// the TOP of the next ov_field_frame. `FfSpan::lookup(addr)` returns the builder name (latest-span-wins).
// The span table + bracket stack live on `c->game->ffspan` (class FfSpan, game/render/ffspan.h).
// g_pkt_track/lo/hi retired 2026-07-02 — per-Core Render::mPktTrack/mPktLo/mPktHi (reached below).
#include "dualview_snapshot.h"    // c->rsub.dualviewSnapshot.capturePre/restorePre
// (g_render_psx + g_dualview both retired 2026-07-02 — reach as c->rsub.mode.psxRender() / dualview())
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry                    // class Game — c->game->ffspan (FfSpan) + Game::sbs
#include "sbs.h"                     // `sefprobe` probe below — Sbs::coreId/frame
// FFS: nested span tracker. c must be a Core* in scope. Same shape as FfSpan::begin/end inlined.
#define FFS(nm, call) do { \
  FfSpan& _ff = c->game->ffspan; \
  if (_ff.bdtagOn()) { PktSpan& _ps = c->rsub.pktSpan; \
    PktSpan::Snapshot _outer = _ps.save(); _ps.open(); call; \
    uint32_t _mlo, _mhi; bool _captured = _ps.current(&_mlo, &_mhi); \
    if (_captured) _ff.record(nm, _mlo, _mhi); \
    _ps.restoreMerge(_outer, _captured ? _mlo : 0xFFFFFFFFu, _captured ? _mhi : 0); } \
  else { call; } } while (0)

static inline void d0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static inline void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4]=a0; rec_dispatch(c, fn); }
static inline void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static inline void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn);
}

// TaskSm — typed lens over the GAME task-state-machine record at *0x1F800138 (guest addresses
// documented at the top of this file: sm[0x48] top state, sm[0x4a] running sub-mode, sm[0x4c] area
// machine, sm[0x4e]/[0x50] area-machine sub-state, sm[0x5c] intro timer, sm[0x69]/[0x6b] flag bytes).
// A pure re-read wrapper — same guest addresses as the raw c->mem_r/w calls it replaces, not a cache;
// construct one per use (`TaskSm sm(c);`) exactly like the raw `mem_r32(0x1f800138)` it stands in for.
struct TaskSm {
  Core* c;
  uint32_t base;
  explicit TaskSm(Core* c_) : c(c_), base(c_->mem_r32(0x1F800138u)) {}

  uint16_t top() const { return c->mem_r16(base + 0x48u); }
  void setTop(uint16_t v) { c->mem_w16(base + 0x48u, v); }

  uint16_t subMode() const { return c->mem_r16(base + 0x4Au); }
  void setSubMode(uint16_t v) { c->mem_w16(base + 0x4Au, v); }

  uint16_t stage4c() const { return c->mem_r16(base + 0x4Cu); }
  void setStage4c(uint16_t v) { c->mem_w16(base + 0x4Cu, v); }

  uint16_t s4e() const { return c->mem_r16(base + 0x4Eu); }
  void setS4e(uint16_t v) { c->mem_w16(base + 0x4Eu, v); }

  uint16_t s50() const { return c->mem_r16(base + 0x50u); }
  void setS50(uint16_t v) { c->mem_w16(base + 0x50u, v); }

  uint16_t introTimer() const { return c->mem_r16(base + 0x5Cu); }
  void setIntroTimer(uint16_t v) { c->mem_w16(base + 0x5Cu, v); }

  uint8_t f69() const { return c->mem_r8(base + 0x69u); }
  void setF69(uint8_t v) { c->mem_w8(base + 0x69u, v); }

  uint8_t f6b() const { return c->mem_r8(base + 0x6Bu); }
  void setF6b(uint8_t v) { c->mem_w8(base + 0x6Bu, v); }
};

// sm[0x48] == 0 — area INIT: advance to running (sm[0x48]=2), reset the sub-machine state, run the per-area
// setup fns. (GAME.BIN 0x801086e0) Verified runtime-exercised + RAM 0-diff.
// GUEST FRAME MIRROR (abi_extract --contract, single epilogue label -> GuestFrame RAII is safe): sp-24, ra@+16.
void Engine::s48_0() { Core* c = core;
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frame(c, kSpills);
  TaskSm sm(c);
  sm.setTop(2);          // sm[0x48] = 2 (running)
  sm.setSubMode(0);
  sm.setStage4c(0);
  sm.setF69(0);
  guest_dispatch(c, 0x80108708u, 0x8007a8e0u);   // per-area setup (resident system, synchronous)
  guest_dispatch(c, 0x80108710u, 0x8007b38cu);   // per-area setup (resident system, synchronous)
}

// sm[0x48] == 1 — area RESUME-INIT (re-enter a running area, sub-mode 1): like init but sm[0x4a]=1 plus
// flag resets. (GAME.BIN 0x80108720) Faithful transcription of the disasm; the field-intro path used to
// verify (later-168) does not hit sm[0x48]==1, so this handler is not yet runtime-exercised — its callee
// FUN_8007b3f4 is synchronous like the init pair, so it is registered alongside ov_game_s48_0.
// GUEST FRAME MIRROR (abi_extract --contract, single epilogue label -> GuestFrame RAII is safe): sp-24, ra@+16.
void Engine::s48_1() { Core* c = core;
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frame(c, kSpills);
  TaskSm sm(c);
  sm.setTop(2);          // sm[0x48] = 2 (running)
  sm.setSubMode(1);
  sm.setStage4c(0);
  sm.setF69(0);
  c->mem_w8 (0x1f8001ffu, 0xff);     // DAT_1f8001ff = 0xff
  c->mem_w16(0x1f800278u, 0);        // DAT_1f800278 = 0 (16-bit; delay-slot before the setup call)
  guest_dispatch(c, 0x80108760u, 0x8007b3f4u);   // per-area setup (resident system, synchronous)
  c->mem_w8 (0x1f800206u, 0);        // display flags cleared after setup
  c->mem_w8 (0x1f800236u, 0);
  c->mem_w8 (0x1f800234u, 0);
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
    uint16_t s4c = c->mem_r16(sm + 0x4c);
    if (s4a != mLast4a || s4c != mLast4c) {
      cfg_logf("stage", "running: sm[0x4a]=%u sm[0x4c]=%u", s4a, s4c);
      mLast4a = s4a; mLast4c = s4c;
    }
  }
  if (s4a >= 6) { rec_coro_redirect(c, 0x8010881Cu); return; }   // out of range -> guest epilogue
  c->r[31] = 0x801087CCu + (uint32_t)s4a * 0x10u;       // handler's guest return (`j 0x8010881c` trampoline)
  rec_coro_redirect(c, handler[s4a]);                  // run the handler IN-CONTEXT (survives a deep yield)
}

// s4c() reference: sm[0x4c] == the AREA machine (the 9-state load/intro/play scene state machine, GAME.BIN,
// reached as ov_game_s48_2's sub-mode 2, the area LOAD/TRANSITION path). STAGED, NOT REGISTERED — dead
// code kept for the coro-redirect handshake reference/comparison. CORRECTION (this pass): the "per-state
// bodies yield deep" claim below is FALSE — Ghidra decomp (scratch/decomp/game_all_list.c) shows
// this guest fn has NO jal to the yield primitive FUN_80051f80 anywhere in its 9 states; it's a plain
// synchronous pause/save/quit-menu sequencer. It is now OWNED as `Engine::areaLoadState()` (a plain
// method, no coro-redirect needed) and wired onto the LIVE path at `Engine::s48_2_frame`'s s4a==2 branch.
// This s4c()/state[] coro-redirect body below is retained only as the original RE reference (see
// Engine::areaLoadState's own doc comment for the ported semantics + the walkable-Tomba spawn-hunt
// negative result). Faithful transcription of the guest body:
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
    cfg_logf("stage", "ov_game_s4c ENTER sm[0x4c]=%u (caller ra=0x%08X)",
             c->mem_r16(sm0 + 0x4c), ra);
  }
  c->r[29] -= 0x18;
  c->mem_w32(c->r[29] + 0x14, ra);              // sw ra,0x14(sp)
  c->mem_w32(c->r[29] + 0x10, c->r[16]);        // sw s0,0x10(sp)
  eng(c).areaSlots.updateTail();                  // 0x80075a80 NATIVE (synchronous — verified yield-free)
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c >= 9) { rec_coro_redirect(c, 0x80106a14u); return; }   // out of range -> shared epilogue
  rec_coro_redirect(c, state[s4c]);             // run the state IN-CONTEXT (it `j`s to the epilogue itself)
}

// Engine::areaLoadState — native ownership of FUN_80106478 (the RUNNING/sm[0x4a]==2 sub-mode's
// sm[0x4c] area LOAD/TRANSITION machine; s48_2_frame's handler[2]). Verified SYNCHRONOUS: the
// decompiled body (Ghidra scratch/decomp/game_all_list.c) has no jal to the yield primitive
// FUN_80051f80 anywhere in its 9 states, so it's safe to call as a plain native method (unlike
// FUN_801088d8's FIELD area machine, which genuinely yields and stays behind rec_coro_redirect
// via s4c() above — s4c() itself is a DIFFERENT sm[0x4c] context, reused field, not this one).
//
// WALKABLE-TOMBA SPAWN HUNT — NEGATIVE RESULT: none of states 0-8 spawn anything. This machine is
// the PAUSE/SAVE/QUIT menu's confirm-dialog sequencer: state 0 arms a ~330-frame fade-out timer +
// dispatch3Way(0x2C) (audio-armed poll) + a 128-byte zero-init (FUN_8004D8B0, still un-owned);
// state 1 counts the fade-out timer down against pad-edge bit 0x800E7E68, driving a camera/view
// helper (FUN_8007E8DC); state 2 selects state-index 4 (AudioDispatch::selectState) and advances;
// states 3-5 are per-frame SAVE-prompt / CONTINUE-prompt renders (FUN_8007ED5C/EE74/EF60 — text
// widgets, own the "Save"/"Continue"/"Load data"/"Quit game" strings) gated entirely on pad-edge
// bits (0x4000 confirm, 0x10/0x40/0x2000 cursor/cancel) with SFX cues via eng(c).sfx.trigger;
// state 6 just advances sm[0x4a] to 4 (hands off to the next running sub-mode, itself substrate);
// states 7/8 are the QUIT-CONFIRM Y/N dialog (FUN_8007BF20, its own DAT_800bf84a-keyed SM) —
// on accept, state 7 calls reloadEntityPool() (was FUN_8007B3F4) and returns to sm[0x4a]==1
// (back to the FIELD area machine), i.e. "confirm quit-to-title" unwinds to the SOP/field bridge,
// it does not spawn anything either. Rules out the last un-RE'd sibling of the sm[0x4c] area
// machine as a spawn candidate.
// NOTE (found during this pass, not fixed here — out of scope for a readability-only refactor):
// gen_func_80106478 descends a 24-byte frame (ra@+20, r16@+16, abi_extract --contract) that this
// native port never mirrors. Pre-existing gap (predates this refactor); flagged for a follow-up
// pass under the "MIRROR THE GUEST STACK" rule, not addressed here since adding the frame would be
// a behavior change (new guest-stack bytes), not a readability one.
void Engine::areaLoadState() { Core* c = core;   // FUN_80106478
  TaskSm sm(c);
  switch (sm.stage4c()) {
    case 0: {
      rec_dispatch(c, 0x8001CF2Cu);                          // engine tick (substrate)
      c->mem_w16(0x800BE222u, 0x47FFu);                       // FUN_80075CEC(0x47ff): fade target (inlined —
                                                               // same private leaf music_coord.cpp inlines;
                                                               // see BgSceneTransitionSm::audioFadeTarget)
      sm.setIntroTimer(0x14A);
      sm.setStage4c((uint16_t)(sm.stage4c() + 1));
      eng(c).audioDispatch.dispatch3Way(0x2C, 0);         // native — was FUN_800750D8(0x2c,0)
      rec_dispatch(c, 0x8004D8B0u);                          // 128-byte zero-init (substrate; un-owned this pass)
      c->mem_w8(0x1F800206u, 0);
      break;
    }
    case 1: {
      int16_t v = (int16_t)(sm.introTimer() - 1);
      sm.setIntroTimer((uint16_t)v);
      if (v == 0 || (v < 0x10E && c->mem_r16(0x800E7E68u) != 0)) {
        sm.setStage4c((uint16_t)(sm.stage4c() + 1));
      }
      c->r[4] = 0xA0; c->r[5] = 0x70; c->r[6] = 0; c->r[7] = 0x17A;
      rec_dispatch(c, 0x8007E8DCu);                          // camera/view helper (substrate)
      break;
    }
    case 2:
      rec_dispatch(c, 0x8001CF2Cu);                          // engine tick (substrate)
      eng(c).audioDispatch.selectState(4);                // native — was FUN_800750A4(4)
      sm.setS4e(1);
      sm.setF6b(0);
      sm.setStage4c((uint16_t)(sm.stage4c() + 1));
      sm.setS50(0);
      [[fallthrough]];
    case 3: {
      c->r[4] = sm.s4e();
      rec_dispatch(c, 0x8007ED5Cu);                          // SAVE-prompt text render (substrate)
      if (c->mem_r16(0x800E7E68u) & 0x4000u) {
        uint16_t e = sm.s4e();
        if (e == 0) {
          rec_dispatch(c, 0x80078824u);                      // AREA START POS write (substrate)
          sm.setStage4c(8);
          c->mem_w8(0x800BF84Au, 0);
        } else if (e == 1) {
          sm.setStage4c((uint16_t)(sm.stage4c() + 1));
        }
        sm.setF6b(0);
        sm.setS4e(0);
        sm.setS50(0);
        eng(c).sfx.trigger(0x11, 0, 0);                   // native — was FUN_80074590(0x11,0,0)
      }
      if ((c->mem_r16(0x800E7E68u) & 0x10u) == 0) {
areaload_joined_68f4:
        if ((c->mem_r16(0x800E7E68u) & 0x40u) == 0) return;
        if (sm.s4e() != 0) return;
        sm.setS4e(1);
      } else {
        if (sm.s4e() == 0) return;
        sm.setS4e((uint16_t)(sm.s4e() - 1));
      }
areaload_lab_106918:
      eng(c).sfx.trigger(0x15, 0, 0);                     // native — was FUN_80074590(0x15,0,0)
      break;
    }
    case 4: {
      c->r[4] = sm.s4e();
      rec_dispatch(c, 0x8007EE74u);                          // CONTINUE/LOAD/QUIT prompt render (substrate)
      if ((c->mem_r16(0x800E7E68u) & 0x4000u) == 0) {
        if (c->mem_r16(0x800E7E68u) & 0x2000u) {
          int16_t s = (int16_t)(sm.stage4c() - 1);
          sm.setS4e(0);
          sm.setF6b(0);
          sm.setStage4c((uint16_t)s);
          eng(c).sfx.trigger(0x14, -9, 0);                // native — was FUN_80074590(0x14,-9,0)
          goto areaload_case4_edge;
        }
      } else {
        uint16_t e = sm.s4e();
        if (e == 1) {
          sm.setStage4c(7);
          c->mem_w8(0x800BF84Au, 0);
          rec_dispatch(c, 0x8001CF2Cu);                      // engine tick (substrate)
        } else if (e == 0) {
          sm.setTop(2);
          sm.setSubMode(1);
          sm.setStage4c(0);
          rec_dispatch(c, 0x8001CF2Cu);                      // engine tick (substrate; guest arg 0x11 unused by callee)
        }
        // e == 2: sm[0x4c]++ (no other write) falls straight through to the shared tail below.
        if (e == 2) sm.setStage4c((uint16_t)(sm.stage4c() + 1));
        sm.setF6b(0);
        sm.setS4e(0);
        sm.setS50(0);
        eng(c).sfx.trigger(0x11, 0, 0);                   // native — was FUN_80074590(0x11,0,0)
      }
areaload_case4_edge:
      if ((c->mem_r16(0x800E7E68u) & 0x10u) == 0) {
        if ((c->mem_r16(0x800E7E68u) & 0x40u) == 0) return;
        if (sm.s4e() > 1) return;
        sm.setS4e((uint16_t)(sm.s4e() + 1));
      } else {
        if (sm.s4e() == 0) return;
        sm.setS4e((uint16_t)(sm.s4e() - 1));
      }
      goto areaload_lab_106918;
    }
    case 5: {
      c->r[4] = sm.s4e();
      rec_dispatch(c, 0x8007EF60u);                          // QUIT-confirm render (substrate)
      if (c->mem_r16(0x800E7E68u) & 0x4000u) {
        uint16_t e = sm.s4e();
        if (e == 0) {
          sm.setStage4c((uint16_t)(sm.stage4c() + 1));
          rec_dispatch(c, 0x8001CF2Cu);                      // engine tick (substrate)
          return;
        }
        if (e != 1) return;
        int16_t s = (int16_t)(sm.stage4c() - 1);
        sm.setS4e(0);
        sm.setF6b(0);
        sm.setStage4c((uint16_t)s);
        eng(c).sfx.trigger(0x14, -9, 0);                  // native — was FUN_80074590(0x14,-9,0)
        return;
      }
      if (c->mem_r16(0x800E7E68u) & 0x10u) {
        if (sm.s4e() == 0) return;
        sm.setS4e((uint16_t)(sm.s4e() - 1));
        goto areaload_lab_106918;
      }
      goto areaload_joined_68f4;
    }
    case 6:
      sm.setSubMode(4);
      sm.setStage4c(0);
      sm.setS4e(0);
      break;
    case 7: {
      c->r[4] = 0; c->r[5] = 1;
      rec_dispatch(c, 0x8007BF20u);                          // QUIT-confirm Y/N dialog SM (substrate)
      uint8_t result = sm.f6b();
      if (result == 7) {
        reloadEntityPool();                                  // native — was FUN_8007B3F4()
        c->mem_w8(0x1F800134u, 1);
        sm.setSubMode(1);
        sm.setStage4c(0);
        sm.setS4e(0);
        return;
      }
      if ((uint8_t)(result - 1u) > 1u) return;
      sm.setStage4c(4);
      sm.setS4e(1);
      break;
    }
    case 8: {
      c->r[4] = 0x81; c->r[5] = 1;
      rec_dispatch(c, 0x8007BF20u);                          // QUIT-confirm Y/N dialog SM (substrate)
      uint8_t result = sm.f6b();
      if (result == 9) {
        sm.setF6b(0);
        sm.setStage4c(4);
        sm.setS4e(0);
        sm.setS50(0);
        return;
      }
      if ((uint8_t)(result - 1u) > 1u) return;
      sm.setStage4c(3);
      sm.setS4e(1);
      break;
    }
    default: break;   // s4c >= 9 -> no-op (shared epilogue; matches the guest's no-default switch)
  }
}

// ---- NATIVE PER-FRAME GAME LOOP (game_native path, mirrors DEMO demo_native) ------------------------
// The GAME stage is owned as a native per-frame dispatcher instead of coro-redirecting into the guest
// loop 0x801063F4: each frame ov_game_frame runs ONE loop iteration natively (dispatch the sm[0x48]
// handler + bump the frame counter), and "yield" = return. This re-wires the previously-orphaned native
// handlers and descends ownership into gameplay (the SOP field-mode machine). Prereq landed (later-217b):
// the SOP area load is native+synchronous, so SOP state-0 never yields.

// (ov_sop_field_mode moved to Sop::fieldMode — eng(c).sop.fieldMode())
#include "sop.h"                              // class Sop — transitionAreaLoad (sync FIELD transition load)
#include "render/screen_fade.h"   // class ScreenFade — the single fade driver
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
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x8010810Cu, submitPage810cFaithful()); return; }
  uint32_t task = c->mem_r32(0x1F800138u);
  if (task && c->mem_r8(task + 0x6Bu) == 1) {
    fade(c).set(ScreenFade::SUBTRACTIVE, 0x80, 0x80, 0x80);   // pause-menu dim: flat gray, held each frame page-1 handler runs
    ov_game_func_801084F8(c);              // still recomp: menu draw + cursor/page transitions
    return;
  }
  d0(c, 0x8010810cu);
}

// pc_faithful mirror of ov_game_gen_8010810C's page-1 (pause-menu dim) branch. Guest frame (sp-32,
// ra@+24, r17@+20, r16@+16 -- gen's shared prologue spills these on EVERY dispatch-table branch, so
// they're spilled here too even though r17/r16 are unused on this branch) + jal-site ras (0x801082B0
// fade leaf, 0x801082B8 menu draw) + the L_801084CC/L_801084D0 common epilogue tail (mem_w8
// 0x1F800232=0; mem_w8 0x800BF81E &= 2) that every branch falls through to and the pc_skip shortcut
// was missing entirely. The fade leaf dispatches to substrate 0x8007E9C8 (byte-exact packet-pool/
// scratchpad/OT writes -- same pattern as Sop::fieldModeFaithful) instead of the host-state-only
// ScreenFade::set() the pc_skip=true path uses. Other pages delegate whole to substrate (own frame
// + dispatch table).
void Engine::submitPage810cFaithful() { Core* c = core;
  uint32_t task = c->mem_r32(0x1F800138u);                  // gen: r2 = mem_r32(0x1F800138) BEFORE frame descent
  if (task && c->mem_r8(task + 0x6Bu) == 1) {                // page==1 -> table[1]==0x8010829C (docs/engine_re.md)
    c->r[29] -= 32;                                          // gen 8010810C shared prologue: sp-32
    const uint32_t sp = c->r[29];
    c->mem_w32(sp + 24, c->r[31]);                           // ra @ sp+24
    c->mem_w32(sp + 20, c->r[17]);                           // s1 @ sp+20
    c->mem_w32(sp + 16, c->r[16]);                           // s0 @ sp+16 (unused this branch, spilled anyway)
    c->r[4] = 0x00808080u; c->r[5] = 0; c->r[6] = 4;
    c->r[31] = 0x801082B0u;
    rec_dispatch(c, 0x8007E9C8u);                            // FUN_8007E9C8: fade-set GP0 packet builder (substrate -- byte-exact)
    c->r[31] = 0x801082B8u;
    ov_game_func_801084F8(c);                                // still-recomp: menu draw + cursor/page nav
    uint8_t v = c->mem_r8(0x800BF81Eu);                      // L_801084CC/L_801084D0 common epilogue tail
    c->mem_w8(0x1F800232u, 0);
    c->mem_w8(0x800BF81Eu, (uint8_t)(v & 2u));
    c->r[31] = c->mem_r32(sp + 24);
    c->r[17] = c->mem_r32(sp + 20);
    c->r[16] = c->mem_r32(sp + 16);
    c->r[29] += 32;
    return;
  }
  d0(c, 0x8010810cu);                                        // other pages: full substrate re-derive (own frame + dispatch)
}
// (ov_objwalk moved to ObjectList::walkAll — eng(c).objectList.walkAll())
// (ov_disp_26c88 moved to ObjectTable::dispatch — eng(c).objectTable.dispatch())
// (ov_list_walk_69b28 moved to ObjectList::walkAux — eng(c).objectList.walkAux())
// (ov_arr8_dispatch_26368 moved to Array8Dispatch::tick — eng(c).array8Dispatch.tick())
// (submode0 / submode1 are now Engine methods — Engine::submode0() / Engine::submode1())
// (Engine::fieldTransition + workers are defined in this TU; forward declared via engine.h.)

// sm[0x48]==2 RUNNING, per-frame variant: dispatch sm[0x4a] handler. handler[0] = the GAME->SOP bridge
// 0x8010882c (owned native, ov_game_submode0); the others stay rec_dispatch leaves (synchronous; a
// not-yet-sync leaf that yields is contained by the scheduler setjmp = frame-done).
// GUEST FRAME MIRROR (abi_extract --contract 0x80108784: single epilogue label at L_8010881C, spill
// precedes the sm[0x4a]<6 check -> GuestFrame RAII is safe): sp-24, ra@+16.
void Engine::s48_2_frame() { Core* c = core;
  static const uint32_t handler[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  static const uint32_t jal_ra[6] = {          // 80108784's per-case jal sites
    0x801087CCu, 0x801087DCu, 0x801087ECu, 0x801087FCu, 0x8010880Cu, 0x8010881Cu,
  };
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frame(c, kSpills);
  TaskSm sm(c);
  uint16_t s4a = sm.subMode();
  if (cfg_dbg("stage") && s4a != mLast4a) {
    cfg_logf("stage", "s48_2_frame: sm[0x4a]=%u sm[0x4c]=%u", s4a, sm.stage4c());
    mLast4a = s4a;
  }
  if (s4a < 6) {
    c->r[31] = jal_ra[s4a];
    if (s4a == 0)      { c->game->ffspan.begin(); eng(c).submode0(); c->game->ffspan.end("submode0"); }
    else if (s4a == 1) { c->game->ffspan.begin(); eng(c).submode1(); c->game->ffspan.end("submode1"); }
    else if (s4a == 5) { c->game->ffspan.begin(); eng(c).fieldTransition(); c->game->ffspan.end("transition"); }  // native FUN_80108a60
    else if (s4a == 2) { c->game->ffspan.begin(); eng(c).areaLoadState(); c->game->ffspan.end("areaload"); }      // native FUN_80106478
    else               { c->game->ffspan.begin(); rec_dispatch(c, handler[s4a]); c->game->ffspan.end("s48_2_handler"); }
  }
}

// GAME sub-mode-0 bridge 0x8010882c (sm[0x4c]/sm[0x4e] dispatch) — native. Faithful to the disasm:
// sm[0x4c]==0 & sm[0x4e]==0 -> input-reset 0x8005082c (sync leaf) + sm[0x50]=0, sm[0x4e]=1; sm[0x4e]==1
// -> run the native SOP field-mode machine; sm[0x4c]==1 -> sm[0x4c]=0, sm[0x4a]++.
// GUEST FRAME MIRROR (abi_extract --contract 0x8010882C: single epilogue label at L_801088C8 ->
// GuestFrame RAII is safe): sp-40; r16@+32, ra@+36; live r16=0x1F800000.
void Engine::submode0() { Core* c = core;
  static constexpr GuestFrameSpill kSpills[] = {{16, 32}, {31, 36}};
  GuestFrame<40, 2> frame(c, kSpills);
  c->r[16] = 0x1F800000u;
  TaskSm sm(c);
  if (sm.stage4c() == 0) {
    if (sm.s4e() == 0) {
      c->r[4] = 0; c->r[5] = 0; c->r[6] = 0;
      guest_dispatch(c, 0x8010888Cu, 0x8005082cu);   // input reset (leaf, no yield)
      TaskSm(c).setS50(0);                           // re-derive base: input reset may relocate the task record
      TaskSm(c).setS4e((uint16_t)(TaskSm(c).s4e() + 1));
    } else if (sm.s4e() == 1) {
      c->r[31] = 0x801088B0u;
      // 0x80109450 is the loaded MODE overlay's field-mode fn. Our native machine is SOP-specific, so
      // only use it when SOP is actually loaded (signature = its first insn `lui v0,0x1f80` = 0x3C021F80);
      // for any other mode/field overlay, dispatch the guest fn (until that overlay is owned natively too).
      if (c->mem_r32(0x80109450u) == 0x3C021F80u)
        (c->game && c->game->pc_skip) ? eng(c).sop.fieldMode()          // native SOP (pc_skip)
                                      : eng(c).sop.fieldModeFaithful(); // byte-mirror (faithful)
      else rec_dispatch(c, 0x80109450u);                                   // other overlay -> guest
    }
  } else if (sm.stage4c() == 1) {
    uint16_t s4a = sm.subMode();
    sm.setStage4c(0);
    sm.setSubMode((uint16_t)(s4a + 1));
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
  // MV_CHECK on this fork FAILED at 0x801FE954 (a leaf's own stack spill slot) with registers
  // (v0/v1/s0-s7/gp/sp/fp/ra/hi/lo) all MATCHING — i.e. sceneEventFifoFaithful()'s own control
  // flow/constants are not in question. Re-derived gen_func_80025588 by hand against the mirror
  // line-by-line (frame/ra discipline, the kind==0/1/>=2 branch at L_80025610..30, the FIFO
  // shift-loop trip count, and the phase 3/20-nothing vs 2/7-light-toggle vs else-0x800251f0
  // branch at L_80025694..728) and found it byte-identical — this method is NOT the bug.
  // The FIFO-drain calls 6 leaves that all "stay substrate" (0x80024e00/40aa4/74bf8/24f18/251f0/
  // 77b5c) and none is proven yield-free/side-effect-free: func_80074bf8's (music/track-control)
  // call graph reaches gen_func_80086620, which dispatches through a RUNTIME function-pointer
  // slot (not a static jump table the recompiler could enumerate, unlike every other indirect
  // dispatch in this call graph, which resolves to a closed case set) — a plausible current-BGM-
  // handler callback. MV_CHECK runs the whole call graph TWICE (native leg, then rewound
  // substrate leg) from one snapshot; any such leaf whose outcome depends on state outside
  // {RAM, scratchpad, GPRs, hi/lo} (audio/sequencer engine state, an SPU voice cursor, ...) can
  // legitimately produce a different second-invocation result — exactly the documented gate
  // limit ("host hw side effects run twice while armed") already hit and handled the same way by
  // sceneRenderListBuilder() right below. Plain call, not MV_CHECK; SBS (true single-invocation
  // lockstep) is the correct gate for this leaf chain. Re-arm once the leaves are proven
  // side-effect-free or ported native.
  if (c->game && !c->game->pc_skip) { sceneEventFifoFaithful(); return; }   // faithful: gen mirror
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

// pc_faithful field EVENT/COMMAND-QUEUE state machine — mirror of gen_func_80025588 (struct
// @0x800ed058). Guest frame (sp-32; r16=B@+16, r17@+20, ra@+24) + jal-site ras on every
// dispatch — the piece missing from sceneEventFifo() above: that version never sets c->r[31]
// before d1/d2, so its FIFO-drain/setup leaves (0x80024e00, 0x80040aa4, 0x80074bf8, 0x80024f18,
// 0x800251f0, 0x80077b5c — all confirmed to spill their incoming r31 onto their own guest stack
// frame at entry) spill whatever stale ra happens to be sitting in c->r[31] instead of gen's
// per-call-site constant. Control flow/store values are otherwise byte-identical to
// sceneEventFifo() (verified against gen_func_80025588 line by line); only the frame/ra
// discipline differs. The caller (fieldFrameFaithful) already sets c->r[31]=0x80108B70 right
// before invoking this — that value must be SPILLED here, not silently dropped.
void Engine::sceneEventFifoFaithful() { Core* c = core;
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 24, c->r[31]);
  c->mem_w32(sp + 20, c->r[17]);
  const uint32_t B = 0x800ed058u;
  c->r[16] = B;
  uint8_t st = c->mem_r8(B + 2);
  // `sefprobe` (2026-07-10, ovhit A/B triage — docs/findings/tooling.md "ovhit A/B mismatch is
  // often a call-GRAPH asymmetry, not a counting bug"): proves this NATIVE Engine method is
  // reached ONLY on SBS core A (pc_faithful) — core B (recomp_path/oracle) never enters it even
  // once, despite both cores having pc_skip=false and byte-identical RAM every frame (0-diff SBS).
  // Core B reaches the SAME final RAM state via the pure-substrate per-frame chain instead (a
  // topologically different call graph that happens to converge on the same guest leaves this
  // method also calls, e.g. Animation::advanceLinkChain/0x80077b5c below) — this is the reference
  // technique for telling "genuine control-flow bug" apart from "expected native-vs-substrate
  // call-graph asymmetry" the next time ovhit flags an A-only or B-only leaf.
  if (cfg_dbg("sefprobe")) {
    Sbs* sbs = c->game ? c->game->sbs : nullptr;
    int cid = sbs ? sbs->coreId(c) : -1;
    cfg_logf("sefprobe", "f%u core=%c ENTRY st=%u",
             sbs ? sbs->frame() : 0, cid < 0 ? '-' : (cid ? 'B' : 'A'), (unsigned)st);
  }
  if (st == 0) {
    c->mem_w8(B + 2, 1);
    c->mem_w8(B + 9, 0);
    uint32_t head = c->mem_r32(0x800ecf58u);
    c->mem_w8(0x800bfa5cu, 0);
    c->mem_w32(B + 0x3c, head);
    c->r[31] = 0x800255E0u;
    d1(c, 0x80024e00u, B);
    // fall through into the active body
  } else if (st != 1) {
    c->r[31] = c->mem_r32(sp + 24);
    c->r[17] = c->mem_r32(sp + 20);
    c->r[16] = c->mem_r32(sp + 16);
    c->r[29] += 32;
    return;                      // st >= 2: guest jumps straight to the epilogue
  }
  if (c->mem_r8(B + 0x14) == 0 && c->mem_r8(B + 0x15) != 0) {
    c->r[31] = 0x80025610u;
    d2(c, 0x80040aa4u, c->mem_r8(B + 0x16), c->mem_r8(B + 0x1c));
    uint8_t kind = c->mem_r8(B + 0x1c);
    c->r[31] = 0x80025630u;
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
  c->r[31] = 0x8002569Cu;
  d1(c, 0x80024f18u, B);
  uint8_t phase = c->mem_r8(0x800bf870u);
  if (phase == 3 || phase == 20) {
    // neither the light-toggle nor 0x800251f0
  } else if (phase == 2 || phase == 7) {
    if (c->mem_r8(0x800bf816u) == 0 && (c->mem_r16(0x800e7e68u) & 0x0c00) != 0)
      c->mem_w8(B + 8, (uint8_t)(1 - c->mem_r8(B + 8)));
  } else {
    c->r[31] = 0x80025728u;
    d1(c, 0x800251f0u, B);
  }
  c->r[31] = 0x80025730u;
  d1(c, 0x80077b5cu, B);
  c->r[31] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 32;
}

// Native FUN_8004FE84 — a 2-phase scene/render-list builder driver (struct @0x800bf548). base[0] is the
// phase: 0 -> ARM (snapshot list ptr 0x800ecf64 into base+0x2b0, +0x2b4 = ptr+0x10, +0x2b8 = ptr+0x10 +
// (*(u16)ptr << 1); base[1]=0, base[0]=1); 1 -> run sub-state base[1] (0->0x8004f430, 1->0x8004f474,
// 2->0x8004f514, 3->0x8004f6d0, >=4 none). After the sub-state, set bit0 of flag byte @0x800bf822 when
// (base[1]!=0 || base[0x0a]!=0) else clear it. phase>=2 is a no-op. Leaf callees stay substrate. Faithful
// to the recomp body; a direct child of ov_field_frame (was `d0(c, 0x8004fe84)`).
void Engine::sceneRenderListBuilder() { Core* c = core;
  // sceneRenderListBuilderFaithful dispatches through rec_dispatch to substrate leaves
  // (0x8004F430/74/514/6D0, selected by base[1]); those leaves are not proven yield-free, so
  // MV_CHECK's synchronous compare can observe residual v0/v1 across a yield boundary and
  // misreport it as a divergence (mv_tdd.log 2026-07-08: 0x8004FE84 FAILED (2+ diffs) at v0/v1 —
  // not a real yield-abort, a mismatch abort from this). Plain call, not MV_CHECK — yields —
  // SBS-gated; re-arm once the leaves are proven yield-free or ported native.
  if (c->game && !c->game->pc_skip) { sceneRenderListBuilderFaithful(); return; }   // faithful: gen mirror
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

// Faithful mirror of gen_func_8004FE84 (generated/shard_1.c) -- adds the guest-stack frame discipline
// the plain native body (above) omits: sp-=24 at entry, mem_w32(sp+16,r16_entry) unconditionally
// (before r16 is repurposed as the struct base) / mem_w32(sp+20,r31_entry) unconditionally (gen's
// delay-slot store on the phase==1 test, fires on every phase value), matching restore + sp+=24 at
// the single shared exit; and the literal jal-site r31 constants before each of the 4 sub-state
// dispatch leaves (their own gen bodies save r31 to their own stack frame, so a wrong r31 there is a
// second guest-visible diff). Logic is identical to Engine::sceneRenderListBuilder() otherwise.
void Engine::sceneRenderListBuilderFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);              // save entry r16 (gen: sw r16,16(sp) before r16 reassigned)
  const uint32_t B = 0x800bf548u;
  c->r[16] = B;
  uint8_t phase = c->mem_r8(B + 0);
  c->mem_w32(sp + 20, c->r[31]);              // save entry r31 (gen: delay-slot store on the phase==1 test)

  if (phase == 1) {
    uint8_t sub = c->mem_r8(B + 1);
    switch (sub) {
      case 0: c->r[31] = 0x8004FF30u; c->r[4] = B; rec_dispatch(c, 0x8004F430u); break;
      case 1: c->r[31] = 0x8004FF40u; c->r[4] = B; rec_dispatch(c, 0x8004F474u); break;
      case 2: c->r[31] = 0x8004FF50u; c->r[4] = B; rec_dispatch(c, 0x8004F514u); break;
      case 3: c->r[31] = 0x8004FF60u; c->r[4] = B; rec_dispatch(c, 0x8004F6D0u); break;
      default: break;                          // sub >= 4: no sub-handler (guest j 8004ff60)
    }
    uint32_t flag = 0x800bf822u;
    uint8_t v = c->mem_r8(flag);
    if (c->mem_r8(B + 1) != 0 || c->mem_r16s(B + 0x0a) != 0)
      c->mem_w8(flag, (uint8_t)(v | 1));
    else
      c->mem_w8(flag, (uint8_t)(v & 0xfe));
  } else if (phase == 0) {
    uint32_t p = c->mem_r32(0x800ecf64u);
    uint32_t r3 = p + 0x10;
    c->mem_w32(B + 0x2b0, p);
    c->mem_w32(B + 0x2b4, r3);
    uint16_t h = c->mem_r16(p);
    c->mem_w8(B + 1, 0);
    c->mem_w8(B + 0, 1);
    c->mem_w32(B + 0x2b8, r3 + ((uint32_t)h << 1));
  }
  // phase >= 2: no-op -- straight to epilogue, matching gen's L_8004FFA4 direct jump.

  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// FIELD PER-FRAME UPDATE 0x80108b0c — native control flow (the field frame's work driver, called by
// the running states of the sm[0x4e] machine). Faithful to the disasm: bump *0x1f80017c and *0x800bf878;
// if NOT paused (*0x1f800136==0) run the 11-call gameplay-update block; if *0x1f800136 < 2 run 0x8003f9a8;
// then always the render-submit 0x8010810c + 0x80077d8c + per-frame area update 0x80075a80. The
// object-walk 0x8007a904 and display 0x80026c88 now run as the NATIVE ov_objwalk / ov_disp_26c88
// (direct C calls — the previously-orphan bodies wired into the live field frame); the remaining
// callees stay rec_dispatch leaves until owned in turn. NOT yield-free (0x8003F9A8 render
// orchestrator + other callees can scheduler_yield) — the earlier "yield-free (transitive jal scan)"
// claim here was wrong; the fork below is a plain call, proven by SBS full, not MV_CHECK
// (mv_tdd.log 2026-07-08: 0x80108B0C FAILED 10+ diffs — a double-run/rewind artifact, not a real bug).
// pc_faithful field per-frame update — mirror of ov_game_gen_80108B0C. Guest frame (sp-24,
// r16@+16, ra@+20, r16=0x1F800000 live for callee spills) + jal-site ras on every child. The
// children run their native owners (the ported path — byte-exactness is each owner's own gate);
// the audio-command-queue tail 0x80075A80 is dispatched substrate per the f11 lib-fallback
// recipe (same fork the DEMO tail uses — demo.cpp demo_tail_75a80_faithful). NO dualviewSnapshot
// capture/restore here: with the substrate render orchestrator executing underneath
// (Render::frame), its guest writes ARE faithful state — rewinding them would diverge from B.
void Engine::fieldFrameFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->r[16] = 0x1F800000u;
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: full gameplay update
    c->r[31] = 0x80108B50u; FFS("ff_59d28", eng(c).frameStartTick());
    c->r[31] = 0x80108B58u; FFS("ff_69b28", eng(c).objectList.walkAux());
    c->r[31] = 0x80108B60u; FFS("ff_26368", eng(c).array8Dispatch.tick());
    c->r[31] = 0x80108B68u; FFS("ff_objwalk", eng(c).objectList.walkAll());
    c->r[31] = 0x80108B70u; FFS("ff_25588", eng(c).sceneEventFifo());
    c->r[31] = 0x80108B78u; FFS("ff_4fe84", eng(c).sceneRenderListBuilder());
    c->r[31] = 0x80108B80u; FFS("ff_disp26c88", eng(c).objectTable.dispatch());
    c->r[31] = 0x80108B88u; FFS("ff_22a80", eng(c).modePerFrameDispatch());
    c->r[31] = 0x80108B90u; FFS("ff_6ec44", CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update());
    c->r[31] = 0x80108B98u; FFS("ff_50de4", eng(c).sceneStateStep());
    c->r[31] = 0x80108BA0u; FFS("ff_1cac0", eng(c).areaModeDispatch());
  }
  if (c->mem_r8(0x1f800136u) < 2) { c->r[31] = 0x80108BBCu; rend(c)->frame(); }   // 0x8003F9A8 underneath
  c->r[31] = 0x80108BC4u; FFS("ff_submit810c", eng(c).submitPage810c());
  c->r[31] = 0x80108BCCu; FFS("ff_77d8c", eng(c).postRenderTick());
  c->r[31] = 0x80108BD4u; rec_dispatch(c, 0x80075A80u);   // audio-cmd queue tail — substrate (lib fallback)
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

void Engine::fieldFrame() { Core* c = core;
  // yields — SBS-gated: fieldFrameFaithful() drives the render orchestrator (0x8003F9A8) and other
  // callees that can scheduler_yield; MV_CHECK only supports yield-free mirrors (strictCheck aborts
  // on yield-while-inCheck) and a rewind+replay of the substrate body here does not reproduce the
  // same interleaving as the live run, so the compare is bogus (10+ byte diffs at 0x801FE8A0.. are
  // an artifact of that double-run, not a real native/substrate mismatch). Byte-exactness for this
  // fork is proven by SBS full (core A vs core B), not MV_CHECK.
  if (c->game && !c->game->pc_skip) { fieldFrameFaithful(); return; }   // faithful: gen mirror
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: full gameplay update
    FFS("ff_59d28", eng(c).frameStartTick()); FFS("ff_69b28", eng(c).objectList.walkAux());    // 0x80059d28/0x80069b28 NATIVE
    FFS("ff_26368", eng(c).array8Dispatch.tick()); FFS("ff_objwalk", eng(c).objectList.walkAll());     // 0x80026368/0x8007a904 NATIVE
    FFS("ff_25588", eng(c).sceneEventFifo()); FFS("ff_4fe84", eng(c).sceneRenderListBuilder());   // 0x80025588/0x8004fe84 NATIVE (Engine methods)
    FFS("ff_disp26c88", eng(c).objectTable.dispatch());                                            // 0x80026c88 NATIVE
    FFS("ff_22a80", eng(c).modePerFrameDispatch());                                // 0x80022a80 NATIVE (Engine::modePerFrameDispatch)
    FFS("ff_6ec44", CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update());         // 0x8006ec44 NATIVE (CutsceneCamera::update)
    FFS("ff_50de4", eng(c).sceneStateStep());                 // 0x80050de4 NATIVE (Engine::sceneStateStep)
    FFS("ff_1cac0", eng(c).areaModeDispatch());               // 0x8001cac0 NATIVE (Engine::areaModeDispatch)
  }
  // DUAL-VIEW: snapshot the post-gameplay / pre-render state so the side-by-side PSX render pass (the
  // ACTUAL dualview feature, native_boot.cpp's mode.dualview() branch) can re-run the substrate render
  // into the right-hand pane. No-op unless dualview is on; kept even though the plain pc_render fork
  // below no longer rewinds (see comment there) because the dualview feature still needs a pre-render
  // snapshot to make its second pass re-runnable.
  c->rsub.dualviewSnapshot.capturePre(c);
  if (c->mem_r8(0x1f800136u) < 2) rend(c)->frame();   // 0x8003f9a8 — substrate render orchestrator (ALWAYS runs, both render modes)
  // NO restorePre HERE (fixed 2026-07-08, issue: default ./run.sh renders BLACK — poly=0/rect=0 at
  // free-roam). Render::frame() (game/render/render_frame.cpp) was repointed 2026-07-07 (commit 9d436e3,
  // issue #32: "PSX render path ALWAYS executes underneath") to unconditionally dispatch the FULL substrate
  // orchestrator 0x8003f9a8 in BOTH render modes — its guest writes (OT links, packet pool, walk cursors,
  // scratchpad GTE workspace) are now BYTE-IDENTICAL to the recomp reference by construction, exactly like
  // fieldFrameFaithful()'s call to the same rend(c)->frame() (which has never rewound them, is the
  // fieldFrame the whole pc_faithful/SBS-full byte-exact proof is built on).
  //
  // The restore this replaces predates that pivot: back when Render::frame() ran a PARTIAL, pc_render-only
  // pass list (see 9d436e3's diff) that genuinely diverged from the recomp reference's writes, rewinding
  // them here was the correct decoupling (docs/findings/sbs.md 0x800BF81E finding, later-284/292). The
  // pivot changed Render::frame() but this fork of fieldFrame() was never updated to match — an asymmetry
  // between fieldFrame() (still restoring) and fieldFrameFaithful() (never did) that nothing caught because
  // SBS full/9d436e3's own verification both exercise fieldFrameFaithful(), not this fork.
  //
  // The stale rewind's actual effect: it ran BEFORE the OT walk. The per-frame OT is main RAM (part of the
  // 2MB dualviewSnapshot restores) and gets cleared once per frame near the top of the native_boot.cpp
  // frame loop, before c->game->pcSched.step() (which is what reaches this function) even starts; capturePre
  // above snapshots that CLEARED OT. mRender->frame() above then fills it. rec_dispatch(c,0x8003f9a8u)
  // returns, this fork used to call restorePre() and wipe the OT straight back to the pre-render (i.e.
  // EMPTY) snapshot — so by the time native_boot.cpp's own post-scheduler eng(c).drawOTag(...) walk ran
  // (the pc_render picture draw, a read-only pass over guest RAM), the OT had nothing in it: poly=0, rect=0,
  // black screen, every frame, forever. fieldFrameFaithful() never had this rewind, so its OT survives to
  // drawOTag() intact — which is why GATE=1 (pc_faithful/pc_render) and PSXPORT_ORACLE=1 both rendered fine
  // while the default ./run.sh (pc_skip=true, this fork) did not.
  //
  // pc_render stays a read-only overlay: it never itself writes guest memory (drawOTag only reads OT/scene
  // data and writes host VK batches); the substrate writes above are gameplay-side (same call path
  // fieldFrameFaithful takes), not a pc_render violation.
  FFS("ff_submit810c", eng(c).submitPage810c()); // render submit (page-1 dim-fade owned; other pages recomp)
  FFS("ff_77d8c", eng(c).postRenderTick());   // 0x80077d8c NATIVE (Engine::postRenderTick)
  FFS("ff_area75a80", eng(c).areaSlots.updateTail());   // 0x80075a80 NATIVE (AreaSlots::updateTail)
}

// -- Small per-object leaves shared across many behavior handlers. Ghidra decomp:
//    scratch/decomp/batch_leaves.c ----------------------------------------------------------

// Object fields touched by the animTick/objMatrixCompose/walkStart leaf cluster below.
// Named locally rather than added to game/object/actor.h: several offsets are overloaded with a
// DIFFERENT meaning elsewhere (e.g. obj+0x46 is Actor::retryDelay() in another sub-behavior's
// state — see actor.h) so a shared accessor would misname one caller or the other.
namespace ObjAnimField {
  constexpr uint32_t kAnimMode   = 0x46u;   // u8:  current anim mode (walkStart's dedupe/set field)
  constexpr uint32_t kAnimResult = 0x79u;   // u8:  animTick's stashed VM return byte
}

// FUN_80040CDC is NOT owned here — it is ScriptInterp::init (game/scene/script_interp.cpp), the
// cutscene-script bytecode init (sets obj[0x7C]=tableA / obj[0x46]=0xFF, clears 0x10/0x70/0x78,
// loads the first entry, derives obj[0x71] from the op flags). A dead, mis-named duplicate lived
// here as Engine::animEnvInit ("animation-env init") — the fields it wrote were the SCRIPT machine's,
// not an anim env's. It had no callers and was registered nowhere; deleted after `codemap.py
// --conflicts` surfaced the dual-ownership (see docs/findings/scene.md).

// Engine::animTick — FUN_8004190C. Ticks the animation VM (native Animation::step, which is the
// full port of FUN_80076D68 — its 3 frame sub-leaves stay substrate) and stashes its return byte
// into obj+0x79. Returns 1 (matches recomp v0).
uint32_t Engine::animTick(uint32_t obj) { Core* c = core;
  using namespace ObjAnimField;
  // GUEST FRAME MIRROR (abi_extract --contract, single epilogue label -> RAII safe): sp-24;
  // r16@+16, ra@+20; live r16=obj, r31=0x80041920 at the FUN_80076D68 call. stepFramed pushes
  // 76D68's own 40-byte frame exactly like the gen callee does — step() (frameless) left every
  // downstream substrate spill 24+40 bytes high vs core B (SBS watch-cut f218, 2026-07-10).
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {31, 20}};
  GuestFrame<24, 2> frame(c, kSpills);
  c->r[16] = obj;
  c->r[31] = 0x80041920u;
  eng(c).animation.stepFramed(obj);                               // native, mirrors 76D68's frame
  c->mem_w8(obj + kAnimResult, (uint8_t)c->r[2]);
  c->r[2] = 1;
  return 1;
}

// Engine::announcerCue — FUN_8004ED94. `id` sign-extended s16, then times-2 index into u16 table
// at *DAT_800BF7FC. That u16 offset is added to base *DAT_800BF800, then FUN_8004FA38 fires.
void Engine::announcerCue(uint32_t id, uint8_t flag) { Core* c = core;
  const int32_t  id32   = (int32_t)(int16_t)(uint16_t)id;              // (id << 16) >> 15 in the decomp = sext16 << 1
  const uint32_t idxOff = (uint32_t)(id32 * 2);
  const uint32_t tblPtr = c->mem_r32(0x800BF7FCu);
  const uint16_t entry  = c->mem_r16(tblPtr + idxOff);
  const uint32_t base   = c->mem_r32(0x800BF800u) + (uint32_t)entry;
  c->r[4] = base; c->r[5] = 0xFFFFFFFFu; c->r[6] = flag;
  rec_dispatch(c, 0x8004FA38u);                                       // announcer-cue queue push (substrate)
}

// FUN_800518FC is NOT owned here — it is NodeXform::buildWithOffset (game/render/node_xform.cpp),
// now the SOLE owner (registered as the 0x800518FC override, MIRROR_VERIFY byte-exact to substrate
// across 23k+ passes). A duplicate lived here as Engine::objMatrixCompose (same guest fn via substrate
// leaves 0x80085480/84110/84470/51128); it + its 4 SOP-intro callers were retired onto buildWithOffset
// after `codemap.py --conflicts` surfaced the dual-ownership (see docs/findings/render.md).

// Engine::walkStart — FUN_80054D14.
uint32_t Engine::walkStart(uint32_t obj, uint32_t mode, int16_t subMode) { Core* c = core;
  using namespace ObjAnimField;
  // GUEST FRAME MIRROR (abi_extract --contract, single epilogue label -> RAII safe): sp-32;
  // spills r16@+16, r17@+20, r18@+24, ra@+28; live r16=obj, r17=mode, r18=subMode; r31 constants
  // per call site. The gen spills BEFORE the early-exit test, so the mirror must too: the
  // early-exit path still leaves the caller's ra at sp+28 (SBS watch-cut f747 diverged on exactly
  // that slot when the mirror sat below the test — stale byte vs B's spilled 0x8010A990).
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {17, 20}, {18, 24}, {31, 28}};
  GuestFrame<32, 4> frame(c, kSpills);
  c->r[16] = obj; c->r[17] = mode; c->r[18] = (uint32_t)(int32_t)subMode;
  const uint8_t cur = c->mem_r8(obj + kAnimMode);
  if ((uint32_t)cur == (mode & 0xFFu)) { c->r[2] = 0; return 0; }
  c->mem_w8(obj + kAnimMode, (uint8_t)mode);
  guest_fn(c, 0x80054790u, 0x80054D58u, obj, mode);                   // pre-hook (substrate)
  // gen_func_80054D14 passes FOUR args: a3 = subMode (sext16). gen_func_80077CFC consumes it as
  // the anim PHASE SEED (obj+0x0E = a3 + 0x1000) and as the frame-seek arg for the stream decoder
  // (FUN_80075FF8/75F0C a2). Leaving a3 stale seeked the decoder to a garbage frame — Tomba's
  // wrong walk pose + Charles' narration-scene vertex explosion (2026-07-10).
  const uint32_t subMode32 = (uint32_t)(int32_t)subMode;
  if (subMode == 0) guest_fn(c, 0x80077C40u, 0x80054D78u, obj, 0x80017FE8u, mode, subMode32);
  else              guest_fn(c, 0x80077CFCu, 0x80054D90u, obj, 0x80017FE8u, mode, subMode32);
  c->r[2] = 1;
  return 1;
}

// Engine::playerGrowthStep moved to ActorTomba::growthStep (game/player/actor_tomba.cpp).

// Engine::uploadModeSprites — native ownership of FUN_80067DA8 (Ghidra decomp
// scratch/decomp/fun_80067da8.c). Stages a RECT struct (X=0x1F0, Y=<per-strip>, W=0x10, H=1) on
// the guest stack and hands it to the substrate LoadImage leaf FUN_80081218 for each of the 5
// mode-selected sprite patterns. Kept as a per-frame VRAM upload — no PC-native texture cache
// hookup yet, since the patterns are consumed by still-substrate UI code that reads back VRAM.
void Engine::uploadModeSprites() { Core* c = core;
  const uint8_t mode = c->mem_r8(0x800BF88Du);
  uint32_t p0, p1, p2, p3, p4;
  switch (mode) {
    case 0: p0 = 0x800A4800u; p1 = 0x800A4820u; p2 = 0x800A48C0u; p3 = 0x800A48E0u; p4 = 0x800A4980u; break;
    case 1: p0 = 0x800A4840u; p1 = 0x800A4860u; p2 = 0x800A4900u; p3 = 0x800A4920u; p4 = 0x800A49A0u; break;
    case 2: p0 = 0x800A4880u; p1 = 0x800A48A0u; p2 = 0x800A4940u; p3 = 0x800A4960u; p4 = 0x800A49C0u; break;
    default: return;                                  // recomp: any other value early-exits
  }

  // Stage the shared RECT on the guest stack (X, Y, W, H = u16 × 4). Y is patched per strip. The
  // recomp allocates a 0x30-byte frame; we mirror it so LoadImage's arg1 pointer + any deep
  // stack use falls in the same window.
  const uint32_t sp_save = c->r[29];
  const uint32_t ra_save = c->r[31];
  c->r[29] = sp_save - 0x30u;
  const uint32_t rect = c->r[29] + 0x10u;             // sp+0x10..sp+0x18 = the RECT struct
  c->mem_w16(rect + 0u, 0x1F0);                       // X = 496
  c->mem_w16(rect + 4u, 0x10);                        // W = 16
  c->mem_w16(rect + 6u, 1);                           // H = 1

  auto upload = [&](uint16_t y, uint32_t data) {
    c->mem_w16(rect + 2u, y);                         // patch Y
    c->r[4] = rect;
    c->r[5] = data;
    rec_dispatch(c, 0x80081218u);                     // LoadImage(rect, data) — substrate leaf
  };
  upload(0x1E2, p0);
  upload(0x1E5, p1);
  upload(0x1C9, p2);
  upload(0x1D0, p3);
  upload(0x1B3, p4);

  c->r[29] = sp_save;
  c->r[31] = ra_save;
}

// Engine::gStateMutate — native ownership of FUN_80058304 (Ghidra decomp scratch/decomp/
// fieldrun_s2_init.c). See engine.h for the semantics of each case. Two guest leaves stay
// substrate: FUN_8004ED94(id, 0x41) (the announcer/UI cue queue) and FUN_800310F4(0x25, 0)
// (case 1's inventory refresh) — neither has a native equivalent yet. Sfx::trigger is native
// (Sfx class) so the alt-cue path routes there directly.
void Engine::gStateMutate(uint32_t G, uint8_t op) { Core* c = core;
  auto cue = [&](uint32_t id) { announcerCue(id, 0x41); };            // native FUN_8004ED94
  const uint8_t f174 = c->mem_r8(G + 0x174u);
  const uint8_t f0D  = c->mem_r8(G + 0x0Du);
  uint8_t n174 = f174, n0D = f0D;

  switch (op) {
    case 0:
      if (f174 & 0x08) break;                        // already-set fast exit
      cue(0x3A);
      n174 = f174 | 0x08;
      n0D  = f0D  | 0x42;
      c->mem_w8(G + 0x174u, n174); c->mem_w8(G + 0x0Du, n0D);
      break;

    case 1: {
      if ((f174 & 0x08) == 0) break;
      c->r[4] = 0x25; c->r[5] = 0; rec_dispatch(c, 0x800310F4u);
      n174 = f174 & 0xF7;
      n0D  = f0D  & 0xBD;
      c->mem_w8(G + 0x174u, n174); c->mem_w8(G + 0x0Du, n0D);
      cue(0x3B);
      break;
    }

    case 2:
      if ((f174 & 0x08) == 0) break;
      cue(0x3B);
      n174 = f174 & 0xF7;
      n0D  = f0D  & 0xBD;
      c->mem_w8(G + 0x174u, n174); c->mem_w8(G + 0x0Du, n0D);
      break;

    case 3:
    case 4: {
      const uint8_t bit = (op == 3) ? 0x20u : 0x10u;
      n174 = f174 | bit;
      n0D  = f0D  | 0x12;
      c->mem_w8(G + 0x174u, n174); c->mem_w8(G + 0x0Du, n0D);
      cue(0x4B);
      c->mem_w8(0x1F800247u, 0);
      break;
    }

    case 5:
      n174 = f174 & 0xCF;
      n0D  = f0D  & 0xED;
      c->mem_w8(G + 0x174u, n174); c->mem_w8(G + 0x0Du, n0D);
      cue(0x4C);
      break;

    case 6:
    case 7: {
      if (op == 6) n174 = f174 | 0x04;
      else         n174 = f174 & 0xFB;
      c->mem_w8(G + 0x174u, n174);
      eng(c).actorTomba.growthStep((op == 6) ? 1 : 0);              // native ActorTomba::growthStep (FUN_80057DC0)
      c->mem_w16(0x800BF89Eu, c->mem_r16(G + 0x17Eu));
      cue(op == 6 ? 0x49u : 0x4Au);
      break;
    }

    case 8:
    case 0xD: {
      if (f174 & 0x01) { c->mem_w8(0x800BF881u, f174); return; }
      n174 = (uint8_t)((f174 & 0xFD) | 0x01);
      c->mem_w8(G + 0x174u, n174);
      cue(0x45);
      if (op == 0xD) {
        eng(c).sfx.trigger(0x39, 0, 0);           // Sfx::trigger — native (FUN_80074590 alt-cue path)
      } else {
        c->mem_w8(G + 5, 0x38);
        c->mem_w8(G + 6, 2);
      }
      break;
    }

    case 9:
    case 0xE: {
      if (f174 & 0x02) { c->mem_w8(0x800BF881u, f174); return; }
      n174 = (uint8_t)((f174 & 0xFE) | 0x02);
      c->mem_w8(G + 0x174u, n174);
      cue(0x47);
      if (op == 0xE) {
        eng(c).sfx.trigger(0x3A, 0, 0);
      } else {
        c->mem_w8(G + 5, 0x39);
        c->mem_w8(G + 6, 2);
      }
      break;
    }

    case 10: {
      n174 = f174 & 0xFC;
      c->mem_w8(G + 0x174u, n174);
      const uint8_t bf881 = c->mem_r8(0x800BF881u);
      if      (bf881 & 0x01) cue(0x46);
      else if (bf881 & 0x02) cue(0x48);
      break;
    }

    case 0xB: {
      if (f174 & 0x20) cue(0x4C);
      n174 = f174 & 0xDF;
      c->mem_w8(G + 0x174u, n174);
      if ((f174 & 0x10) == 0) {
        n0D = f0D & 0xED;
        c->mem_w8(G + 0x0Du, n0D);
      }
      break;
    }

    case 0xC: {
      c->mem_w8(G + 0x174u, 0); n174 = 0;
      c->mem_w8(G + 0x0Du,  0); n0D  = 0;
      const int16_t f17E = (int16_t)c->mem_r16(G + 0x17Eu);
      if ((uint16_t)f17E & 0x8000u) {
        const uint16_t masked = (uint16_t)(f17E & 0x7FFF);
        c->mem_w16(0x800BF89Eu, masked);
        c->mem_w16(G + 0x17Eu, masked);
      }
      if ((c->mem_r16(G + 0x17Eu) & 0x200u) != 0) {
        c->mem_w8 (G + 0x6Fu, 0);
        c->mem_w8 (0x800BF88Fu, 0);
        c->mem_w16(G + 0x17Eu, 0x10);
        c->mem_w16(0x800BF89Eu, 0x10);
      }
      break;
    }

    default: break;                                  // op >= 0xF: no-op
  }

  c->mem_w8(0x800BF881u, n174);                      // shared tail — mirror the post-mutation G+0x174
}

// Engine::fadeSequencer moved to ScreenFade::sequence (game/render/screen_fade.cpp).

// FIELD RUNNING sub-machine 0x80106b98 — native control flow + state bodies (decomp:
// scratch/decomp/game/80106b98.c). A 12-way switch on sm[0x4e]; the running states call the native
// ov_field_frame (0x80108b0c) and the heavy leaf callees rec_dispatch. NB the guest fall-throughs are
// faithful: case 2 -> 3, case 4 -> 1 (no break). sm[0x4e] >= 12 = no-op. This anchors the field frame
// natively; the leaf callees (object-placement FUN_80072a78 etc.) are the next descent.
// pc_faithful FIELD RUNNING sub-machine — exact mirror of ov_game_gen_80106B98 (12 states on
// sm[0x4e]). Guest frame (sp-24, ra@+20, r16@+16, live values) + every leaf dispatched at its
// RE'd jal site so callee spills byte-match core B. ov_game_func_80108B0C (the field per-frame
// update) runs the native owner Engine::fieldFrame with the gen's r31. Notable gen details the
// rebuilt fieldRun below got WRONG (kept there for pc_skip, fixed here): 0x1F800194 is a HALFWORD
// store of u16(0x800E7FEE) (not w32), the state-0 area check is mem_r16(0x800BF870)==21 (not
// r32==0x15), and state 6 writes 0x800BF870/71 as TWO byte stores of the swapped/masked halves.
void Engine::fieldRunFaithful() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);
  uint16_t s4e = c->mem_r16(sm + 0x4e);
  if (s4e < 12) switch (s4e) {
    case 0: {                                  // L_80106BDC — area object/pool init chain
      c->r[31] = 0x80106BE4u; rec_dispatch(c, 0x8007B18Cu);
      c->r[31] = 0x80106BECu; rec_dispatch(c, 0x800796DCu);
      c->r[31] = 0x80106BF4u; rec_dispatch(c, 0x800263E8u);
      c->r[31] = 0x80106BFCu; rec_dispatch(c, 0x80072A78u);
      c->r[31] = 0x80106C04u; rec_dispatch(c, 0x80075240u);
      c->r[31] = 0x80106C0Cu; rec_dispatch(c, 0x800783DCu);
      c->r[31] = 0x80106C14u; rec_dispatch(c, 0x80078610u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      c->mem_w8(sm + 0x6b, 0);
      if (c->mem_r8(0x800BF89Cu) == 2) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4e, 9);
      } else if (c->mem_r8(0x800BF870u) == 8) {
        c->r[31] = 0x80106C88u; rec_dispatch(c, 0x80114B90u);
      } else if (c->mem_r16(0x800BF870u) == 21) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4e, 11);
        break;
      }
      c->r[4] = c->mem_r8(0x800BF870u);
      c->r[31] = 0x80106C98u; rec_dispatch(c, 0x80074F24u);
      break;
    }
    case 2:                                    // L_80106DDC — G-state mutate, fall into 3
      c->r[4] = 0x800E7E80u; c->r[5] = 12;
      c->r[31] = 0x80106DECu; rec_dispatch(c, 0x80058304u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      /* fallthrough */
    case 3:                                    // L_80106E08 — settle audio, arm mode state
      c->r[31] = 0x80106E10u; rec_dispatch(c, 0x80074BC4u);
      c->r[4] = 0;
      if (c->mem_r8(0x800BF870u) == 8) {
        c->r[31] = 0x80106E2Cu; rec_dispatch(c, 0x80114B90u);
        c->r[4] = 0;
      }
      c->r[5] = 0; c->r[6] = 0;
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 2);
      c->mem_w16(sm + 0x4c, 0);
      c->mem_w16(sm + 0x4e, 0);
      c->r[31] = 0x80106E54u; rec_dispatch(c, 0x8005082Cu);
      break;
    case 4:                                    // L_80106CE0 — camera + mode-state re-arm, fall into 1
      c->r[31] = 0x80106CE8u; rec_dispatch(c, 0x8006C7C4u);
      c->r[31] = 0x80106CF0u; rec_dispatch(c, 0x800508A8u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      /* fallthrough */
    case 1: {                                  // L_80106D00 — the RUNNING field frame
      c->r[31] = 0x80106D08u;
      c->game->ffspan.begin(); eng(c).fieldFrame(); c->game->ffspan.end("fieldframe");
      if ((int8_t)c->mem_r8(0x800BF80Du) == 3) {
        if (c->mem_r8(0x800BF80Fu) != 0) break;
        c->r[31] = 0x80106D38u; rec_dispatch(c, 0x80074BC4u);
        int16_t ev = c->mem_r16s(0x800E7FEEu);
        uint16_t evu = c->mem_r16(0x800E7FEEu);
        if (ev == 0) {                         // L_80106F48 — s4e++
          sm = c->mem_r32(0x1f800138u);
          c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
          break;
        }
        c->mem_w8(0x800BF880u, 1);
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(0x1F800194u, evu);
        c->mem_w16(sm + 0x4e, 0);
        break;
      }
      uint8_t trig = c->mem_r8(0x800BF839u);
      if (trig == 0) break;
      if (c->mem_r8(0x800BF80Fu) != 0) break;
      if (trig == 8) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4a, 3);
        c->mem_w16(sm + 0x4c, 0);
        c->mem_w16(sm + 0x4e, 0);
        break;
      }
      if (c->mem_r8(0x1F800236u) >= 5) {
        c->r[4] = 0;
        c->r[31] = 0x80106DCCu; rec_dispatch(c, 0x80050894u);
      }
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 1);                // L_80106FAC join (r2 = 1)
      c->mem_w16(sm + 0x4c, 2);
      c->mem_w16(sm + 0x4e, 6);
      break;
    }
    case 5:                                    // L_80106CA0 — area-7 mode re-arm + field frame
      if (c->mem_r8(0x800BF870u) == 7) {
        c->r[31] = 0x80106CBCu; rec_dispatch(c, 0x801128BCu);
        c->r[31] = 0x80106CC4u; rec_dispatch(c, 0x800508A8u);
      }
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      c->r[31] = 0x80106CD8u;
      eng(c).fieldFrame();
      break;
    case 6: {                                  // L_80106E5C — zone-change settle + next-area select
      int16_t ev = c->mem_r16s(0x800E7FEEu);
      uint16_t evu = c->mem_r16(0x800E7FEEu);
      if (ev != 0) {
        c->mem_w8(0x800BF880u, 1);
        c->mem_w16(0x1F800194u, evu);
      }
      c->r[31] = 0x80106E88u; rec_dispatch(c, 0x80074BC4u);
      c->r[16] = 0x800BF808u;
      c->mem_w8(0x800BF870u, (uint8_t)((c->mem_r16(0x800BF83Au) >> 8) & 31u));
      c->mem_w8(0x800BF871u, (uint8_t)(c->mem_r8(0x800BF83Au) & 63u));
      uint8_t trig = c->mem_r8(0x800BF839u);
      if (trig == 7) {
        c->r[31] = 0x80106ECCu; rec_dispatch(c, 0x80114B90u);
        c->mem_w8(0x800BF839u, 3);
        trig = c->mem_r8(0x800BF839u);
      }
      if (trig != 3) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x48, 2);
        uint8_t b = c->mem_r8(0x1F800236u);
        c->mem_w16(sm + 0x4a, 5);
        c->mem_w16(sm + 0x4e, 0);
        c->mem_w16(sm + 0x4c, b);
      } else {                                 // L_80106F0C
        c->r[31] = 0x80106F14u; rec_dispatch(c, 0x8005245Cu);
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x48, 2);
        c->mem_w16(sm + 0x4a, 1);
        c->mem_w16(sm + 0x4c, 1);
        c->mem_w16(sm + 0x4e, 0);
      }
      break;
    }
    case 7:                                    // L_80106F38 — poll 0x80045580(1)
      c->r[4] = 1;
      c->r[31] = 0x80106F40u; rec_dispatch(c, 0x80045580u);
      if (c->r[2] == 0) break;
      sm = c->mem_r32(0x1f800138u);            // L_80106F48
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      break;
    case 8:                                    // L_80106F68 — wait done_flag, arm transition
      if (c->mem_r8(0x1F80019Bu) == 0) break;
      c->mem_w8(0x800BF89Cu, 4);
      c->mem_w8(0x1F800236u, 0);
      c->mem_w8(0x800BF839u, 3);
      c->mem_w16(0x800BF83Au, 0);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 1);                // L_80106FAC join (r2 = 1)
      c->mem_w16(sm + 0x4c, 2);
      c->mem_w16(sm + 0x4e, 6);
      break;
    case 9:                                    // L_80106FC4 — field frame + gate on pad bit 3
      c->r[31] = 0x80106FCCu;
      eng(c).fieldFrame();
      if (c->mem_r8(0x800BF89Cu) == 2 && (c->mem_r16(0x800E7E68u) & 8u) != 0) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->mem_w8(0x800BF809u, 1);
        c->mem_w8(sm + 0x6e, 31);
      }
      break;
    case 10: {                                 // L_80107020 — fade-out ramp then CD settle
      c->r[31] = 0x80107028u;
      c->r[16] = 0x1F800000u;
      eng(c).fieldFrame();
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6e) * (uint32_t)-8) & 0xFFu;
      c->r[4] = (u << 16) | (u << 8) | u; c->r[5] = 0; c->r[6] = 0;
      c->r[31] = 0x80107058u; rec_dispatch(c, 0x8007E9C8u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w8(sm + 0x6e, (uint8_t)(c->mem_r8(sm + 0x6e) - 1));
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(sm + 0x6e) == 0) {
        c->mem_w16(sm + 0x4e, 7);
        c->r[31] = 0x80107090u; rec_dispatch(c, 0x8001CF2Cu);
      }
      break;
    }
    case 11:                                   // L_80107098 — a0l fade sequencer on the BG node
      c->r[4] = 0x800E8008u;
      c->r[31] = 0x801070A4u; rec_dispatch(c, 0x8010957Cu);
      break;
    default: break;
  }
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

void Engine::fieldRun() { Core* c = core;
  // fieldRun can yield transitively (case 0's substrate init chain descends into loaders) — plain
  // call, not MV_CHECK: fieldRunFaithful is not byte-exact yet (12+ diffs at 0x801FE8xx / v0 / v1,
  // scratch/logs/mv_tdd.log), and MV_CHECK's synchronous compare is wrong across a yield boundary
  // regardless. SBS-gated; fix fieldRunFaithful and re-arm MV_CHECK once it's byte-exact and known
  // yield-free (or drop this call for a plain one permanently if it always yields).
  if (c->game && !c->game->pc_skip) { fieldRunFaithful(); return; }   // faithful: gen mirror
  uint32_t sm  = c->mem_r32(0x1f800138u);
  uint16_t s4e = c->mem_r16(sm + 0x4e);
  switch (s4e) {
    case 0:
      eng(c).pool.init();   // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x8007b18c)
      eng(c).pool.resetControlBlock();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800796dc)
      eng(c).pool.seedAreaObjects();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800263e8)
      eng(c).placement.placeAreaObjects();   // OWNED native (game/world/placement.cpp) — replaces rec_dispatch(0x80072a78)
      eng(c).pool.reset75240();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x80075240)
      eng(c).pool.setupViewScroll();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800783dc)
      eng(c).pool.finalViewInit();       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x80078610)
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      c->mem_w8(sm + 0x6b, 0);
      if (c->mem_r8(0x800bf89cu) == 2) { c->mem_w16(sm + 0x4e, 9); }
      else if (c->mem_r8(0x800bf870u) == 8) { d0(c, 0x80114b90u); }
      else if (c->mem_r32(0x800bf870u) == 0x15) { c->mem_w16(sm + 0x4e, 0xb); return; }
      eng(c).pool.selectStateIndex(c->mem_r8(0x800bf870u));   // OWNED native — replaces d1(0x80074f24, area)
      break;
    case 2:
      eng(c).gStateMutate(0x800E7E80u, 0xC);   // native — was rec_dispatch 0x80058304(G, 0xC)
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      /* fallthrough */
    case 3:
      eng(c).audioDispatch.settleField();     // native — was rec_dispatch 0x80074BC4
      if (c->mem_r8(0x800bf870u) == 8) d0(c, 0x80114b90u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4a, 2);
      c->mem_w16(sm + 0x4c, 0);
      c->mem_w16(sm + 0x4e, 0);
      eng(c).modeStateArm.arm();                  // native — was rec_dispatch 0x8005082C(0,0,0)
      break;
    case 4:
      d0(c, 0x8006c7c4u); eng(c).modeStateArm.armFromAreaTable();   // native — was rec_dispatch 0x800508A8
      c->mem_w16(c->mem_r32(0x1f800138u) + 0x4e, 1);
      /* fallthrough */
    case 1: {
      c->game->ffspan.begin(); eng(c).fieldFrame(); c->game->ffspan.end("fieldframe");   // native field per-frame update (0x80108b0c)
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf80du) == 3) {        // (signed byte) special mode 3
        if (c->mem_r8(0x800bf80fu) == 0) {
          eng(c).audioDispatch.settleField();     // native — was rec_dispatch 0x80074BC4
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
      if (c->mem_r8(0x800bf870u) == 7) { d0(c, 0x801128bcu); eng(c).modeStateArm.armFromAreaTable(); }
      c->mem_w16(c->mem_r32(0x1f800138u) + 0x4e, 1);
      eng(c).fieldFrame();
      break;
    case 6: {
      if (c->mem_r32(0x800e7feeu) != 0) { c->mem_w8(0x800bf880u, 1); c->mem_w32(0x1f800194u, c->mem_r32(0x800e7feeu)); }
      eng(c).audioDispatch.settleField();     // native — was rec_dispatch 0x80074BC4
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
      eng(c).fieldFrame();
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf89cu) == 2 && (c->mem_r32(0x800e7e68u) & 8) != 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->mem_w8(0x800bf809u, 1);
        c->mem_w8(sm + 0x6e, 0x1f);
      }
      break;
    case 10: {
      eng(c).fieldFrame();
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6e) * (uint32_t)-8) & 0xff;
      cfg_logf("fadesites", "[fadesite] fieldRun-case10 u=%02x sm6e=%u", u, c->mem_r8(sm+0x6e));
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, 0);   // = guest FUN_8007e9c8(color, 0, 4): area-transition subtractive fade-out ramp
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
      fade(c).sequence(0x800e8008u);   // OWNED native — replaces d1(0x8010957c, node) (a0l fade sequencer)
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
// pc_faithful mirror of ov_game_gen_80108BE4 (generated/ov_game_shard_1.c). Reduced twin of
// fieldFrameFaithful (0x80108B0C): same frame descent/spill/jal-site-ra shape, 9 gameplay-update
// calls (drops sceneStateStep 0x80050de4 and areaModeDispatch 0x8001cac0, which the full variant
// has and this one does not per gen), render orchestrator dispatched as mRender->frameX()
// (0x8003FA44) under the <2 gate, then submitPage810c/postRenderTick/areaSlots.updateTail tail —
// all with jal-site ras matching the gen's constants so any nested unowned leaf's own frame-spill
// stays byte-exact with core B.
void Engine::fieldFrameXFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->r[16] = 0x1F800000u;
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: reduced gameplay update
    c->r[31] = 0x80108C28u; FFS("ffx_59d28",    eng(c).frameStartTick());
    c->r[31] = 0x80108C30u; FFS("ffx_69b28",    eng(c).objectList.walkAux());
    c->r[31] = 0x80108C38u; FFS("ffx_26368",    eng(c).array8Dispatch.tick());
    c->r[31] = 0x80108C40u; FFS("ffx_7b04c",    eng(c).transitionState3.walkOnce());
    c->r[31] = 0x80108C48u; FFS("ffx_25588",    eng(c).sceneEventFifo());
    c->r[31] = 0x80108C50u; FFS("ffx_4fe84",    eng(c).sceneRenderListBuilder());
    c->r[31] = 0x80108C58u; FFS("ffx_disp26c88",eng(c).objectTable.dispatch());
    c->r[31] = 0x80108C60u; FFS("ffx_22a80",    eng(c).modePerFrameDispatch());
    c->r[31] = 0x80108C68u; FFS("ffx_6ec44",    CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update());
  }
  if (c->mem_r8(0x1f800136u) < 2) { c->r[31] = 0x80108C84u; rend(c)->frameX(); }   // 0x8003FA44 underneath
  c->r[31] = 0x80108C8Cu; FFS("ffx_submit810c", eng(c).submitPage810c());
  c->r[31] = 0x80108C94u; FFS("ffx_77d8c",      eng(c).postRenderTick());
  c->r[31] = 0x80108C9Cu; FFS("ffx_75a80",      eng(c).areaSlots.updateTail());
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

void Engine::fieldFrameX() { Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80108BE4u, fieldFrameXFaithful()); return; }   // faithful: gen mirror
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: reduced gameplay update
    eng(c).frameStartTick(); eng(c).objectList.walkAux(); d0(c, 0x80026368u); eng(c).transitionState3.walkOnce();   // 0x80059d28/0x80069b28/0x8007b04c NATIVE
    eng(c).sceneEventFifo(); eng(c).sceneRenderListBuilder(); eng(c).objectTable.dispatch(); eng(c).modePerFrameDispatch();   // 25588/4fe84/26c88/22a80 NATIVE
    CutsceneCamera(c, CutsceneCamera::CAM_OBJ).update();   // 0x8006ec44 NATIVE (CutsceneCamera::update)
  }
  if (c->mem_r8(0x1f800136u) < 2) rend(c)->frameX(); // 0x8003fa44 — NATIVE render orchestrator twin
  eng(c).submitPage810c();                      // render submit (page-1 dim-fade owned; other pages recomp)
  eng(c).postRenderTick();                   // 0x80077D8C NATIVE (was d0)
  eng(c).areaSlots.updateTail();                   // 0x80075a80 NATIVE
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
  // NO RNG draw here: this is the FUN_80044bd4(...,flag=1) call. In gen_func_80044BD4 the flag==1
  // branch jumps to the epilogue (`if (r19==1) goto L_80044CB8`) BEFORE func_8009A450 — zero RNG
  // draws. Only flag!=1 (see bd4Tail) draws the stamp. func_80051F14 (spawnPrim) draws no RNG either.
  SV_CHECK(c, 0x800452C0u, eng(c).sop.transitionAreaLoad(), rec_dispatch(c, 0x800452C0u));   // skip leg vs the slot-1 task body oracle (observable compare)
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
      fade(c).applyLeafCall(0xffffffu, 0);                 // = guest FUN_8007e9c8(0xffffff, 0, 4): full black held (one-frame state)
      native_area_load_bd4(c, c->mem_r8(0x800bf870u), 0);        // FUN_80044bd4(0x800452c0,bf870,0,1)
      return;
    case 1: {                                                    // FADE-OUT — subtractive ramp
      uint32_t u = (uint32_t)c->mem_r8(sm + 0x6b) & 0x1f;
      fade(c).applyLeafCall((u << 19) | (u << 11) | (u << 3), 0);
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
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, 1);
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
        eng(c).audioDispatch.zoneTransitionSetup(9);         // native, FUN_8001D71C
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
  if (c->game && !c->game->pc_skip) { fieldTransitionFaithful(); return; }   // yield-capable (spawnAndWait): plain call, no MV_CHECK
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  switch (s4c) {
    case 0: case 9:
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 0); c->mem_w16(sm + 0x4e, 0);
      break;
    case 1: case 2: case 3: case 4: eng(c).transitionMain(); break;
    case 5: case 6:                 eng(c).transitionD3c();  break;
    case 7:                         eng(c).transitionE20();  break;
    case 8:                         eng(c).transitionF3c();  break;
    default: break;
  }
}

// Faithful mirror of ov_game_gen_80108A60. Own frame: sp-=24, r31 spill @sp+16. Dispatches
// sm[0x4c] exactly as the gen jump table (0/9=reset, 1-4=main, 5/6=d3c, 7=e20, 8=f3c); every
// worker call gets its own jal-site ra constant set immediately before the call.
void Engine::fieldTransitionFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c < 10) switch (s4c) {
    case 0: case 9:
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 0); c->mem_w16(sm + 0x4e, 0);
      break;
    case 1: case 2: case 3: case 4:
      c->r[31] = 0x80108ACCu; eng(c).transitionMainFaithful(); break;
    case 5: case 6:
      c->r[31] = 0x80108AECu; eng(c).transitionD3cFaithful(); break;
    case 7:
      c->r[31] = 0x80108ADCu; eng(c).transitionE20Faithful(); break;
    case 8:
      c->r[31] = 0x80108AFCu; eng(c).transitionF3cFaithful(); break;
    default: break;
  }
  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// Faithful mirror of ov_game_gen_80107AFC. Frame: sp-=24, r31 spill @sp+20, r16 spill @sp+16
// (r16 is a dead s-reg for our control flow -- spilled/restored verbatim for byte-fidelity only).
void Engine::transitionMainFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st < 5) switch (st) {
    case 0:
      c->mem_w8(0x1f800234u, 1);
      c->r[31] = 0x80107B50u; rec_dispatch(c, 0x8007A810u);
      c->r[31] = 0x80107B58u; rec_dispatch(c, 0x800798F8u);
      c->r[31] = 0x80107B60u; rec_dispatch(c, 0x8007B0F0u);
      c->r[31] = 0x80107B68u; rec_dispatch(c, 0x801079ACu);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w8(sm + 0x6b, 0x1f);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      fade(c).applyLeafCall(0xffffffu, 0);          // = guest FUN_8007e9c8(0xffffff,0,4); jal-site was 0x80107B98u
      sm = c->mem_r32(0x1f800138u);
      c->r[4] = 0x800452C0u;
      c->r[5] = c->mem_r8(0x800bf870u);
      c->r[6] = 0;
      c->r[7] = 1;
      c->r[31] = 0x80107BB4u;
      rec_dispatch(c, 0x80044BD4u);
      goto epilogue;
    case 1: {
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = (uint32_t)c->mem_r8(sm + 0x6b) & 0x1fu;
      fade(c).applyLeafCall((u << 19) | (u << 11) | (u << 3), 0);   // jal-site was 0x80107BECu
      sm = c->mem_r32(0x1f800138u);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6b) - 1);
      c->mem_w8(sm + 0x6b, v);
      if (v == 0) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      }
      break;
    }
    case 2:
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x1f80019bu) != 0) {
        c->mem_w8(sm + 0x6b, 0x1f);
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      }
      break;
    case 3: {
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6b) * (uint32_t)-8) & 0xffu;
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, 1);          // jal-site was 0x80107C98u
      sm = c->mem_r32(0x1f800138u);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6b) - 1);
      c->mem_w8(sm + 0x6b, v);
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(sm + 0x6b) == 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->r[4] = 0;
        c->r[31] = 0x80107CDCu;
        rec_dispatch(c, 0x80050894u);
      }
      break;
    }
    case 4:
      c->r[31] = 0x80107D0Cu; rec_dispatch(c, 0x80074E48u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
      goto epilogue;
    default: break;
  }
  // per-frame update tail (states 1/2/3 fall through here; state 0/4 skip it via goto epilogue)
  c->r[31] = 0x80107CE4u; rec_dispatch(c, 0x80059C60u);
  c->r[31] = 0x80107CECu; rec_dispatch(c, 0x8006EF38u);
  c->r[31] = 0x80107CF4u; rec_dispatch(c, 0x8007B008u);
  c->r[31] = 0x80107CFCu; rec_dispatch(c, 0x8003FA1Cu);
epilogue:
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// Faithful mirror of ov_game_gen_80107D3C. Frame: sp-=24, r16 spill @sp+16, r31 spill @sp+20.
void Engine::transitionD3cFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 20, c->r[31]);
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st == 1) {
    c->r[31] = 0x80107DC8u; rec_dispatch(c, 0x8003FB84u);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
  } else if (st == 0) {
    c->mem_w8(0x1f800234u, 1);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
    c->r[4] = 0x800452C0u;
    c->r[5] = c->mem_r8(0x800bf870u);
    c->r[6] = 0;
    c->r[7] = 1;
    c->r[31] = 0x80107DB8u;
    rec_dispatch(c, 0x80044BD4u);
  } else if (st == 2) {
    if (c->mem_r8(0x1f80019bu) == 0) {
      c->r[31] = 0x80107E10u; rec_dispatch(c, 0x8003EA88u);
    } else {
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
    }
  }
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// Faithful mirror of ov_game_gen_80107E20. Frame: sp-=32, r16@sp+16/r17@sp+20/r18@sp+24/r31@sp+28.
void Engine::transitionE20Faithful() { Core* c = core;
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 20, c->r[17]);
  c->mem_w32(sp + 24, c->r[18]);
  c->mem_w32(sp + 28, c->r[31]);
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st == 1) {
    c->r[31] = 0x80107ECCu; rec_dispatch(c, 0x8003E264u);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
  } else if (st == 0) {
    c->r[4] = 9; c->r[31] = 0x80107E80u; rec_dispatch(c, 0x80074BF8u);
    c->r[31] = 0x80107E88u; rec_dispatch(c, 0x8003E264u);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
    c->mem_w8(0x1f800234u, 1);
    c->r[4] = 0x800452C0u;
    c->r[5] = c->mem_r8(0x800bf870u);
    c->r[6] = 0;
    c->r[7] = 1;
    c->r[31] = 0x80107EBCu;
    rec_dispatch(c, 0x80044BD4u);
  } else if (st == 2) {
    c->r[31] = 0x80107EF0u; rec_dispatch(c, 0x8003E894u);
    if (c->mem_r8(0x1f80019bu) != 0) {
      c->r[31] = 0x80107F0Cu; rec_dispatch(c, 0x80074E48u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
      c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
    }
  }
  c->r[31] = c->mem_r32(sp + 28);
  c->r[18] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 32;
}

// Faithful mirror of ov_game_gen_80107F3C. Frame: sp-=24, r31 spill @sp+16 (no s-reg spills).
// State 0 uses the TEXGROUP loader (0x80044f58) instead of the area loader (0x800452c0) -- both
// go through FUN_80044BD4 (rec_dispatch -> PcScheduler::spawnAndWait), NOT a direct sync call.
void Engine::transitionF3cFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x4e);
  if (st < 7) switch (st) {
    case 0: {
      c->r[4] = 9; c->r[31] = 0x80107F84u; rec_dispatch(c, 0x80074BF8u);
      c->r[31] = 0x80107F8Cu; rec_dispatch(c, 0x8003E264u);
      c->mem_w8(0x1f800234u, 1);
      sm = c->mem_r32(0x1f800138u);
      // gen: (int16_t) SIGNED 16-bit read of 0x1f800240, +26, &0xff -- NOT a 32-bit read.
      int32_t texArg = c->mem_r16s(0x1f800240u) + 26;
      c->r[4] = 0x80044F58u;
      c->r[5] = (uint32_t)texArg & 0xFFu;
      c->r[6] = 0;
      c->r[7] = 1;
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      c->r[31] = 0x80107FD0u;
      rec_dispatch(c, 0x80044BD4u);
      break;
    }
    case 1: case 5:
      c->r[31] = 0x80108098u; rec_dispatch(c, 0x8003E264u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      break;
    case 2:
      c->r[31] = 0x80107FE0u; rec_dispatch(c, 0x8003E894u);
      if (c->mem_r8(0x1f80019bu) != 0) {
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->r[31] = 0x80108010u; rec_dispatch(c, 0x80074E48u);
        c->r[31] = 0x80108018u; eng(c).audioDispatch.zoneTransitionSetup(9);   // native, FUN_8001D71C
        c->r[31] = 0x80108020u; rec_dispatch(c, 0x8003FB94u);
      }
      break;
    case 3:
      c->r[31] = 0x80108030u; rec_dispatch(c, 0x8003EBE0u);
      if (c->r[2] == 0) goto epilogue;    // still running -> stay
      c->r[31] = 0x80108040u; rec_dispatch(c, 0x8001CF2Cu);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      break;
    case 4:
      c->r[4] = 8; c->r[31] = 0x80108050u; rec_dispatch(c, 0x80074BF8u);
      c->r[31] = 0x80108058u; rec_dispatch(c, 0x8003E264u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
      c->r[4] = 0x800452C0u;
      c->r[5] = c->mem_r8(0x800bf870u);
      c->r[6] = 0;
      c->r[7] = 1;
      c->r[31] = 0x80108088u;
      rec_dispatch(c, 0x80044BD4u);
      break;
    case 6:
      c->r[31] = 0x801080C0u; rec_dispatch(c, 0x8003E894u);
      if (c->mem_r8(0x1f80019bu) != 0) {
        c->r[31] = 0x801080DCu; rec_dispatch(c, 0x80074E48u);
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x48, 2); c->mem_w16(sm + 0x4a, 1);
        c->mem_w16(sm + 0x4c, 1); c->mem_w16(sm + 0x4e, 0);
      }
      break;
    default: break;
  }
epilogue:
  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// FIELD RUNNING sub-machine VARIANT 0x801070b4 (sm[0x4c]==3, the mid-transition running handler reached
// when a door/edge sets up an area change). A switch on sm[0x4e]: state 0 = init (scene reset + input
// reset) then fall into state 1; state 1 = run ov_field_frame_x and check the mode-3 / area-change exit
// conditions to hand back to the normal running handler (sm[0x4c]=2); state 2 = bump to 1. Faithful to
// the disasm (hand-decompiled from the field overlay).
// pc_faithful mirror of ov_game_gen_801070B4 (mid-transition running sub-machine, sm[0x4c]==3).
// Guest frame descent (24, ra spilled at sp+16) + r31 set to the exact gen jal-site constant
// before every dispatch/native-call boundary, so every callee's own frame push lands with the
// right ra at the right (correctly-descended) address. Store shape/branch conditions are
// unchanged from the existing pc_skip body -- only frame/ra discipline was missing.
void Engine::fieldRunXFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);

  uint32_t sm  = c->mem_r32(0x1f800138u);
  uint16_t s4e = c->mem_r16(sm + 0x4e);

  if (s4e >= 2) {                                  // L_801070EC
    if (s4e == 2) c->mem_w16(sm + 0x4e, 1);         // L_8010721C: re-arm to running
  } else {
    if (s4e == 0) {                                 // L_80107100: init
      c->r[31] = 0x80107108u;
      c->mem_w16(sm + 0x4e, 1);
      rec_dispatch(c, 0x8006c77cu);
      c->r[4] = 0; c->r[5] = 0; c->r[6] = 0;
      c->r[31] = 0x80107118u;
      rec_dispatch(c, 0x8005082cu);                 // input reset
      // fall through to state 1 (L_80107118)
    }

    c->r[31] = 0x80107120u;
    eng(c).fieldFrameX();                        // ov_game_func_80108BE4 -- native owner

    if (c->mem_r8(0x800bf80du) == 3) {               // mode-3 exit (0x80107138)
      if (c->mem_r8(0x800bf80fu) == 0) {
        c->r[31] = 0x80107150u;
        eng(c).audioDispatch.settleField();       // native owner -- was rec_dispatch 0x80074BC4
        sm = c->mem_r32(0x1f800138u);
        c->mem_w16(sm + 0x4c, 2);                    // back to normal running handler
        int16_t  e_s = c->mem_r16s(0x800e7feeu);
        uint16_t e_u = c->mem_r16(0x800e7feeu);
        if (e_s != 0) {
          c->mem_w8(0x800bf880u, 1);
          c->mem_w16(0x1f800194u, e_u);
          c->mem_w16(sm + 0x4e, 0);
        } else {
          c->mem_w16(sm + 0x4e, 2);
        }
      }
    } else {                                        // L_80107194: area-change request via bf839
      uint8_t bf839 = c->mem_r8(0x800bf839u);
      if (bf839 != 0 && c->mem_r8(0x800bf80fu) == 0) {
        if (bf839 == 8) {                            // 0x801071bc
          sm = c->mem_r32(0x1f800138u);
          c->mem_w16(sm + 0x4a, 1);
          c->mem_w16(sm + 0x4c, 2);
          c->mem_w16(sm + 0x4e, 3);
        } else {
          if (c->mem_r8(0x1f800236u) >= 5) {         // 0x801071f0
            c->r[31] = 0x801071f8u;
            c->r[4] = 0;
            rec_dispatch(c, 0x80050894u);
          }
          sm = c->mem_r32(0x1f800138u);
          c->mem_w16(sm + 0x4a, 1);
          c->mem_w16(sm + 0x4c, 2);
          c->mem_w16(sm + 0x4e, 6);
        }
      }
    }
  }

  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

void Engine::fieldRunX() { Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x801070B4u, fieldRunXFaithful()); return; }   // faithful: gen mirror, r31/frame discipline
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
  eng(c).fieldFrameX();                           // 0x80108be4 per-frame (state 1, 0x80107118)
  if (c->mem_r8(0x800bf80du) == 3) {             // mode-3 exit (0x80107138)
    if (c->mem_r8(0x800bf80fu) != 0) return;
    eng(c).audioDispatch.settleField();     // native — was rec_dispatch 0x80074BC4
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

// pc_faithful walkable-field area machine — mirror of ov_game_gen_801088D8 (7 states on sm[0x4c]).
// Same recipe as Sop::fieldModeFaithful: guest frame + jal-site ras; every un-owned leaf is the
// substrate dispatch at its RE'd jal site; owned states run the native Engine methods. Case 0
// dispatches the REAL 0x80044BD4 spawn-and-wait (override registry -> PcScheduler::spawnAndWait) of
// the area-DATA loader 0x800452C0 (Asset::areaDataLoadAsTask, task-1 fiber) — the stage fiber
// parks inside the wait loop and falls through to case 1 on completion, organically reproducing
// the substrate's multi-tick cadence (retires the Slip #3 two-tick defer + Slip #5 RNG
// compensation from the faithful path).
void Engine::submode1Faithful() { Core* c = core;
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c < 7) switch (s4c) {
    case 0:                                   // L_80108918
      c->r[31] = 0x80108920u;
      rec_dispatch(c, 0x8005245Cu);           // sound/CD setup leaf
      c->r[4] = 0x800452C0u;                  // area-DATA loader (task-1 body)
      c->r[5] = c->mem_r8(0x800BF870u);       // area index
      c->r[6] = 0;
      c->r[7] = 2;
      c->r[31] = 0x8010893Cu;
      rec_dispatch(c, 0x80044BD4u);           // spawn-and-wait — parks the stage fiber
      /* fallthrough */
    case 1: {                                 // L_8010893C
      c->mem_w8(0x1f800234u, 0);
      uint8_t area = c->mem_r8(0x800BF870u);
      uint8_t next = c->mem_r8(0x80108F60u + area);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4c, next);
      break;
    }
    case 2: c->r[31] = 0x80108974u; c->game->ffspan.begin(); eng(c).fieldRun(); c->game->ffspan.end("fieldrun"); break;
    case 3: c->r[31] = 0x80108984u; eng(c).fieldRunX(); break;
    case 4: c->r[31] = 0x80108994u; rec_dispatch(c, 0x80107230u); break;
    case 5: c->r[31] = 0x801089A4u; rec_dispatch(c, 0x8010766Cu); break;
    case 6: c->r[31] = 0x801089B4u; rec_dispatch(c, 0x80107790u); break;
    default: break;
  }
  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// Skip: one tick, no per-frame yield cost. The two branches deliberately do not converge:
// substrate parity demands cadence match, live gameplay does not.
bool Engine::submode1Case0Skip() { Core* c = core;
  rec_dispatch(c, 0x8005245cu);
  // FUN_80044bd4's a3==2 TAIL — shared helper PcScheduler::bd4Tail (game/core/pc_scheduler.cpp;
  // docs/findings/scene.md "pc_skip FUN_80044BD4-collapse INCOMPLETENESS class", bug #58 — this
  // was the HIGHEST-reachability site, missing the WHOLE tail: the current task's RNG stamp at
  // +0x56 AND the flag==2 wait-counter bump + FUN_8007fd54 dispatch). Ordering is load-bearing
  // (matches demo.cpp/gen): the tail must run BEFORE SV_CHECK(...transitionAreaLoad()) below —
  // running it after would observe the done_flag already ==1 (the load we just finished) and take
  // the guest's OTHER branch, skipping the counter-bump + FUN_8007fd54 dispatch entirely.
  c->game->pcSched.bd4Tail(c->mem_r32(0x1f800138u), /*flag=*/2);
  SV_CHECK(c, 0x800452C0u, sop.transitionAreaLoad(), rec_dispatch(c, 0x800452C0u));   // observable gate vs the 0x800452C0 oracle
  // pc_skip counter-bump ([[pc-skip-frame-counter-bump]]): substrate would consume 2 field-
  // frame ticks in this case-0 body (Slip #3 in docs/findings/sbs.md — FUN_80044BD4 yields
  // between load and fall-through), each bumping 0x1F80017C + 0x800BF878 via fieldFrame.
  // pc_skip collapses to 1 tick and would bump those counters only once — so downstream
  // phase-gated code (e.g. FUN_8004B374's obj+0xD gate on `0x1F80017C & 0x1F`) samples
  // out-of-phase compared to recomp. Bump the counters here to inject the extra tick's
  // worth of counter progression the substrate would have made.
  c->mem_w16(0x1F80017Cu, (uint16_t)(c->mem_r16(0x1F80017Cu) + 1));
  c->mem_w32(0x800BF878u, c->mem_r32(0x800BF878u) + 1);
  return true;
}

void Engine::submode1() { Core* c = core;
  if (c->game && !c->game->pc_skip) { submode1Faithful(); return; }   // faithful: gen mirror on the stage fiber
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c <= 1)
    cfg_logf("stage", "submode1 case %u: bf870=%u nexttab[bf870]=%u",
             s4c, c->mem_r8(0x800bf870u), c->mem_r8(0x80108f60u + c->mem_r8(0x800bf870u)));
  switch (s4c) {
    case 0:
      if (!submode1Case0Skip())
        return;
      /* fallthrough */
    case 1: {
      c->mem_w8(0x1f800234u, 0);
      uint8_t area = c->mem_r8(0x800bf870u);
      uint8_t next = c->mem_r8(0x80108f60u + area);
      c->mem_w16(sm + 0x4c, next);
      break;
    }
    case 2: c->game->ffspan.begin(); eng(c).fieldRun(); c->game->ffspan.end("fieldrun"); break;   // field RUNNING sub-machine (sm[0x4e]) — native
    case 3: eng(c).fieldRunX(); break;              // mid-transition running sub-machine 0x801070b4 — native
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
  // fade(c).applyLeafCall / set. Still-recomp SMs' fade calls don't reach the class yet — those
  // are the top-down port frontier for closing coverage.
  fade(c).frameStart();
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s48 = c->mem_r16(sm + 0x48);
  if (s48 == 2) {
    uint16_t s4a = c->mem_r16(sm + 0x4a);
    // OWNED running sub-modes: 0 = SOP-intro (SOP overlay must be loaded); 1 = field area machine
    // (0x801088d8, the walkable field — its load is sync via native_transition_area_load, its running
    // states are yield-free). Other sub-modes (2..5, the area-machine variants) aren't owned yet.
    if (s4a == 0) {
      if (c->mem_r32(0x80109450u) != 0x3C021F80u) { cfg_logf("gframe", "ret0 s48=2 s4a=0 SOP-not-loaded ov=%08X sm@%08X", c->mem_r32(0x80109450u), sm); return 0; } // SOP not loaded -> cooperative
    } else if (s4a != 1) {
      cfg_logf("gframe", "ret0 s48=2 s4a=%u unowned-submode sm@%08X", s4a, sm); return 0; // unowned running sub-mode
    }
    c->r[31] = 0x8010645Cu;                    // guest loop jal site (L_80106454)
    eng(c).s48_2_frame();
  } else if (s48 == 0) {
    c->r[31] = 0x8010643Cu;                    // guest loop jal site (L_80106434)
    c->game->ffspan.begin(); eng(c).s48_0(); c->game->ffspan.end("s48_0");
  } else if (s48 == 1) {
    c->r[31] = 0x8010644Cu;                    // guest loop jal site (L_80106444)
    c->game->ffspan.begin(); eng(c).s48_1(); c->game->ffspan.end("s48_1");
  } else {
    cfg_logf("gframe", "ret0 unknown s48=%u sm@%08X", s48, sm); return 0; // unknown top state -> cooperative
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
  cfg_logf("stage", "ov_game_stage_prologue run, sm[0x48]=%u", init48);
}

// pc_faithful GAME stage body (fiber task; see engine.h). Byte shape: ov_game_gen_8010637C +
// ov_game_gen_801063F4. stagePrologue leaves the guest frame descended and s0/s1/s2 holding the
// loop constants, exactly like the substrate (the frame stays live for the whole stage). frame()
// bumps the 0x1F800198 counter itself when it handles the state; the unowned-state fallback
// dispatches the guest sm[0x48] handler (deep yields park the fiber) and bumps the counter here.
void Engine::stageBodyFaithful() { Core* c = core;
  stagePrologue();
  for (;;) {
    c->game->ffspan.begin();
    int handled = frame();
    c->game->ffspan.end("gameframe");
    if (!handled) {
      uint32_t sm = c->mem_r32(0x1f800138u);
      uint16_t s48 = c->mem_r16(sm + 0x48);
      if (s48 == 1)      { c->r[31] = 0x8010644Cu; rec_dispatch(c, 0x80108720u); }
      else if (s48 == 0) { c->r[31] = 0x8010643Cu; rec_dispatch(c, 0x801086E0u); }
      else if (s48 == 2) { c->r[31] = 0x8010645Cu; rec_dispatch(c, 0x80108784u); }
      c->mem_w16(0x1f800198u, (uint16_t)(c->mem_r16(0x1f800198u) + 1));
    }
    c->r[4] = 1;
    c->r[31] = 0x80106470u;
    rec_dispatch(c, 0x80051F80u);          // loop-tail yield (override registry -> yieldPrim)
  }
}

// OLD guest-loop entry (prologue + coro-redirect into the guest loop 0x801063F4). SUPERSEDED by the
// native per-frame path (game_native in PcScheduler::step calls eng(c).stagePrologue +
// eng(c).frame). Retained as a reference / fallback; not on the live path.
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
// Engine::areaModeDispatchFaithful — byte-exact mirror of gen_func_8001CAC0. The literal recompiler
// translation (generated/shard_2.c:348-363) always descends a 24-byte frame and spills the incoming
// ra to sp+16 BEFORE testing the mode-index bound (MIPS delay-slot store executes unconditionally).
// For a valid index (10 of 22 slots: 0,4,5,6,7,10,11,13,15,21) the resident table at 0x80010000
// holds the address of a small STUB (0x8001CB00, CB10, CB20, CB30, CB40, CB50, CB60, CB70, CB80,
// CB90) — never the overlay handler directly. Each stub sets r31 to its OWN jal-site return address
// (stub_addr+8) before rec_dispatching the overlay handler (generated/shard_{3,4,5,6,7,1}.c), then
// falls into the shared epilogue gen_func_8001CB98 (r31 = mem_r32(sp+16); sp += 24). The other 12
// in-range indices (1,2,3,8,9,12,14,16,17,18,19,20) have a table entry that IS 0x8001CB98 directly
// (no stub, no overlay call) — same for out-of-range idx>=22, which gen_func_8001CAC0 special-cases
// to call func_8001CB98 without even reading the table. All non-dispatching paths still perform the
// full sp-24/spill/restore round trip, so the transient guest-stack word at (orig_sp-8) is always
// overwritten with the caller's ra, exactly like gen — this matters for SBS byte-compare even though
// the net register effect is a no-op.
void Engine::areaModeDispatchFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);                 // unconditional prologue spill (gen's delay-slot store)
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx >= 22) { rec_dispatch(c, 0x8001CB98u); return; }   // out-of-range: straight to shared epilogue
  static const uint32_t handler[22] = {
    /* 0*/ 0x8011534Cu, /* 1*/ 0,           /* 2*/ 0,           /* 3*/ 0,
    /* 4*/ 0x8013EE84u, /* 5*/ 0x80136CDCu, /* 6*/ 0x8014189Cu, /* 7*/ 0x8012F6ECu,
    /* 8*/ 0,           /* 9*/ 0,           /*10*/ 0x801140D0u, /*11*/ 0x80113F94u,
    /*12*/ 0,           /*13*/ 0x80116980u, /*14*/ 0,           /*15*/ 0x80116560u,
    /*16*/ 0,           /*17*/ 0,           /*18*/ 0,           /*19*/ 0,
    /*20*/ 0,           /*21*/ 0x8010B918u,
  };
  static const uint32_t stubRa[22] = {          // resident stub's own jal-site ra (stub_addr + 8)
    /* 0*/ 0x8001CB08u, 0,0,0,
    /* 4*/ 0x8001CB18u, /* 5*/ 0x8001CB28u, /* 6*/ 0x8001CB38u, /* 7*/ 0x8001CB48u,
    0,0,
    /*10*/ 0x8001CB58u, /*11*/ 0x8001CB68u,
    0,
    /*13*/ 0x8001CB78u,
    0,
    /*15*/ 0x8001CB88u,
    0,0,0,0,0,
    /*21*/ 0x8001CB98u,
  };
  uint32_t target = handler[idx];
  if (!target) { rec_dispatch(c, 0x8001CB98u); return; }   // table entry IS the epilogue addr for no-op modes
  c->r[4] = 0x800ED018u;                          // a0 carried through from gen's early (dead-looking but live) load
  c->r[31] = stubRa[idx];                         // mode-specific stub jal-site ra, NOT the field-frame caller's ra
  rec_dispatch(c, target);
  rec_dispatch(c, 0x8001CB98u);                   // shared epilogue: r31 = mem_r32(sp+16); sp += 24
}

void Engine::areaModeDispatch() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x8001CAC0u, areaModeDispatchFaithful()); return; }   // faithful: gen+stub+epilogue mirror
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
// Engine::sceneStateStepFaithful — byte-exact mirror of gen_func_80050DE4 (guest 0x80050DE4).
// Frame: sp -= 24; spill r16@sp+16 (unconditional, before r16 is repurposed as the SCENE_STATE
// pointer); spill ra@sp+20 (unconditional -- gen's branch-delay slot on the very first test, runs
// on every path). Both restored at the single shared epilogue below (mirrors gen's L_80051118,
// reached from every exit including the phase-out-of-range no-op). Every rec_dispatch is preceded
// by r31 = the gen's per-index jal-site return address (required so a handler's own ra-spill lands
// on the same value CoreB produces). The INIT block ALWAYS writes phase=1 to SCENE_STATE on the
// way out -- including the idx==9 null-handler and the idx>=22 out-of-range case (gen's shared
// L_80050F90 -> L_80050F94 fallback); the RUN block never writes SCENE_STATE, in any case.
//
// v0 (r2) end-state (bug #TDD-80050DE4): the gen body is a literal MIPS register-reuse translation
// that leaves v0 holding whichever scratch value the LAST instruction on the taken path happened to
// write, even on paths that dispatch nothing -- there is no dedicated "return value" in the source.
// The original mirror never touched c->r[2] at all, so v0 leaked whatever the PREVIOUS rec_dispatch
// call (elsewhere) had left there (observed: 0x80150000) instead of the gen's own constant.  Traced
// every exit in generated/shard_7.c (gen_func_80050DE4, lines 6729-6929):
//   - INIT (phase==0), idx<22 dispatched, idx==9 null, AND idx>=22 out-of-range ALL converge on
//     L_80050F90 ("r2 = r0+1") -> L_80050F94 ("mem_w8(SCENE_STATE, r2)") -> epilogue: v0 == 1,
//     unconditionally, even when a handler was dispatched (its own return value in v0 is clobbered
//     by the post-call `r2 = 1` before falling into L_80050F94).
//   - RUN (phase==1), idx<22 with a real target: v0 == the dispatched handler's own return value
//     (falls out naturally from rec_dispatch reusing c->r[2] as its ABI return slot -- no fixup
//     needed here).
//   - RUN, idx==9 (null slot): gen loads the RUN table entry (0x80051118, the epilogue label used
//     as a table sentinel) into v0 and falls straight to the epilogue with NO call and NO further
//     r2 write -- v0 == 0x80051118 verbatim.
//   - RUN, idx>=22 (out-of-range): gen's bounds check leaves v0 == 32769<<16 == 0x80010000 in the
//     branch-delay slot before jumping to the epilogue.
//   - phase < 0 or >= 2 (no-op): gen's "(phase<2)" boolean is 0 for this range and that same v0==0
//     is what triggers the jump straight to the epilogue -- v0 == 0.
void Engine::sceneStateStepFaithful() { Core* c = core;
  static constexpr uint32_t SCENE_STATE = 0x800F2418u;   // 32783<<16 + 9240
  static constexpr uint32_t MODE_IDX    = 0x800BF870u;   // 32780<<16 - 1936

  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->r[16] = SCENE_STATE;
  uint8_t phase = c->mem_r8(SCENE_STATE);
  c->mem_w32(sp + 20, c->r[31]);              // unconditional -- gen's branch-delay slot

  if (phase == 1) {
    // RUN table (@0x80015A98). idx==9 and idx>=22 both fall straight to the epilogue, no write.
    uint8_t idx = c->mem_r8(MODE_IDX);
    if (idx < 22) {
      static const uint32_t run[22] = {
        0x8013EFA8u,0x8012EE14u,0x80123E1Cu,0x8010E964u,0x80116CCCu,0x80136488u,
        0x8013D6D8u,0x8012E2F4u,0x8012AB30u,0,           0x80110A14u,0x80113770u,
        0x801144D4u,0x80113D68u,0x80114A78u,0x80115DECu,0x8010C9FCu,0x8010BCDCu,
        0x8010C160u,0x8010B140u,0x80116B9Cu,0x8010B200u,
      };
      static const uint32_t runRa[22] = {
        0x80050FD8u,0x80050FE8u,0x80050FF8u,0x80051008u,0x80051018u,0x80051028u,
        0x80051038u,0x80051048u,0x80051058u,0x80051118u,0x80051068u,0x80051078u,
        0x80051088u,0x80051098u,0x800510A8u,0x800510B8u,0x800510C8u,0x800510D8u,
        0x800510E8u,0x800510F8u,0x80051108u,0x80051118u,
      };
      uint32_t target = run[idx];
      if (target) {
        c->r[31] = runRa[idx];
        c->r[4] = c->r[16];
        rec_dispatch(c, target);             // v0 = handler's own return value (natural)
      } else {
        c->r[2] = 0x80051118u;               // idx==9: gen's RUN-table literal, no call made
      }
    } else {
      c->r[2] = 0x80010000u;                 // idx>=22: gen's out-of-range sentinel (32769<<16)
    }
  } else if (phase == 0) {
    // INIT table (@0x80015A40). idx==9 (null handler) and idx>=22 (out-of-range) both skip the
    // dispatch but STILL fall through to the phase=1 write -- do not early-return on idx>=22.
    uint8_t idx = c->mem_r8(MODE_IDX);
    if (idx < 22) {
      static const uint32_t init[22] = {
        0x8013FB4Cu,0x8012F89Cu,0x80124678u,0x8010F174u,0x801175D0u,0x80136CB0u,
        0x8013E144u,0x8012EB50u,0x8012B3E8u,0,           0x80111238u,0x80113F68u,
        0x80114CCCu,0x80114560u,0x80115270u,0x80116534u,0x8010D21Cu,0x8010C4FCu,
        0x8010C980u,0x8010B960u,0x801173A8u,0x8010B8ECu,
      };
      static const uint32_t initRa[22] = {
        0x80050E50u,0x80050E60u,0x80050E70u,0x80050E80u,0x80050E90u,0x80050EA0u,
        0x80050EB0u,0x80050EC0u,0x80050ED0u,0,           0x80050EE0u,0x80050EF0u,
        0x80050F00u,0x80050F10u,0x80050F20u,0x80050F30u,0x80050F40u,0x80050F50u,
        0x80050F60u,0x80050F70u,0x80050F80u,0x80050F90u,
      };
      uint32_t target = init[idx];
      if (target) {
        c->r[31] = initRa[idx];
        c->r[4] = c->r[16];
        rec_dispatch(c, target);             // v0 clobbered again below, matching gen's post-call reset
      }
    }
    c->r[2] = 1;                 // gen: v0 == 1 on EVERY INIT exit (dispatched, idx==9, idx>=22 alike)
    c->mem_w8(SCENE_STATE, (uint8_t)c->r[2]);   // ALWAYS -- idx in range, idx==9, and idx>=22 all reach this
  } else {
    // phase < 0 or >= 2 (unsigned byte 2..255) -> no-op, straight to epilogue.
    c->r[2] = 0;                 // gen: the "(phase<2)" boolean (false here) IS v0 at this exit
  }

  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] = sp + 24;
}

// Engine::sceneStateStep — the SCENE-INIT / SCENE-RUN state machine at guest 0x80050DE4. See engine.h.
void Engine::sceneStateStep() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80050DE4u, sceneStateStepFaithful()); return; }   // faithful: gen mirror
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
#include "ai/behaviors.h"                         // Behaviors::areaSeasidePerframe (FUN_80113C5C)

// Engine::modePerFrameDispatchFaithful — pc_faithful mirror of gen_func_80022A80 (disasm-verified
// 0x80022A80-0x80022AC8; the C body in generated/shard_6.c also contains ~1.3KB of unrelated dead
// code from an unlabeled neighboring function up to 0x8002313C — not part of this dispatcher, see
// findings). Guest frame: sp-=24, ra spilled to sp+16 UNCONDITIONALLY (delay-slot store — fires on
// both the idx==3 early-return path and the dispatch path), restored + sp+=24 on every exit. Jal-
// site ra (0x80022AB8u, the instruction after `jalr v0`) is set immediately before the indirect
// call so any callee that spills ra to ITS OWN guest frame gets the correct value. No null-target
// guard: the gen jalr fires unconditionally once the table read completes.
void Engine::modePerFrameDispatchFaithful() {
  Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[31]);          // delay-slot store: happens on BOTH branch outcomes
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx != 3) {
    uint32_t target = c->mem_r32(0x8009D1D4u + (uint32_t)idx * 4u);
    c->r[31] = 0x80022AB8u;               // jal-site ra for the indirect call (jalr v0)
    // Faithful path: dispatch the SUBSTRATE mode fn unconditionally. Behaviors::areaSeasidePerframe
    // is a REBUILD (result-equivalent, not byte-exact) — routing it here is what failed the strict
    // gate (2026-07-08: 18+ stack/hi-lo diffs). It stays the pc_skip shortcut below; the faithful
    // conversion of 0x80113C5C is a behaviors-wave item (lib-fallback recipe meanwhile).
    rec_dispatch(c, target);              // NO null check — matches gen, fails fast on target==0
  }
  c->r[31] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

void Engine::modePerFrameDispatch() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80022A80u, modePerFrameDispatchFaithful()); return; }   // faithful: gen mirror
  uint8_t idx = c->mem_r8(0x800BF870u);
  if (idx == 3) return;
  uint32_t target = c->mem_r32(0x8009D1D4u + (uint32_t)idx * 4u);
  if (!target) return;
  // Area-0 (seaside) per-frame update is native — this is Tomba's seaside per-frame tick.
  if (target == 0x80113C5Cu) { Behaviors::areaSeasidePerframe(c); return; }
  rec_dispatch(c, target);
}

// Engine::postRenderTick — 3-state fx-trigger + countdown on byte 0x800BF842 at guest 0x80077D8C.
// Faithful to the disasm: low 7 bits select (== 1: fire FX 41, set b42 = 0x87), (== 2: fire FX 42,
// clear b42), other/nonzero: decrement b42. Zero = no-op. FX 41/42 leaf FUN_80074590 stays substrate.
void Engine::postRenderTick() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80077D8Cu, postRenderTickFaithful()); return; }
  uint8_t b = c->mem_r8(0x800BF842u);
  if (b == 0) return;
  uint8_t low = (uint8_t)(b & 0x7F);
  if (low == 1) {
    eng(c).sfx.trigger(41, 2, -65);      // FUN_80074590 (native; pitchBend -65)
    c->mem_w8(0x800BF842u, 0x87);
    return;
  }
  if (low == 2) {
    eng(c).sfx.trigger(42, 2, -65);      // FUN_80074590 (native; pitchBend -65)
    c->mem_w8(0x800BF842u, 0);
    return;
  }
  c->mem_w8(0x800BF842u, (uint8_t)(b - 1));
}

// Engine::postRenderTickFaithful -- byte-exact mirror of gen_func_80077D8C. Descends the gen's own
// 24-byte guest frame, spills s0 (r16, the 0x800BF808 struct base) at sp+16 and the caller's ra at
// sp+20 exactly like the reference-mirror shape (Engine::submode1Faithful), sets r31 to the gen's
// jal-site constant before the FX-trigger leaf, and dispatches that leaf via rec_dispatch so it runs
// the literal substrate body gen_func_80074590 (no override-registry entry exists for 0x80074590, so
// this is guaranteed byte-identical -- including gen_func_80074590's own ra-to-stack spill, which the
// native Sfx::trigger port does not reproduce and must not be used here). v0/v1 (r2/r3) are dead to
// every known caller but gen_func_80077D8C leaves them holding whatever value each branch happened to
// compute (b, b&0x7f, 135, 0, 0x800C0000, 0x800BF808, b-1) instead of restoring/clearing them -- the
// mirror must reproduce that leftover ABI end-state per-branch or MV_CHECK's v0/v1 compare fails.
void Engine::postRenderTickFaithful() { Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);            // spill s0
  c->mem_w32(sp + 20, c->r[31]);            // spill caller ra
  c->r[16] = 0x800BF808u;                   // s0 = struct base

  uint8_t b = c->mem_r8(c->r[16] + 58);     // == 0x800BF842
  c->r[2] = b;                              // v0 = loaded byte (gen never re-clears it on the
                                             // early-exit path -- the mirror must reproduce that
                                             // leftover ABI value, not leave v0/v1 untouched from
                                             // the caller)
  if (b != 0) {
    uint8_t low = (uint8_t)(b & 0x7F);
    c->r[3] = low;                          // v1 = andi r3,r2,0x7f (gen's unconditional delay slot)
    if (low == 1) {
      c->r[4] = 41; c->r[5] = 2; c->r[6] = (uint32_t)-65;
      c->r[31] = 0x80077DDCu;               // gen's jal-site ra for this call
      rec_dispatch(c, 0x80074590u);         // -> gen_func_80074590 (substrate, un-owned leaf)
      c->r[2] = 135;                        // v0 = 135 (gen literal, r3/v1 stays == 1)
      c->mem_w8(c->r[16] + 58, (uint8_t)c->r[2]);
    } else if (low == 2) {
      c->r[3] = 0x800C0000u;                // v1 = lui r3,0x800c (gen's delay-slot clobber of low)
      c->r[4] = 42; c->r[5] = 2; c->r[6] = (uint32_t)-65;
      c->r[31] = 0x80077DF8u;               // gen's jal-site ra for this call
      rec_dispatch(c, 0x80074590u);
      c->r[2] = 0;                          // v0 = r0
      c->mem_w8(c->r[16] + 58, 0);
    } else {
      c->r[3] = 0x800C0000u - 2040;         // v1 = 0x800BF808 (gen: r3 = 0x800c0000 + -2040)
      uint8_t v = c->mem_r8(c->r[3] + 58);  // same address as b (0x800BF842); re-read per gen
      c->r[2] = (uint32_t)v - 1;            // v0 = byte - 1 (full 32-bit, not just the stored 8 bits)
      c->mem_w8(c->r[3] + 58, (uint8_t)c->r[2]);
    }
  } else {
    c->r[3] = 0;                            // v1 = 0 & 127 (gen's delay slot on the b==0 branch)
  }

  c->r[31] = c->mem_r32(sp + 20);           // restore ra
  c->r[16] = c->mem_r32(sp + 16);           // restore s0
  c->r[29] += 24;
}

// Engine::frameStartTick — per-frame prologue at guest 0x80059D28 (FIRST call in ov_field_frame's
// gameplay-update block). Faithful port of the disasm; see engine.h for the step-by-step contract.
// Callees kept substrate: the mode-keyed overlay handler (branches at (e)), FUN_8005950C default,
// and the rand LFSR advance at 0x8009A450 (the top recdep hit — 86 calls/frame — a future target).
void Engine::frameStartTick() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { frameStartTickFaithful(); return; }   // yields — SBS-gated: mode-keyed
  // dispatch (d) reaches dynamically-loaded overlay handlers (0x8010F63C/0x80109024/0x80112220/
  // 0x8010F654) that can scheduler_yield; MV_CHECK's synchronous strictCheck only supports yield-free
  // mirrors (strictCheck aborts while inCheck), so this fork is a plain call, proven by SBS full
  // (core A vs core B), not MV_CHECK.
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
  (void)rngOf(c).next();
}

// Engine::frameStartTickFaithful — byte-exact mirror of gen_func_80059D28 (guest 0x80059D28), the
// FIRST call in ov_field_frame's gameplay-update block. Descends the guest frame (sp-=24), spills
// incoming r31/r16 at +20/+16 per the gen prologue, and sets the gen's per-case jal-site r31 constant
// at every dispatch/call boundary so callee guest-stack ra spills (e.g. func_8005950C's spill at its
// own sp+28) byte-match core B. Callees kept substrate: the mode-keyed overlay handler (targets at
// 0x8010F63C/0x80109024/0x80112220/0x8010F654 — dynamically-loaded overlay code, not in generated/),
// FUN_8005950C default, and the rand LFSR advance at 0x8009A450 — dispatched to the real gen leaf
// (not the native Rng class) so its v0/v1/hi/lo side effects, which are still live at this function's
// own return (rand is the last statement before the epilogue), byte-match core B; gen_func_8009A450
// never touches r31, so the r31 set before it is precautionary discipline only.
void Engine::frameStartTickFaithful() {
  Core* c = core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);              // spill CALLER's r16 (not yet reassigned)
  static constexpr uint32_t G = 0x800E7E80u;   // master G block base
  c->r[16] = G;                                // gen: delay-slot `r16 = r3 + 32384` — must land in
                                                // the guest register itself (not just the local `G`
                                                // constant), since callees (FUN_8005950C / the mode
                                                // handlers below) are callee-saved on s0/r16 and spill
                                                // it verbatim onto their own frames.

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
  // (c) per-frame stamp++ (unconditional — gen's branch-delay-slot store).
  c->mem_w8(0x1F800247u, (uint8_t)(c->mem_r8(0x1F800247u) + 1u));

  // (d) if 0x800BF841 == 0: mode-keyed per-frame handler dispatch (each case sets its own gen
  // jal-site r31 constant before the call), then clear 0x1F800230.
  if (c->mem_r8(0x800BF841u) == 0) {
    uint8_t mode = c->mem_r8(0x800BF870u);
    c->r[4] = G;
    switch (mode) {
      case 2:  c->r[31] = 0x80059DF8u; rec_dispatch(c, 0x8010F63Cu); break;
      case 3:  c->r[31] = 0x80059E08u; rec_dispatch(c, 0x80109024u); break;
      case 7:  c->r[31] = 0x80059E18u; rec_dispatch(c, 0x80112220u); break;
      case 20: c->r[31] = 0x80059E28u; rec_dispatch(c, 0x8010F654u); break;
      default: c->r[31] = 0x80059E38u; rec_dispatch(c, 0x8005950Cu); break;
    }
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
  // (h) advance rand LFSR. gen_func_8009A450 is the LAST statement in the guest body before the
  // epilogue, so its v0/v1/hi/lo side effects (masked value in v0, the 0x41C64E6D multiplier
  // loaded into v1, MULT's hi/lo product) are still live in those registers when this function
  // returns — the strict ABI-register gate compares them unconditionally. rngOf(c).next() only
  // performs the guest-memory seed update (mem_r32/mem_w32 on 0x80105EE8, matching the shared
  // stream with substrate callers) but is a plain C++ call with no register-level side effects,
  // so it silently dropped v0/v1/hi/lo. Dispatch the real gen leaf instead so those registers
  // land bit-for-bit like core B (gen_func_8009A450 is a pure no-yield leaf — a plain call/
  // rec_dispatch is safe here, same as the mode-dispatch calls above).
  c->r[31] = 0x80059EC8u;
  rec_dispatch(c, 0x8009A450u);

  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 24;
}

// Register the GAME-stage area-init overrides when this just-loaded overlay is GAME.BIN at the stage base.
// Detect by the fixed entry + handler signatures (START.BIN/DEMO.BIN are smaller and hold stale bytes at
// these addresses, so they never match). Called from the overlay-load scan (submit.cpp); registered
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


// FUN_800450bc: load the stage overlay (if any) and point the task's restart entry at the stage code.
static void native_load_overlay(Core* c, uint32_t taskfields, uint32_t stage) {
  uint32_t entry;
  if (stage == 3) {
    entry = c->mem_r32(STAGE_ENTRY_TBL + 3 * 4);     // stage 3 is already resident: no overlay load
  } else {
    uint32_t lba  = c->mem_r32(STAGE_FILE_TBL + stage * 8);
    uint32_t size = c->mem_r32(STAGE_FILE_TBL + stage * 8 + 4);
    c->game->cd.loadFile(0x80106228, lba, size);    // = FUN_8001db8c / cd_loadfile
    // FUN_80051f80(1) cooperative yield is a no-op with the native scheduler — skipped.
    entry = c->mem_r32(STAGE_ENTRY_TBL + stage * 4);
  }
  c->mem_w32(taskfields, entry);                     // task+0xc = restart PC
  c->mem_w32(taskfields + 4, entry);                 // task+0x10
}

// FUN_80052078: switch task 0 to the given stage (load overlay + reset the display/BIOS bits).
// Public entry: called by DEMO's LEAVE-to-GAME substate (demo.cpp s5), by task0Bootstrap after
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
  if (!disc_find_file(&c->game->disc, "\\BIN\\START.BIN", &lba, &size)) {
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
  eng(c).asset.preloadTexgroup(0, 0);
  eng(c).asset.preloadStage1();
  eng(c).startStage(1);
  scheduler_yield(c);
}

// Stage-0 START.BIN entry (0x8010649C): own the file-table BUILDER PC-native. Original substrate is
// a sequence of CdSearchFile loops that resolve ~36 disc filenames and record each {LBA,size}; we
// bypass libcd entirely and walk the native ISO9660 directory with disc_find_file, then seed the
// preload SM at state 0 so stage0Advance can pace preloadTexgroup / preloadStage1 / startStage(1)
// across subsequent ticks. Reference: overlay disas of 0x8010649c..0x80106728 (later-211).
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

static bool resolve_via_iso9660(Core* c, uint32_t name_ptr, uint32_t* lba, uint32_t* size) {
  char name[80]; read_guest_str(c, name_ptr, name, sizeof name);
  if (disc_find_file(&c->game->disc, name, lba, size)) return true;
  fprintf(stderr, "[start.bin] not found: %s\n", name);
  return false;
}

// libcd directory cache — native port of FUN_8008BBE8 (CdNewMedia) + FUN_8008BF50 (CdCacheFile),
// the two libcd helpers that populate the game's in-memory directory cache. The pc_skip path uses
// native ISO9660 (resolve_via_iso9660 above) for file lookup — fast, doesn't touch libcd — but the
// substrate on B populates the libcd cache as a side effect of CdSearchFile. To keep A byte-
// identical, reproduce the writes here from the same ISO9660 sectors, using native disc_read_sector.
//
// Guest state populated:
//   0x800AC2D4  u32           currently cached dir index (0 = root loaded, else path-table 1-based idx)
//   0x80102D68  128 × 44 B    path-table cache (index, parent, extent, name[32])
//   0x80102768   64 × 24 B    file-entry cache for the currently-cached dir (MSF, size, name)
//
// RE: Ghidra headless decomp of FUN_8008BBE8 / FUN_8008BF50 (SCEI library range 0x80080000..0x8009E000).
// ISO9660 field offsets are the standard PVD / path-table-L / dir-record layout.
static constexpr uint32_t kGuestCurCachedDir   = 0x800AC2D4u;
static constexpr uint32_t kGuestMediaCookieDst = 0x800AC2D8u;   // = DAT_800ABFD0 after CdSearchFile
static constexpr uint32_t kGuestMediaCookieSrc = 0x800ABFD0u;   // libcd media-generation counter
static constexpr uint32_t kGuestPathTableCache = 0x80102D68u;   // stride 44 B, 128 slots
static constexpr uint32_t kGuestFileEntryCache = 0x80102768u;   // stride 24 B, 64 slots
static constexpr uint32_t kGuestPathExtentBase = 0x80102D70u;   // = cache + 8 (extent field of slot 0)
static constexpr uint32_t kGuestSectorScratch  = 0x80104368u;   // 2 KB scratch buffer libcd reads into

// Read one 2048 B disc sector into a local buffer AND into the guest-RAM scratch at 0x80104368,
// mirroring the substrate's per-sector read into DAT_80104368. Returns false on read failure.
static bool cdlibcd_read_into_scratch(Core* c, uint32_t lba, uint8_t* out) {
  if (!disc_read_sector(&c->game->disc, lba, out)) return false;
  for (uint32_t i = 0; i < 2048; i++) c->mem_w8(kGuestSectorScratch + i, out[i]);
  return true;
}

// FUN_8008BBE8 CdNewMedia: read PVD at LBA 16, follow to the L-path-table, walk entries into the
// path-table cache. Sets 0x800AC2D4 = 0.
static void cdlibcd_new_media(Core* c) {
  uint8_t sec[2048];
  if (!cdlibcd_read_into_scratch(c, 16, sec)) return;
  // Verify "CD001" magic at PVD offset 1 (bytes 1..5).
  if (sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' || sec[4] != '0' || sec[5] != '1') return;
  // PVD offset 140 (0x8C) = L-path-table LBA (u32 LE).
  const uint32_t pt_lba = (uint32_t)sec[0x8C] | ((uint32_t)sec[0x8D] << 8) |
                          ((uint32_t)sec[0x8E] << 16) | ((uint32_t)sec[0x8F] << 24);
  if (!cdlibcd_read_into_scratch(c, pt_lba, sec)) return;

  // Walk path-table entries: byte name_len + byte xa_len + u32 extent LE + u16 parent LE +
  // name[name_len] + pad-to-even.
  uint32_t off = 0;
  int i = 0;
  while (off + 8 <= 2048 && sec[off] != 0 && i < 128) {
    const uint8_t  name_len = sec[off + 0];
    const uint32_t extent   = (uint32_t)sec[off + 2] | ((uint32_t)sec[off + 3] << 8) |
                              ((uint32_t)sec[off + 4] << 16) | ((uint32_t)sec[off + 5] << 24);
    const uint8_t  parent   = sec[off + 6];   // only low byte kept (max 128 dirs)

    const uint32_t slot = kGuestPathTableCache + (uint32_t)i * 44u;
    c->mem_w32(slot + 0, (uint32_t)(i + 1));   // 1-based dir index
    c->mem_w32(slot + 4, (uint32_t)parent);    // parent idx as u32 (only low byte meaningful)
    c->mem_w32(slot + 8, extent);              // dir extent LBA
    for (uint32_t j = 0; j < name_len; j++) c->mem_w8(slot + 12 + j, sec[off + 8 + j]);
    c->mem_w8(slot + 12 + name_len, 0);         // null-terminate

    off += 8u + (uint32_t)name_len + ((uint32_t)name_len & 1u);
    i++;
  }
  // Sentinel: next slot's parent field cleared (loop exit marker for the original FUN_8008BBE8).
  if (i < 128) c->mem_w32(kGuestPathTableCache + (uint32_t)i * 44u + 4, 0);

  c->mem_w32(kGuestCurCachedDir, 0);
}

// FUN_8008BF50 CdCacheFile(dir_idx): read the directory sector at path_cache[dir_idx-1].extent,
// walk directory records, populate the file-entry cache. Sets 0x800AC2D4 = dir_idx. Idempotent when
// dir_idx already matches the cached one.
static void cdlibcd_cache_file(Core* c, uint32_t dir_idx) {
  if (dir_idx == c->mem_r32(kGuestCurCachedDir)) return;
  const uint32_t dir_lba = c->mem_r32(kGuestPathExtentBase + (dir_idx - 1u) * 44u);
  uint8_t sec[2048];
  if (!cdlibcd_read_into_scratch(c, dir_lba, sec)) {
    c->mem_w32(kGuestCurCachedDir, dir_idx); return;
  }

  uint32_t off = 0;
  int i = 0;
  while (off < 2048 && sec[off] != 0 && i < 64) {
    const uint32_t rec_len = sec[off + 0];
    const uint32_t extent  = (uint32_t)sec[off + 2]  | ((uint32_t)sec[off + 3]  << 8) |
                             ((uint32_t)sec[off + 4] << 16) | ((uint32_t)sec[off + 5] << 24);
    const uint32_t size    = (uint32_t)sec[off + 10] | ((uint32_t)sec[off + 11] << 8) |
                             ((uint32_t)sec[off + 12] << 16) | ((uint32_t)sec[off + 13] << 24);
    const uint8_t  fn_len  = sec[off + 32];

    // FUN_8008A00C: LBA -> BCD MSF (sector = LBA + 150, then split into min/sec/frame at 75 fps).
    const int t = (int)extent + 150;
    const int frame = t % 75, rem = t / 75, ssec = rem % 60, min = rem / 60;

    const uint32_t slot = kGuestFileEntryCache + (uint32_t)i * 24u;
    c->mem_w8(slot + 0, (uint8_t)((min  % 10) + ((min  / 10) << 4)));
    c->mem_w8(slot + 1, (uint8_t)((ssec % 10) + ((ssec / 10) << 4)));
    c->mem_w8(slot + 2, (uint8_t)((frame % 10) + ((frame / 10) << 4)));
    c->mem_w32(slot + 4, size);
    if (i == 0) {
      // "." — DAT_8001C528 = 0x0000002E as u32 written at slot+8.
      c->mem_w32(slot + 8, 0x0000002Eu);
    } else if (i == 1) {
      // ".." — DAT_8001C52C = 0x00002E2E at slot+8, DAT_8001C52E = 0x0000 (u16) at slot+10.
      c->mem_w32(slot + 8, 0x00002E2Eu);
      c->mem_w16(slot + 10, 0);
    } else {
      for (uint32_t j = 0; j < fn_len; j++) c->mem_w8(slot + 8 + j, sec[off + 33 + j]);
      c->mem_w8(slot + 8 + fn_len, 0);
    }

    off += rec_len;
    i++;
  }
  // Sentinel: null the next name slot's first byte (original: (u8)(&DAT_80102770 + iVar3*0xc) = 0).
  if (i < 64) c->mem_w8(kGuestFileEntryCache + (uint32_t)i * 24u + 8, 0);

  c->mem_w32(kGuestCurCachedDir, dir_idx);
}


// Baked pixel data address inside START.BIN (16x1 pixel strip loaded to VRAM(944,256)).
static constexpr uint32_t kStartBinLoadImageSrc = 0x80106878u;

// Advance stage-0 SM + preload boot texgroup. pc_skip only — the pc_faithful path splits these
// writes across their real substrate call sites (task-1 spawn for preload, FUN_80044BD4 tail for
// sm[0x48]:=1, FUN_80044BD4 body for RNG stamp).
static void startBinCommonAdvance(Core* c, Asset& asset, Rng& rng) {
  uint32_t task = c->mem_r32(CUR_TASK);
  c->mem_w16(task + 0x4a, 0);
  asset.preloadTexgroup(0, 0);
  c->mem_w16(task + 0x56, (uint16_t)rng.next());
  c->mem_w16(task + 0x48, 1);
}

// One-line dispatchers — the pc_skip/faithful fork lives at method granularity so each path
// reads top-to-bottom without inline branching. User rule (2026-07-04): no code blocks inside
// if (pc_skip) / if (!pc_skip) — call one of two named methods.
void Engine::startBinStage() {
  if (core->game->pc_skip) startBinStageSkip();
  else                     startBinStageFaithful();
}

// ── STARTBINSTAGE — pc_skip (default ./run.sh) ──────────────────────────────────────────
// Collapsed native shortcut. Native VRAM upload (bypasses libgs LoadImage), native ISO9660 file
// lookup (bypasses libcd — libcd's dir/file cache is written from the same ISO9660 sectors so
// any post-boot rec_dispatch into CdSearchFile short-circuits its newmedia branch), inline
// preloadTexgroup (bypasses task-1 spawn), task-1 slot closed with state=0 as if it ran-and-
// completed inline. task-0's native_task_spawn mirrors the substrate's task-slot writes (BIOS
// TCB placeholder at +0x04, entry_pc at +0x0C, caller gp at +0x10) — gp value 0x800BE0D4 is the
// substrate's caller-of-FUN_80051F14 gp captured at the STAGE-0 call site.
void Engine::startBinStageSkip() {
  Core* c = core;
  c->mem_w16(0x1F800008u + 0, 944);
  c->mem_w16(0x1F800008u + 2, 256);
  c->mem_w16(0x1F800008u + 4, 16);
  c->mem_w16(0x1F800008u + 6, 1);
  asset.uploadImage(0x1F800008u, kStartBinLoadImageSrc);

  cdlibcd_new_media(c);
  c->mem_w32(kGuestMediaCookieDst, c->mem_r32(kGuestMediaCookieSrc));
  cdlibcd_cache_file(c, 2);
  cdlibcd_cache_file(c, 3);

  for (const auto& L : kStartBinLoops) {
    for (uint32_t i = 0; i < L.count; i++) {
      uint32_t lba = 0, size = 0;
      resolve_via_iso9660(c, c->mem_r32(L.name_table + i * 4), &lba, &size);
      c->mem_w32(L.dest_table + i * 8,     lba);
      c->mem_w32(L.dest_table + i * 8 + 4, size);
    }
  }
  for (uint32_t i = 0; i < 3; i++) {
    const auto& X = kStartBinXa[i];
    uint32_t lba = 0, size = 0;
    resolve_via_iso9660(c, X.name_ptr, &lba, &size);
    c->mem_w32(X.lba_dest, lba);
  }

  startBinCommonAdvance(c, asset, rngOf(c));   // inline preload + rng stamp + sm[0x48]:=1

  const uint32_t saved_gp = c->r[28];
  c->r[28] = 0x800BE0D4u;
  native_task_spawn(c, 1, 0x80044F58u);      // slot writes only; task-1 body never runs
  c->r[28] = saved_gp;
  c->mem_w16(0x801FE070u, 0);                // close task-1 (already ran inline)

  fprintf(stderr, "[start.bin] file table built (pc/iso9660); preload SM stepped across ticks\n");
}

// ── STARTBINSTAGE — pc_faithful (PSXPORT_PC_SKIP=0) ─────────────────────────────────────
// The COMPLETE ov_start_gen_8010649C task body as a native port, run on a PcScheduler fiber
// (runStage0FiberStanza) — faithful-execution model, docs/faithful-execution.md. It executes on
// the guest machine state: locals in the real guest frame (sp descent 456; LoadImage RECT at
// sp+400, per-iteration CdlFILE records at sp+16+i*24, XA CdlFILE at sp+408), s-registers
// maintained live through the loops (nested callee prologues spill them into compared task-0
// stack bytes), and suspension inside PcScheduler primitives — so the per-frame slice cadence
// and every wait-loop stack byte match core B by construction. Guest identity constants (call-
// site ra values, table addresses) are the RE of the START.BIN overlay body (generated/
// ov_start_shard_0.c + scratch/decomp/overlay.c FUN_801064f0).
//
// The body never returns: the sm==3 arm dispatches the still-substrate stage-swap leaf
// FUN_80052078, which parks the fiber forever; the stanza cancels it once the entry rewrites.
// NOTE fiber teardown unwinds via longjmp — no destructibles may live across a yield here.
namespace {
struct StartBinFaithfulLoop {
  uint32_t names, dest, count;
  uint32_t raSearch, raPosToInt, raPrintf;   // guest call-site ra constants (part of the port's identity)
};
constexpr StartBinFaithfulLoop kStartBinFaithfulLoops[3] = {
  { 0x80106808u, 0x800BE118u, 25, 0x8010651Cu, 0x80106540u, 0x80106530u },  // \BIN overlays
  { 0x8010686Cu, 0x800BE1E0u,  3, 0x80106594u, 0x801065B8u, 0x801065A8u },  // \BIN stage .BINs
  { 0x801067F4u, 0x800BE0F0u,  5, 0x8010660Cu, 0x80106630u, 0x80106620u },  // \CD data files
};
struct StartBinFaithfulXa {
  uint32_t name, dest;
  uint32_t raSearch, raPosToInt, raPrintf;
};
constexpr StartBinFaithfulXa kStartBinFaithfulXa[3] = {
  { 0x8010646Cu, 0x1F80021Cu, 0x80106670u, 0x80106694u, 0x80106684u },      // \CD\VOICE.XA
  { 0x8010647Cu, 0x1F800220u, 0x801066B4u, 0x801066D8u, 0x801066C8u },      // \CD\DEMO.XA
  { 0x8010648Cu, 0x1F800224u, 0x801066F8u, 0x8010671Cu, 0x8010670Cu },      // \CD\BGM.XA
};
constexpr uint32_t kNotFoundFmt = 0x80106454u;   // "Not found file name %s"
}  // namespace

void Engine::startBinStageFaithful() {
  Core* c = core;
  PcScheduler& sched = c->game->pcSched;

  // Prologue: sp -= 456; spill ra/s4..s0 at 452..432 (live values — at fiber start these are the
  // frame-loop registers + the 0xDEAD0000 entry ra, same as core B's fiber).
  c->r[29] -= 456;
  const uint32_t S = c->r[29];
  c->mem_w32(S + 452, c->r[31]);
  c->mem_w32(S + 448, c->r[20]);
  c->mem_w32(S + 444, c->r[19]);
  c->mem_w32(S + 440, c->r[18]);
  c->mem_w32(S + 436, c->r[17]);
  c->mem_w32(S + 432, c->r[16]);

  // LoadImage RECT {944,256,16,1} is a frame local at sp+400 (NOT scratchpad — that's the
  // pc_skip shortcut's location).
  c->mem_w16(S + 400, 944);
  c->mem_w16(S + 402, 256);
  c->mem_w16(S + 404, 16);
  c->mem_w16(S + 406, 1);
  c->r[4] = S + 400; c->r[5] = kStartBinLoadImageSrc; c->r[31] = 0x801064E8u;
  rec_dispatch(c, 0x80081218u);                       // libgs LoadImage
  c->r[4] = 0; c->r[31] = 0x801064F0u;
  rec_dispatch(c, 0x80080F6Cu);                       // DrawSync(0)

  // Three CdSearchFile loops. Loop registers live in the guest s-regs (r16=CdlFILE ptr,
  // r17=name-table ptr, r18=dest ptr, r19=index, r20=i*24) because CdSearchFile's prologue
  // spills them into its frame — compared task-0 stack bytes.
  LibcdNative libcd(c);
  for (const auto& L : kStartBinFaithfulLoops) {
    c->r[19] = 0; c->r[20] = 0; c->r[18] = L.dest; c->r[17] = L.names;
    do {
      c->r[16] = S + 16 + c->r[20];                   // per-iteration CdlFILE record
      if (libcd.searchFile(c->r[16], c->mem_r32(c->r[17]), L.raSearch)) {
        c->mem_w32(c->r[18],     libcd.posToInt(c->r[16], L.raPosToInt));
        c->mem_w32(c->r[18] + 4, c->mem_r32(c->r[16] + 4));
      } else {
        c->r[4] = kNotFoundFmt; c->r[5] = c->mem_r32(c->r[17]); c->r[31] = L.raPrintf;
        rec_dispatch(c, 0x8009A730u);
      }
      c->r[18] += 8; c->r[17] += 4; c->r[19] += 1; c->r[20] += 24;
    } while ((int32_t)c->r[19] < (int32_t)L.count);
  }

  // Three XA singletons — CdlFILE at sp+408, LBA to the scratchpad slots.
  for (const auto& X : kStartBinFaithfulXa) {
    c->r[16] = S + 408;
    c->r[17] = X.name;
    if (libcd.searchFile(c->r[16], c->r[17], X.raSearch)) {
      c->mem_w32(X.dest, libcd.posToInt(c->r[16], X.raPosToInt));
    } else {
      c->r[4] = kNotFoundFmt; c->r[5] = c->r[17]; c->r[31] = X.raPrintf;
      rec_dispatch(c, 0x8009A730u);
    }
  }
  fprintf(stderr, "[start.bin] pc_faithful file table built via libcd (fiber body)\n");

  // SM loop (L_80106744): the state constants live in the s-regs — spawnAndWait's prologue
  // spills them (s0=3, s1=2, s2=1, s3=r19 left at 5 by loop 3's exit — the RE'd live values).
  c->r[16] = 3; c->r[17] = 2; c->r[18] = 1;
  uint32_t task = c->mem_r32(CUR_TASK);
  c->mem_w16(task + 0x48, 0);
  c->mem_w16(task + 0x4A, 0);
  for (;;) {
    task = c->mem_r32(CUR_TASK);
    switch (c->mem_r16(task + 0x48)) {
      case 0:                                        // L_8010678C: spawn task-1 texgroup preload
        c->mem_w16(task + 0x48, 1);
        c->mem_w16(task + 0x4A, 0);
        c->r[4] = 0x80044F58u; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0; c->r[31] = 0x801067A8u;
        sched.spawnAndWait(0x80044F58u, 0, 0, 0);    // parks here once per wait frame
        break;
      case 1:                                        // L_801067B0: spawn task-1 stage-1 preload
        c->mem_w16(task + 0x48, 2);
        c->r[4] = 0x8004514Cu; c->r[5] = 1; c->r[6] = 1; c->r[7] = 0; c->r[31] = 0x801067CCu;
        sched.spawnAndWait(0x8004514Cu, 1, 1, 0);
        break;
      case 2:
        c->mem_w16(task + 0x48, 3);
        break;
      case 3:                                        // L_801067DC: stage swap to DEMO
        c->r[4] = 1; c->r[31] = 0x801067E4u;
        rec_dispatch(c, 0x80052078u);                // parks the fiber; stanza cancels on entry rewrite
        break;
    }
    c->r[4] = 1; c->r[31] = 0x801067ECu;             // L_801067E4 trailing per-iteration yield
    sched.yieldPrim(1);
  }
}

// ── STAGE0ADVANCE — pc_skip cadence ─────────────────────────────────────────────────────
// 5 steps mirroring substrate's per-iteration yield loop as compact non-yielding native calls
// (preload was done inline in startBinStageSkip; task-1 body never runs). RNG stand-in at step 1
// matches substrate's FUN_80044BD4 rng advance at f2 without actually spawning a task.
int Engine::stage0AdvanceSkip(uint8_t& step) {
  Core* c = core;
  uint32_t task = c->mem_r32(CUR_TASK);
  // FRAME ALIGNMENT (USER 2026-07-10, compare-mode only — SBS MODE=skip): startBinStageSkip()
  // already built the WHOLE file table and stamped this task's +0x48 preload-SM byte to 1 in ONE
  // native call/frame (see its header comment above); the oracle recomp substrate on SBS's sibling
  // core spreads the equivalent CD-directory work across many frames (docs/config.md "Boot-preload
  // TRANSIENT regions" — "native's startBinStage collapsing many substrate ticks into ~5 ticks…
  // while substrate B spreads the same work across ~10+ ticks"). Rather than let A race ahead of B
  // at the same lockstep frame index (the old settled-divergence-tolerance shape), hold at step 0
  // (no state advance — task+0x48 stays at its already-1 value, nothing downstream observes step
  // itself) until the sibling core's OWN task+0x48 (same shared task-slot layout) reaches the same
  // "load done" value. `c->game->sbs` is null outside the SBS harness and skipRendezvousReached()
  // is a pass-through in every SBS mode except M_SKIP, so this is a genuine no-op everywhere but
  // the compare harness — no PSXPORT_* gameplay toggle.
  if (step == 0 && c->game->sbs &&
      !c->game->sbs->skipRendezvousReached(c, task + 0x48u, 1u, "start_bin_load")) {
    return 1;   // idle this frame — retry next frame, no step advance
  }
  // FRAME ALIGNMENT #2 (SBS-harness-only, no gameplay change — same pattern as the step==0 gate
  // above): case2 below is the SEQ/VAB build (asset.preloadStage1(), the native mirror of the
  // substrate leaf 0x800754F4 that lands sample data at SPU RAM 0x1020). On the oracle sibling
  // (SBS core B, always the PURE recomp substrate — never this native path, see
  // startBinStageFaithful vs. the raw generated ov_start_gen_8010649C), the equivalent work is
  // FUN_8010678C's case-1 spawnAndWait(0x8004514Cu,...): it bumps task+0x48 to 2 BEFORE the wait
  // and only advances it to 3 once the spawned task-1 fiber's own done_flag write (0x1F80019B,
  // asset.cpp:383/gen_func_8004514C) lets spawnAndWait return. task+0x48==3 is therefore the first
  // point after which the oracle's SPU 0x1020 write has already landed.
  //
  // done_flag (0x1F80019B) itself is NOT usable as the gate: it's a REUSED single scratchpad byte
  // — every spawnAndWait call (including case0's texgroup preload, which finishes and sets it back
  // to 1 as early as f0, well before the VAB build even starts) writes the same address. A
  // `skipRendezvousReached(0x1F80019B, 1, …)` gate would false-pass immediately on the STALE value
  // left by case0, never actually waiting for the VAB build. Traced live via
  // PSXPORT_SBS_PREWATCH=0x1F80019B (docs/config.md): the first divergent store the harness catches
  // is gen_func_80044F58 (case0's texgroup task) writing done_flag=1 at f0 — confirming the reuse.
  // task+0x48 has no such ambiguity: it's the SM's own step counter, monotonic 0->1->2->3 across
  // this boot sequence, and is the SAME field (same mem_w16 halfword width) the step==0 gate above
  // already keys on — proven safe to read from the sibling core.
  if (step == 2 && c->game->sbs &&
      !c->game->sbs->skipRendezvousReached(c, task + 0x48u, 3u, "seqvab_build")) {
    return 1;   // idle this frame — retry next frame, no step advance
  }
  switch (step) {
    case 0: break;
    case 1: (void)rngOf(c).next();                                     break;
    case 2: asset.preloadStage1(); c->mem_w16(task + 0x48, 2);        break;
    case 3: c->mem_w16(task + 0x48, 3);                              break;
    case 4: startStage(1); scheduler_yield(c); return 0;             // unreachable
  }
  step++;
  return 1;
}


// FUN_80078824 — Engine::setAreaStartPos. Loads the player's per-area spawn position + facing from
// the start-pos table 0x800A54A8[area] (word = the area's 8-byte-record sub-table), record[sub]:
// three int16 coords stored <<16 fixed to 0x800BF890/894/898, facing byte (masked 0x7F) to
// 0x800BFE38. Leaf, no frame, no callees.
// ORACLE: gen_func_80078824
void Engine::setAreaStartPos() {
  Core* c = this->core;
  const uint32_t START_TABLE = 0x800A54A8u;              // per-area start-pos table base
  uint32_t area  = c->mem_r8(0x800BF870);                // current area index
  uint32_t sub   = c->mem_r8(0x800BF871);                // current sub-area index
  uint32_t rec   = c->mem_r32(START_TABLE + (area << 2));// area's 8-byte-record sub-table
  rec += sub << 3;                                        // record[sub]
  c->mem_w32(0x800BF890, (uint32_t)(int16_t)c->mem_r16(rec + 0) << 16);  // start X (<<16 fixed)
  c->mem_w32(0x800BF894, (uint32_t)(int16_t)c->mem_r16(rec + 2) << 16);  // start Y
  c->mem_w32(0x800BF898, (uint32_t)(int16_t)c->mem_r16(rec + 4) << 16);  // start Z
  c->mem_w8(0x800BFE38, c->mem_r8(rec + 6) & 0x7Fu);                     // facing byte
}


// ── Engine anim-leaf overrides (phase-3 fallthrough-for-already-native, 2026-07-15) ─────────────────
// animTick (FUN_8004190C) and walkStart (FUN_80054D14) are native Engine methods, but the guest
// addresses were registered NOWHERE — so their rec_dispatch/callObj callers (beh_actor_tomba_proximity_
// combat, beh_a06_scripted_actor) + the 5/9 direct substrate func_<addr>(c) shard sites all ran the
// EMULATED body while direct native callers (beh_sop_intro_pilot) ran the port (a split). Found by
// `codemap.py --substrate-fallthrough`. One `overrides::install` entry covers both the registry's
// rec_dispatch path and shard_set_override (g_override[]); core B stays pure substrate. MIRROR_VERIFY-gated.
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_8004190C(Core*);
extern void gen_func_80054D14(Core*);
extern void gen_func_80078824(Core*);   // Engine::setAreaStartPos (engine.cpp)
extern void gen_func_80086604(Core*);   // Engine::activeModeCtx     (startup.cpp)
extern void gen_func_80086738(Core*);   // Engine::installModeHandlers (startup.cpp)
extern void gen_func_80086764(Core*);   // Engine::runModeEnter      (startup.cpp)
namespace {
void ov_engineAnimTick(Core* c)  { c->r[2] = eng(c).animTick(c->r[4]); }
void ov_engineWalkStart(Core* c) { c->r[2] = eng(c).walkStart(c->r[4], c->r[5], (int16_t)c->r[6]); }
void ov_engineSetAreaStartPos(Core* c)   { eng(c).setAreaStartPos(); }
void ov_engineActiveModeCtx(Core* c)     { c->r[2] = eng(c).activeModeCtx(); }
void ov_engineInstallModeHandlers(Core* c) { eng(c).installModeHandlers(); }
void ov_engineRunModeEnter(Core* c)      { c->r[2] = eng(c).runModeEnter(); }
}  // namespace

void RegisterEngineAnimLeafOverrides(Game* /*game*/) {
  using overrides::install;
  install(0x8004190Cu, "Engine::animTick",  ov_engineAnimTick,  gen_func_8004190C, shard_set_override);
  install(0x80054D14u, "Engine::walkStart", ov_engineWalkStart, gen_func_80054D14, shard_set_override);
  install(0x80078824u, "Engine::setAreaStartPos",     ov_engineSetAreaStartPos,     gen_func_80078824, shard_set_override);
  install(0x80086604u, "Engine::activeModeCtx",       ov_engineActiveModeCtx,       gen_func_80086604, shard_set_override);
  install(0x80086738u, "Engine::installModeHandlers", ov_engineInstallModeHandlers, gen_func_80086738, shard_set_override);
  install(0x80086764u, "Engine::runModeEnter",        ov_engineRunModeEnter,        gen_func_80086764, shard_set_override);
}
