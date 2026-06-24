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
#include "engine_render.h"          // ov_render_frame / ov_render_frame_x (native per-frame render driver)
#include "placement.h"              // ov_place_objects — native field object-placement driver (game/world)
#include "pool.h"                    // ov_pool_init_run — native object-pool init (game/world)
#include <stdio.h>

// dispatch a still-recomp leaf with up to 3 args set (helpers for the native stage machines).
// later-238 BACKDROP ATTRIBUTION (PSXPORT_BDTAG): record each ov_field_frame call's pool-write span so the
// gp0 OT-walk classifier (gpu_native.cpp) can attribute a DEFERRED prim (e.g. the tp(576,256) sea backdrop)
// to the call that BUILT it — reliable where per-pass tags / WWATCH-pc / pool-node-addresses are not. The
// span table persists across the present (which classifies the prior frame's OT) because it is reset only at
// the TOP of the next ov_field_frame. `ffspan_lookup(addr)` returns the builder name (latest-span-wins).
extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
extern "C" void dv_snapshot(Core*);   // dual-view: capture pre-render state (native_boot.cpp); no-op unless on
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
// later in the same frame's DrawOTag). begin/end do NOT nest (single saved slot) — bracket sibling phases only.
static struct { int ot; uint32_t lo, hi; } s_ff_stk[8]; static int s_ff_sp = 0;
extern "C" void ffspan_reset_frame(void) { if (bdtag_on()) { s_ffspan_n = 0; s_ff_sp = 0; } }
extern "C" void ffspan_begin(void) {       // NESTABLE: save outer, start a fresh empty span
  if (!bdtag_on() || s_ff_sp >= 8) return;
  s_ff_stk[s_ff_sp].ot = g_pkt_track; s_ff_stk[s_ff_sp].lo = g_pkt_lo; s_ff_stk[s_ff_sp].hi = g_pkt_hi; s_ff_sp++;
  g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
}
extern "C" void ffspan_end(const char* nm) {
  if (!bdtag_on() || s_ff_sp <= 0) return;
  uint32_t mlo = g_pkt_lo, mhi = g_pkt_hi;
  if (mhi > mlo && s_ffspan_n < 40) { s_ffspan[s_ffspan_n].name = nm;
    s_ffspan[s_ffspan_n].lo = mlo; s_ffspan[s_ffspan_n].hi = mhi; s_ffspan_n++; }
  s_ff_sp--; g_pkt_track = s_ff_stk[s_ff_sp].ot;            // restore outer; MERGE my writes into it
  g_pkt_lo = s_ff_stk[s_ff_sp].lo; g_pkt_hi = s_ff_stk[s_ff_sp].hi;
  if (mhi > mlo) { if (mlo < g_pkt_lo) g_pkt_lo = mlo; if (mhi > g_pkt_hi) g_pkt_hi = mhi; }
}
extern "C" void ffspan_dump(uint32_t a) {   // one-time: show the recorded spans vs an unattributed address
  static int done = 0; if (done) return; done = 1;
  fprintf(stderr, "[ffspan] addr %08x NOT in any of %d field-frame spans:\n", a, s_ffspan_n);
  for (int i = 0; i < s_ffspan_n; i++)
    fprintf(stderr, "[ffspan]   %-12s [%08x .. %08x)\n", s_ffspan[i].name, s_ffspan[i].lo, s_ffspan[i].hi);
}
#define FFS(nm, call) do { \
  if (bdtag_on()) { int _ot = g_pkt_track; uint32_t _olo = g_pkt_lo, _ohi = g_pkt_hi; \
    g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0; call; \
    if (g_pkt_hi > g_pkt_lo && s_ffspan_n < 40) { s_ffspan[s_ffspan_n].name = nm; \
      s_ffspan[s_ffspan_n].lo = g_pkt_lo; s_ffspan[s_ffspan_n].hi = g_pkt_hi; s_ffspan_n++; } \
    g_pkt_track = _ot; g_pkt_lo = _olo; g_pkt_hi = _ohi; } \
  else { call; } } while (0)

static inline void d0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static inline void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4]=a0; rec_dispatch(c, fn); }
static inline void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static inline void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn);
}

// sm[0x48] == 0 — area INIT: advance to running (sm[0x48]=2), reset the sub-machine state, run the per-area
// setup fns. (GAME.BIN 0x801086e0) Verified runtime-exercised + RAM 0-diff.
static void ov_game_s48_0(Core* c) {
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
static void ov_game_s48_1(Core* c) {
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
static void ov_game_s48_2(Core* c) {
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
static void ov_game_s4c(Core* c) {
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
  rec_dispatch(c, 0x80075a80u);                 // per-frame area update (synchronous — verified yield-free)
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

void ov_sop_field_mode(Core*);           // engine/sop.cpp — native SOP field-mode machine
void native_transition_area_load(Core*); // engine/sop.cpp — sync transition area-DATA load
void ov_objwalk(Core*);                  // engine/engine_tomba2.cpp — native FUN_8007a904 object-list walk
void ov_disp_26c88(Core*);               // engine/entity.cpp — native FUN_80026c88 display update
static void ov_game_submode0(Core* c);   // fwd
static void ov_game_submode1(Core* c);   // fwd

// sm[0x48]==2 RUNNING, per-frame variant: dispatch sm[0x4a] handler. handler[0] = the GAME->SOP bridge
// 0x8010882c (owned native, ov_game_submode0); the others stay rec_dispatch leaves (synchronous; a
// not-yet-sync leaf that yields is contained by the scheduler setjmp = frame-done).
static void ov_game_s48_2_frame(Core* c) {
  static const uint32_t handler[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4a = c->mem_r16(sm + 0x4a);
  if (s4a >= 6) return;
  if (s4a == 0) { ffspan_begin(); ov_game_submode0(c); ffspan_end("submode0"); return; }
  if (s4a == 1) { ffspan_begin(); ov_game_submode1(c); ffspan_end("submode1"); return; }
  ffspan_begin(); rec_dispatch(c, handler[s4a]); ffspan_end("s48_2_handler");
}

// GAME sub-mode-0 bridge 0x8010882c (sm[0x4c]/sm[0x4e] dispatch) — native. Faithful to the disasm:
// sm[0x4c]==0 & sm[0x4e]==0 -> input-reset 0x8005082c (sync leaf) + sm[0x50]=0, sm[0x4e]=1; sm[0x4e]==1
// -> run the native SOP field-mode machine; sm[0x4c]==1 -> sm[0x4c]=0, sm[0x4a]++.
static void ov_game_submode0(Core* c) {
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
      if (c->mem_r32(0x80109450u) == 0x3C021F80u) ov_sop_field_mode(c);   // native SOP
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
// FIELD PER-FRAME UPDATE 0x80108b0c — native control flow (the field frame's work driver, called by
// the running states of the sm[0x4e] machine). Faithful to the disasm: bump *0x1f80017c and *0x800bf878;
// if NOT paused (*0x1f800136==0) run the 11-call gameplay-update block; if *0x1f800136 < 2 run 0x8003f9a8;
// then always the render-submit 0x8010810c + 0x80077d8c + per-frame area update 0x80075a80. Yield-free
// (transitive jal scan, 1021 fns). The object-walk 0x8007a904 and display 0x80026c88 now run as the
// NATIVE ov_objwalk / ov_disp_26c88 (direct C calls — the previously-orphan bodies wired into the live
// field frame); the remaining callees stay rec_dispatch leaves until owned in turn.
static void ov_field_frame(Core* c) {
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: full gameplay update
    FFS("ff_59d28", d0(c, 0x80059d28u)); FFS("ff_69b28", d0(c, 0x80069b28u));
    FFS("ff_26368", d0(c, 0x80026368u)); FFS("ff_objwalk", ov_objwalk(c));            // 0x8007a904 NATIVE
    FFS("ff_25588", d0(c, 0x80025588u)); FFS("ff_4fe84", d0(c, 0x8004fe84u));
    FFS("ff_disp26c88", ov_disp_26c88(c)); FFS("ff_22a80", d0(c, 0x80022a80u));       // 0x80026c88 NATIVE
    FFS("ff_6ec44", d0(c, 0x8006ec44u)); FFS("ff_50de4", d0(c, 0x80050de4u)); FFS("ff_1cac0", d0(c, 0x8001cac0u));
  }
  // DUAL-VIEW: snapshot the post-gameplay / pre-render state so the side-by-side PSX render pass can run
  // from it (the native render below consumes per-frame queues, so it is not re-runnable). No-op unless on.
  dv_snapshot(c);
  if (c->mem_r8(0x1f800136u) < 2) ov_render_frame(c);   // 0x8003f9a8 — NATIVE render orchestrator + walk
  FFS("ff_submit810c", d0(c, 0x8010810cu));     // render submit
  FFS("ff_77d8c", d0(c, 0x80077d8cu));
  FFS("ff_area75a80", d0(c, 0x80075a80u));      // per-frame area update
}

// FIELD RUNNING sub-machine 0x80106b98 — native control flow + state bodies (decomp:
// scratch/decomp/game/80106b98.c). A 12-way switch on sm[0x4e]; the running states call the native
// ov_field_frame (0x80108b0c) and the heavy leaf callees rec_dispatch. NB the guest fall-throughs are
// faithful: case 2 -> 3, case 4 -> 1 (no break). sm[0x4e] >= 12 = no-op. This anchors the field frame
// natively; the leaf callees (object-placement FUN_80072a78 etc.) are the next descent.
static void ov_field_run(Core* c) {
  uint32_t sm  = c->mem_r32(0x1f800138u);
  uint16_t s4e = c->mem_r16(sm + 0x4e);
  switch (s4e) {
    case 0:
      ov_pool_init_run(c);   // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x8007b18c)
      ov_796dc_run(c);       // OWNED native (game/world/pool.cpp) — replaces rec_dispatch(0x800796dc)
      d0(c, 0x800263e8u);
      ov_place_objects(c);   // OWNED native (game/world/placement.cpp) — replaces rec_dispatch(0x80072a78)
      d0(c, 0x80075240u); d0(c, 0x800783dcu); d0(c, 0x80078610u);
      sm = c->mem_r32(0x1f800138u);
      c->mem_w16(sm + 0x4e, 1);
      c->mem_w8(sm + 0x6b, 0);
      if (c->mem_r8(0x800bf89cu) == 2) { c->mem_w16(sm + 0x4e, 9); }
      else if (c->mem_r8(0x800bf870u) == 8) { d0(c, 0x80114b90u); }
      else if (c->mem_r32(0x800bf870u) == 0x15) { c->mem_w16(sm + 0x4e, 0xb); return; }
      d1(c, 0x80074f24u, c->mem_r8(0x800bf870u));
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
      ffspan_begin(); ov_field_frame(c); ffspan_end("fieldframe");   // native field per-frame update (0x80108b0c)
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
      ov_field_frame(c);
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
      ov_field_frame(c);
      sm = c->mem_r32(0x1f800138u);
      if (c->mem_r8(0x800bf89cu) == 2 && (c->mem_r32(0x800e7e68u) & 8) != 0) {
        c->mem_w16(sm + 0x4e, (uint16_t)(c->mem_r16(sm + 0x4e) + 1));
        c->mem_w8(0x800bf809u, 1);
        c->mem_w8(sm + 0x6e, 0x1f);
      }
      break;
    case 10: {
      ov_field_frame(c);
      sm = c->mem_r32(0x1f800138u);
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6e) * (uint32_t)-8) & 0xff;
      d3(c, 0x8007e9c8u, (u << 16) | (u << 8) | u, 0, 0);
      uint8_t nv = (uint8_t)(c->mem_r8(sm + 0x6e) - 1);
      c->mem_w8(sm + 0x6e, nv);
      if (nv == 0) { c->mem_w16(sm + 0x4e, 7); d0(c, 0x8001cf2cu); }
      break;
    }
    case 0xb:
      d1(c, 0x8010957cu, 0x800e8008u);
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
static void ov_field_frame_x(Core* c) {
  c->mem_w16(0x1f80017cu, (uint16_t)(c->mem_r16(0x1f80017cu) + 1));   // frame counter
  c->mem_w32(0x800bf878u, c->mem_r32(0x800bf878u) + 1);
  if (c->mem_r8(0x1f800136u) == 0) {            // not paused: reduced gameplay update
    d0(c, 0x80059d28u); d0(c, 0x80069b28u); d0(c, 0x80026368u); d0(c, 0x8007b04cu);   // 0x8007b04c obj update
    d0(c, 0x80025588u); d0(c, 0x8004fe84u); ov_disp_26c88(c); d0(c, 0x80022a80u);     // 0x80026c88 NATIVE
    d0(c, 0x8006ec44u);
  }
  if (c->mem_r8(0x1f800136u) < 2) ov_render_frame_x(c); // 0x8003fa44 — NATIVE render orchestrator twin
  d0(c, 0x8010810cu);                           // render submit
  d0(c, 0x80077d8cu);
  d0(c, 0x80075a80u);                           // per-frame area update
}

// FIELD RUNNING sub-machine VARIANT 0x801070b4 (sm[0x4c]==3, the mid-transition running handler reached
// when a door/edge sets up an area change). A switch on sm[0x4e]: state 0 = init (scene reset + input
// reset) then fall into state 1; state 1 = run ov_field_frame_x and check the mode-3 / area-change exit
// conditions to hand back to the normal running handler (sm[0x4c]=2); state 2 = bump to 1. Faithful to
// the disasm (hand-decompiled from the field overlay).
static void ov_field_run_x(Core* c) {
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
  ov_field_frame_x(c);                           // 0x80108be4 per-frame (state 1, 0x80107118)
  if (c->mem_r8(0x800bf80du) == 3) {             // mode-3 exit (0x80107138)
    if (c->mem_r8(0x800bf80fu) != 0) return;
    d0(c, 0x80074bc4u);
    sm = c->mem_r32(0x1f800138u);
    c->mem_w16(sm + 0x4c, 2);                     // back to normal running handler
    int16_t  e_s = (int16_t)c->mem_r16(0x800e7feeu);
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

static void ov_game_submode1(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (cfg_dbg("stage") && s4c <= 1)
    fprintf(stderr, "[stage] submode1 case %u: bf870=%u nexttab[bf870]=%u\n",
            s4c, c->mem_r8(0x800bf870u), c->mem_r8(0x80108f60u + c->mem_r8(0x800bf870u)));
  switch (s4c) {
    case 0:
      rec_dispatch(c, 0x8005245cu);          // FUN_8005245c (sound/CD setup, sync leaf)
      native_transition_area_load(c);         // INLINE sync load (replaces FUN_80044bd4) -> 1f80019b=1
      // FALL THROUGH to state 1: in the guest, 0x80108918 does NOT branch to the epilogue — it falls
      // into 0x8010893c, so the load AND the next-state selection both run in this one frame.
      /* fallthrough */
    case 1: {
      c->mem_w8(0x1f800234u, 0);
      uint8_t area = c->mem_r8(0x800bf870u);
      uint8_t next = c->mem_r8(0x80108f60u + area);
      c->mem_w16(sm + 0x4c, next);
      break;
    }
    case 2: ffspan_begin(); ov_field_run(c); ffspan_end("fieldrun"); break;   // field RUNNING sub-machine (sm[0x4e]) — native
    case 3: ov_field_run_x(c); break;              // mid-transition running sub-machine 0x801070b4 — native
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
int ov_game_frame(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t s48 = c->mem_r16(sm + 0x48);
  if (s48 == 2) {
    uint16_t s4a = c->mem_r16(sm + 0x4a);
    // OWNED running sub-modes: 0 = SOP-intro (SOP overlay must be loaded); 1 = field area machine
    // (0x801088d8, the walkable field — its load is sync via native_transition_area_load, its running
    // states are yield-free). Other sub-modes (2..5, the area-machine variants) aren't owned yet.
    if (s4a == 0) {
      if (c->mem_r32(0x80109450u) != 0x3C021F80u) return 0;            // SOP not loaded -> cooperative
    } else if (s4a != 1) {
      return 0;                                                        // unowned running sub-mode
    }
    ov_game_s48_2_frame(c);
  } else if (s48 == 0) {
    ffspan_begin(); ov_game_s48_0(c); ffspan_end("s48_0");
  } else if (s48 == 1) {
    ffspan_begin(); ov_game_s48_1(c); ffspan_end("s48_1");
  } else {
    return 0;                                                          // unknown top state -> cooperative
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
void ov_game_stage_prologue(Core* c) {
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
// native per-frame path (game_native in native_scheduler_step calls ov_game_stage_prologue + ov_game_frame).
// Retained as a reference / fallback; not on the live path.
void ov_game_stage_main(Core* c) {
  ov_game_stage_prologue(c);
  rec_coro_redirect(c, 0x801063F4u);
}

// Register the GAME-stage area-init overrides when this just-loaded overlay is GAME.BIN at the stage base.
// Detect by the fixed entry + handler signatures (START.BIN/DEMO.BIN are smaller and hold stale bytes at
// these addresses, so they never match). Called from the overlay-load scan (engine_submit.cpp); registered
// AUTO so it is flushed when GAME.BIN unloads and another overlay reuses the base (mirrors the M3 scan).
// OVERRIDE SYSTEM REMOVED (2026-06-22): this scan used to register the GAME stage-machine handlers
// (ov_game_stage_main + sub-handlers s48_0/1/2) into the address-keyed override table when GAME.BIN
// loaded. The table is gone; ov_game_stage_main is now called DIRECTLY from the scheduler
// (native_boot.cpp). The sub-handler ov_game_* defs are kept as future direct-call targets. No-op.
void stage_scan_overlay(Core* c, uint32_t base, uint32_t size) {
  (void)c; (void)base; (void)size;
  (void)ov_game_s48_0; (void)ov_game_s48_1; (void)ov_game_s48_2; (void)ov_game_s4c;
}
