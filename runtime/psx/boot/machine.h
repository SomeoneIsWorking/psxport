// machine.h — psx::Machine: the boot steps every product composes, in the measured order.
// `psx::host::TitleSession` composes them for a catalog product; a title with its own loop calls the
// same steps and `run`.
#pragma once

#include "field_turn.h"

#include <cstdint>
#include <string>

class Core;
class Game;
struct GuestProgramImage;

namespace psx::host {
struct TitleIdentity;
} // namespace psx::host

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

  // Points the CD model at the title's own disc key. The image opens on the first read, so this
  // precedes any disc access: an executable extracted from the disc, the title's boot, the first field.
  void insertDisc(const psx::host::TitleIdentity &title);

  // Binds the per-Core devices in the framework's measured order and seeds `a0`/`a1` the way the BIOS
  // leaves them, before any guest code can run. Call it once, before `prepare`.
  void bindDevices();

  // The one-time configuration audit — every active PSXPORT_* knob, once, before anything reads one.
  void reportConfigurationOnce();

  // The host's own measuring instruments: the caller-attributing host census, and the active-config
  // dump. Process-wide, armed once before any frame runs; needs no Game.
  static void armHostDiagnostics();

  // Black the display framebuffer before the title builds, so the title's first frames (drawn over
  // several fields while its background/font/CLUT upload) never composite over a stale splash or an
  // FMV last frame. Deterministic, no timer.
  void clearDisplayForFrontEnd();

  // Why the image's declared `bootEntry` cannot boot it, or empty when it can. `Crt0` needs the complete
  // crt0 group (`psx::crt0Completeness`); `DriverOwned` needs nothing here. No image declares no entry.
  static std::string bootEntryRefusal(const GuestProgramImage *image);

  // The guest's boot entry. `Crt0`: apply the derived boot group (bss, stack, heap, `gp`, `a0`/`a1`, libc
  // init) with the completeness refusal and the shipped-constants cross-check, through `crt0_setup`.
  // `DriverOwned`: nothing; the title's frame driver enters the guest from the executable's entry.
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
  // dispatch.
  void prepareProduct();

  // Everything between the binds and the first field, in the order a product must do it: the title's
  // own overrides (which may already need the binds), then the frame-loop preflight. A title whose
  // native leaves it registers itself calls the two halves itself; the titles that used to spell both
  // lines out have nothing left to spell.
  void prepare();

  // The live control channel and the dynarec store observer, armed together and before the first
  // field. How long the run may go is `psx::host::RunBudget`'s, not the channel's.
  void attachControlChannel();

  // One display field: the shared per-field services (`psx::frame::FieldTurn`), then the title's finite frame step
  // (`psx::frame::FrameLoopShell::step`, which takes the frame's capture and enforces the one-presentation-per-field
  // contract). The pause is honoured BEFORE the body and the command serviced AFTER it, so a read never
  // observes a half-completed command.
  void stepFrame(std::uint32_t frame);

  // The product loop, for a title with no loop of its own: `stepFrame` until the cap, a window close,
  // or a client `quit`; then the RunEndReport every run owes, whatever ended it.
  //
  // Ctrl+C is deliberately NOT here: the watchdog handler force-exits with the stuck PC, because a
  // hung guest loop never returns to this loop at all.
  void run(std::uint32_t frameCap = 0);

private:
  Game &game_;
  psx::frame::FieldTurn fieldTurn_;
};

} // namespace psx
