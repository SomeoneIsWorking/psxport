// machine.h — psx::Machine: the boot composition and the field turn every product shares.
//
// WHAT THIS OWNS, and why it had to exist:
//
// Nine consuming repositories each wrote the same spine: bind the per-Core devices in the measured
// order, seed `a0`/`a1` as the BIOS leaves them, register the title's overrides, run the frame-loop
// preflight, attach the live control channel, arm the store observer, and then loop over a finite
// title-owned frame step. Four of them carried a long comment explaining that they had to, because the
// framework's own spine (`native_boot_run`) could not host a field step they own. Every one of those
// spines is a chance to omit a step: a title with no live endpoint cannot be driven or observed, and
// a title with no per-field `honourPause`/`service` pair cannot be paused.
//
// So the composition is here, and a title keeps only its FACTS: which runtime policy it installs,
// where its executable is, what its field step does, and what its frame cap should be.
//
// ONE OWNER, NOT TWO SPINES. The framework's own `native_boot_run` composes through this class as
// well, step for step, so "the product that boots natively" and "the product that owns its loop" are
// the same sequence with the same obligations. Each step below is explicit and optional: a spine calls
// the ones it needs, in the measured order, and the classes that own a step (`psx::frame::FrameLoopShell`,
// `psx::frame::FieldTurn`, `crt0_setup`, `psx::state`) keep their own contracts.
#pragma once

#include "field_turn.h"

#include <cstdint>
#include <string>

class Core;
class Game;

namespace psx {

// One product run: the order the machine must come up in, the control channel it is driven over, and
// the field turn. Construction composes; nothing here decides what a frame does.
class Machine {
public:
  // The title's runtime policy and its `Game` already exist — a `Game` reads the installed runtime as
  // it is constructed, so install it first. Construction binds nothing: every step below is adopted
  // one at a time, because a composition that arrives as a constructor side effect is a composition no
  // caller can decline deliberately and review.
  explicit Machine(Game &game);
  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  // Binds the per-Core devices in the framework's measured order and seeds `a0`/`a1` the way the BIOS
  // leaves them, before any guest code can run. Call it once, before `prepare`.
  void bindDevices();

  // The same binders WITHOUT the device initialisers, for a spine that has already initialised them
  // (`native_boot`'s game_main runs after the title's own init) or must not re-initialise them. The
  // GTE/SPU/MDEC/XA binding is what this exists for: those four reached a PROCESS-GLOBAL through the
  // vendored entry points, and the binding must name the Core that is running, not the one that booted
  // last. `psx::frame::FrameLoopShell::step` re-checks it per field anyway; doing it before the first guest call is
  // what keeps a title's own boot prologue off the wrong instance.
  void bindSession();

  // The one-time configuration audit — every active PSXPORT_* knob, once, before anything reads one.
  void reportConfigurationOnce();

  // The host's own measuring instruments: the caller-attributing host census, and the active-config
  // dump. Armed here, before any frame runs, because an instrument that cannot produce evidence is
  // worse than none — its existence answers "can we measure this?" with a yes.
  void armHostDiagnostics();

  // The render path (native | gte | psx) resolved from the configuration ladder, at the shared Core
  // setup boundary. A harness may deliberately replace it after this call.
  void installRenderPath();

  // The boot movies the title declares (`GameConfig::bootFmv`), played before the guest's own crt0 —
  // and ONLY those: an all-null list is a real answer ("this title plays no movie natively"), not a
  // missing value. `PSXPORT_NO_FMV` is the diagnostic control; whether a movie PLAYS is game
  // behaviour, and is never inferred from the render sink.
  void playBootMovies();

  // Black the display framebuffer before the title builds, so the title's first frames (drawn over
  // several fields while its background/font/CLUT upload) never composite over a stale splash or an
  // FMV last frame. Deterministic, no timer.
  void clearDisplayForFrontEnd();

  // The guest crt0: apply the derived boot group (bss, stack, heap, `gp`, `a0`/`a1`, libc init) with
  // the completeness refusal and the shipped-constants cross-check. Refuses the whole boot rather than
  // fabricating guest state; `crt0_setup` owns that decision and `crt0_boot.h` the derivation.
  void setupGuestBoot();

  // `PSXPORT_LOAD_STATE`: resume a whole-machine state BEFORE the first field, so a headless tool
  // starts inside a level instead of replaying thousands of fields. Fatal on failure rather than a
  // fall back to power-on: a run that silently rebooted after a broken path would spend its budget
  // re-deriving the state it was asked to start from and produce a plausible-looking trace.
  void applyConfiguredState();

  // The same, reporting instead of terminating — the form a test or a tool with its own error policy
  // calls. False carries the reason; the state was not applied.
  bool tryApplyConfiguredState(std::string &error) const;

  Game &game() const {
    return game_;
  }
  Core &core() const;

  // The title's own overrides: which guest leaves are native. Registered BEFORE the preflight, because
  // the preflight reports a product with no finite frame owner and a title's own registration is
  // allowed to be part of answering that.
  void registerTitleOverrides();

  // The frame-loop preflight: refuses a product with no finite frame owner before any guest code can
  // dispatch. Separate from `registerTitleOverrides` because the framework's own spine does NOT
  // register the title's overrides — a standalone game main installs its clusters before entering it.
  void prepareProduct();

  // Everything between the binds and the first field, in the order a product must do it: the title's
  // own overrides (which may already need the binds), then the frame-loop preflight. A title whose
  // native leaves it registers itself calls the two halves itself; the titles that used to spell both
  // lines out have nothing left to spell.
  void prepare();

  // The live control channel and the dynarec store observer, armed together and before the first
  // field, and the frame cap THIS run must use: 0 means uncapped, which is what a client-driven run
  // needs — a cap exists to bound an unattended smoke run, and one that ends the process before a
  // client connects measures nothing. `requestedFrameCap` is the title's own bound (or 0).
  //
  // Both surfaces are here because a surface nobody opens is not a surface: each was reachable only
  // through the framework's own spine, so a title-owned spine had an audit line saying the knob was
  // bound while nothing read it.
  std::uint32_t attachControlChannel(std::uint32_t requestedFrameCap);

  // One display field: the shared per-field services (`psx::frame::FieldTurn`), then the title's finite frame step
  // (`psx::frame::FrameLoopShell::step`, which takes the frame's capture and enforces the one-presentation-per-field
  // contract). The pause is honoured BEFORE the body and the command serviced AFTER it, so a read never
  // observes a half-completed command.
  void stepFrame(std::uint32_t frame);

  // The product loop, for a title with no loop of its own: `stepFrame` until the cap, a window close,
  // or a client `quit`; then the run-end ledger every run owes, whatever ended it.
  //
  // Ctrl+C is deliberately NOT here: the watchdog handler force-exits with the stuck PC, because a
  // hung guest loop never returns to this loop at all.
  void run(std::uint32_t frameCap = 0);

  // The obligations a finished run owes WHATEVER ended it: the whole-run guest ledger, the guest-call
  // census, and the after-loop RAM dump when the title asked for one. The dump is here rather than in a
  // spine because it is a path some products never reach — it is the MID-RUN dump (`psx::frame::FieldTurn`) that
  // answers a question about live guest state.
  void reportRunEnd();

private:
  Game &game_;
  psx::frame::FieldTurn fieldTurn_;
};

} // namespace psx