// title_settings.h - an integer setting a title offers in the settings menu.
//
// The title owns the CVar and its range; the framework draws one stepped row for it and persists the
// player's choice in the settings file under the CVar's name.
#pragma once

#include "config_var.h"
#include "stepped_range.h"

struct TitleIntSetting {
  const char *id;    // menu row id, unique within the title
  const char *label; // row text
  const char *unit;  // appended to the value, e.g. "%"
  psx::config::IntVar *var;
  psx::config::SteppedRange range;
};
