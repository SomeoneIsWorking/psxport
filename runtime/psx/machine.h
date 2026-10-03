// machine.h — psx::Machine: the boot composition and the frame turn every product shares.
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
#pragma once

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

  Game &game() const {
    return game_;
  }
  Core &core() const;

  // Everything between the binds and the first field, in the order a product must do it: the title's
  // own overrides (which may already need the binds), then the frame-loop preflight that refuses a
  // product with no finite frame owner. A title whose native leaves it registers itself instead calls
  // the preflight on its own; the titles that used to spell both lines out have nothing left to spell.
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

  // One display field: honour a client pause, run the title's finite frame step, then service at most
  // one queued command. The pause is honoured BEFORE the step and the command serviced AFTER it, so a
  // read never observes a half-completed command.
  void stepFrame(std::uint32_t frame);

  // The product loop, for a title with no loop of its own: `stepFrame` until the cap, a window close,
  // or a client `quit`; then the run-end ledger every run owes, whatever ended it.
  //
  // Ctrl+C is deliberately NOT here: the watchdog handler force-exits with the stuck PC, because a
  // hung guest loop never returns to this loop at all.
  void run(std::uint32_t frameCap = 0);

private:
  Game &game_;
};

} // namespace psx