// psx::frame::FramePacer: the display-field cadence a session is held to.
#pragma once

#include "field_cadence.h"
#include "pace_plan.h"

class Core;

namespace psx::frame {

class FramePacer {
public:
  // Pace `cadence.fields` display fields (in `cadence.parts` steps) and advance the emulated
  // display-field clock with them.
  void paceSubframeFields(Core &core, FieldCadence cadence);

  // Wait for fields the title's scheduler already delivered, without advancing devices.
  void waitPresentedFields(Core &core, FieldCadence cadence);

  // One host-screen field (no guest display mode), honouring PSXPORT_NOPACE as guest pacing does.
  void hostScreenPace(Core &core);

  // Display fields this pacer waited for since it was created; an unpaced fast-forward adds nothing.
  unsigned long long pacedFields() const {
    return pacedFields_;
  }

private:
  // The one cadence path: plan, advance the clock when asked, count and wait.
  void pace(Core &core, PaceInputs inputs, bool advanceClock);
  PacePlan plan(PaceInputs inputs);

  double nextMs_ = 0.0;
  bool seeded_ = false;
  bool warnedQuotaUnset_ = false;
  bool warnedRateUnset_ = false;
  unsigned long long pacedFields_ = 0;
};

} // namespace psx::frame
