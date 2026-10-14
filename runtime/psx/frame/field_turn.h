// field_turn.h — psx::FieldTurn: the per-field services a display field owes, in one measured order.
//
// Per field: answer a client pause/step, re-arm the frame watchdog, run the title's frame step, take
// the capture and check the presentation contract, honour `PSXPORT_RAMDUMP_FRAME`, and service one
// queued live-channel command. A loop that skips one skips it silently.
//
// So the services around the frame BODY live here, once, in the order they were measured in. The body
// itself stays the title's — `FrameLoopShell::step` calls the title's `FrameDriver::stepFrame`, takes
// the capture and enforces the one-presentation-per-field contract.
#pragma once

#include <cstdint>

class Core;

namespace psx::frame {

// The services around one display field. Stateless by design: it owns an ORDER, not a session, so
// both loops — `TitleSession`'s and the picker's — call the same two halves.
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
