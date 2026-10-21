// psx::frame::FramePacer: the display-field cadence a session is held to.
#pragma once

#include "field_cadence.h"
#include "pace_plan.h"

#include <optional>

class Core;

namespace psx::frame {

class FramePacer {
public:
  // The host deadline of a present representing `cadence.fields` display fields (in `cadence.parts`
  // steps), advancing the emulated display-field clock with them; nullopt when unpaced.
  std::optional<double> subframeDeadline(Core &core, FieldCadence cadence);
  // The same for fields the title's scheduler already delivered: the devices do not advance.
  std::optional<double> presentedFieldsDeadline(Core &core, FieldCadence cadence);

  // subframeDeadline, then wait for it.
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
  // The one cadence path: show what is scheduled, plan, advance the clock when asked, count.
  std::optional<double> deadline(Core &core, PaceInputs inputs, bool advanceClock);
  static void wait(std::optional<double> dueMs);
  PacePlan plan(PaceInputs inputs);

  double nextMs_ = 0.0;
  bool seeded_ = false;
  bool warnedQuotaUnset_ = false;
  bool warnedRateUnset_ = false;
  unsigned long long pacedFields_ = 0;
};

} // namespace psx::frame
