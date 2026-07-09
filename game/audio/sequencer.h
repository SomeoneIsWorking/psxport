// game/audio/sequencer.h — class Sequencer — libsnd per-vblank sequencer TICK wrapper.
//
// PROPER OOP: one instance per Core, embedded on Engine (`c->engine.sequencer`). Back-pointer
// `core` wired at Core construction time (same pattern as MusicCoord / AudioDispatch).
//
// SCOPE (WIDE-RE DRAFT — see docs/engine_re.md libsnd section, docs/journal.md 2026-06-15
// "later 54" entry): 0x800909C0 FUN_800909c0 is libsnd's per-VBlank tick wrapper, installed by
// SsSetTickMode as the IRQ-driven "user cb". Runtime globals (confirmed live via REPL dump):
//   tick mode        DAT_800ac424 = 5
//   SsSeqCalled ptr   DAT_800ac42c = 0x80090BD0   (the sequencer engine itself — deep libsnd
//                                                   internals: per-channel note-state cluster,
//                                                   still substrate — see sequencer.cpp)
//   user callback     DAT_800ac430 = 0x80086288   (Timing::vsyncCallbackDispatch — see timing.h;
//                                                   also unwired/unowned by us)
// The wrapper itself (FUN_800909c0) is a 2-call trampoline: run the user cb if installed, then
// unconditionally run *SsSeqCalled(). NEITHER 0x800909c0 nor 0x80090bd0 is reached by a direct
// `jal` anywhere in MAIN.EXE (only ever fired through the IRQ callback pointer), so the static
// recompiler's indirect-call discovery never saw them — they only run via the hybrid interpreter
// today (game/game_tomba2.cpp SEQ_TICK_WRAPPER, called once per ov_frame_update).
#pragma once
#include <cstdint>
class Core;

class Sequencer {
public:
  Core* core = nullptr;

  // frameTick(): 0x800909C0 FUN_800909c0 — per-VBlank libsnd tick wrapper. Faithful to
  //   gen_func_800909C0 (generated/shard_7.c:14127): if the user-cb slot (0x800AC430) is
  //   non-null, rec_dispatch() it (today: unowned — same guest address as
  //   Timing::vsyncCallbackDispatch, NOT auto-routed there since this draft is unwired); then
  //   unconditionally rec_dispatch() the SsSeqCalled pointer at 0x800AC42C (today: gen_func_
  //   80090BD0, deep per-channel note-flag dispatcher — see sequencer.cpp comment; too large a
  //   dependency graph (func_800910F0/80090E40/80092080/80091050/80091910/80091970/800931C0,
  //   none yet owned) to draft faithfully in this pass, so it stays a rec_dispatch call here
  //   rather than a native body). WIDE-RE DRAFT, UNWIRED: game_tomba2.cpp's SEQ_TICK_WRAPPER
  //   call still reaches the interpreter/substrate body directly, not this method.
  void frameTick();
};
