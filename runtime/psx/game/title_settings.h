// An integer setting a title offers in the settings menu; the title owns the knob, the settings file keeps it by name.
#pragma once

#include "config_var.h"
#include "stepped_range.h"

namespace psx::game {

struct TitleIntSetting {
  const char *id;    // menu row id, unique within the title
  const char *label; // row text
  const char *unit;  // appended to the value, e.g. "%"
  psx::config::Var<long> *var;
  psx::config::SteppedRange range;
};

} // namespace psx::game
