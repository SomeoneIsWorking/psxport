// An integer setting a title offers in the settings menu; the title owns the knob, the settings file keeps it by name.
#pragma once

#include "config_var.h"
#include "stepped_range.h"

#include <span>
#include <string>

namespace psx::game {

struct TitleIntSetting {
  const char *id;    // menu row id, unique within the title
  const char *label; // row text
  const char *unit;  // appended to the value, e.g. "%"
  psx::config::Var<long> *var;
  psx::config::SteppedRange range;
  // A choice's names, one per step from range.min; empty shows the number and unit.
  std::span<const char *const> labels{};

  std::string text() const {
    const long value = range.clamp(var->get());
    const long step = range.step > 0 ? range.step : 1;
    const auto index = static_cast<std::size_t>((value - range.min) / step);
    return index < labels.size() ? std::string(labels[index]) : std::to_string(value) + unit;
  }
};

} // namespace psx::game
