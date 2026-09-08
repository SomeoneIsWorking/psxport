// Shared display-field cadence and host waiting, with explicit simulated-time ownership.
#pragma once

#include "pace_plan.h"

class Core;

class FramePacer {
public:
  // The caller supplies cadence and current host time; this instance owns its running deadline.
  PacePlan plan(PaceInputs inputs);

private:
  double nextMs_ = 0.0;
  bool seeded_ = false;
};

// Combined path for runtimes whose presentation owns simulated display-field advancement.
void gpu_pace_frame(Core *core);
void gpu_pace_subframe(Core *core, int parts);
void gpu_pace_subframe_fields(Core *core, int guestFields, int parts);

// Presentation of fields already delivered by a title's scheduler: wait without advancing devices.
void gpu_wait_presented_fields(Core *core, int guestFields, int parts);

// The display field rate decoded from the standard the guest programmed through GP1(0x08).
unsigned gpu_field_rate_millihz(Core *core);
