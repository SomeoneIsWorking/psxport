// frame_pacer.h — psx::frame::FramePacer: the display-field cadence a run is held to.
//
// WHY THE CADENCE IS AN OBJECT AND NOT SIX GLOBAL FUNCTIONS. `gpu_pace_frame`, `gpu_pace_subframe`,
// `gpu_pace_subframe_fields`, `gpu_wait_presented_fields`, `host_screen_pace` and
// `gpu_fieldRateMilliHz` were six `extern`-style C functions taking `Core *`, defined beside this
// class and reaching back into it (`core->game->framePacer.plan(...)`) to do their work. The owning
// object was already there; the six functions were a second door into it, callable with a null Core
// from anywhere and free to disagree with the instance they were pacing through. They are members
// here, taking `Core &`, because the cadence is a property of the session being paced.
#pragma once

#include "pace_plan.h"

class Core;

namespace psx::frame {

class FramePacer {
public:
  // The caller supplies cadence and current host time; this instance owns its running deadline.
  PacePlan plan(PaceInputs inputs);

  // The combined path for runtimes whose presentation owns simulated display-field advancement: pace
  // one guest field (or `parts` of one) and advance the emulated display-field clock with it.
  void paceFrame(Core &core);
  void paceSubframe(Core &core, int parts);
  void paceSubframeFields(Core &core, int guestFields, int parts);

  // Presentation of fields already delivered by a title's scheduler: wait WITHOUT advancing devices,
  // because the guest has already produced them.
  void waitPresentedFields(Core &core, int guestFields, int parts);

  // Pace a host-only screen (a title picker: no guest, no display mode programmed) at one NTSC display
  // field per call, honouring PSXPORT_NOPACE exactly as guest-driven pacing does.
  void hostScreenPace(Core &core);

  // The display field rate decoded from the standard the guest programmed through GP1(0x08). Zero
  // until it is: no clock to pace against, and none is invented.
  unsigned fieldRateMilliHz(Core &core) const;

private:
  double nextMs_ = 0.0;
  bool seeded_ = false;
};

} // namespace psx::frame
