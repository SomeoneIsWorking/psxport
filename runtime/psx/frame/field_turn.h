// field_turn.h — psx::FieldTurn: the per-field services a display field owes, in one measured order.
//
// WHY THIS IS AN OWNER AND NOT A COMMENT. The framework's own loop in `native_boot.cpp` did six
// things per field: answer a client pause/step, re-arm the frame watchdog, run the title's finite
// frame step, take the frame's capture and check the presentation contract, honour
// `PSXPORT_RAMDUMP_FRAME`, and service one queued live-channel command. `psx::Machine::stepFrame`
// (1b) did three of them. Spider-Man 1's title-owned loop did three others. Three copies, three
// answers, and the missing steps are silent by construction: a loop that omits the watchdog re-arm
// cannot pause without its frame timeout armed; a loop that omits the RAM-dump frame has a knob
// bound, audited, and read by nothing.
//
// So the services around the frame BODY live here, once, in the order they were measured in. The body
// itself stays the title's — `FrameLoopShell::step` calls the title's `FrameDriver::stepFrame`, takes
// the capture and enforces the one-presentation-per-field contract, and a product's own loop is still
// free to say what its field does.
#pragma once

#include <cstdint>

class Core;

namespace psx::frame {

// The services around one display field. Stateless by design: it owns an ORDER, not a session, so
// every loop — the framework's, `psx::Machine`'s, or a title's — calls the same two halves and gets
// the same obligations fulfilled.
class FieldTurn {
public:
  // Before the body. A client pause or single-step is answered first, because a paused field must not
  // advance guest state and must not consume the watchdog budget; then the watchdog is re-armed,
  // which is what a field owes after the idle a pause, a `step` or a REPL wait just caused. Re-arming
  // claims no progress: the completed present, not this call, is what ends the boot grace window.
  void beginField(Core &core) const;

  // After the body, in this order: `PSXPORT_RAMDUMP_FRAME` (the mid-run RAM dump, which is how a
  // question about LIVE guest state gets answered, since the after-loop dump is a path some products
  // never reach), then exactly one queued live-channel command.
  void endField(Core &core, std::uint32_t frame) const;

private:
  void dumpRamIfRequested(Core &core, std::uint32_t frame) const;
};

} // namespace psx::frame
