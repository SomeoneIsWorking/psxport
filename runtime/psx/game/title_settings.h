// An integer setting a title offers in the settings menu; the title owns the knob, the settings file keeps it by name.
#pragma once

#include "config_var.h"
#include "stepped_range.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace psx::game {

struct TitleIntSetting {
  const char *id;    // menu row id, unique within the title
  const char *label; // row text, or the part's name in a shared row
  const char *unit;  // appended to the value, e.g. "%"
  psx::config::Var<long> *var;
  psx::config::SteppedRange range;
  // A choice's names, one per step from range.min; empty shows the number and unit.
  std::span<const char *const> labels{};
  // A row shared with the neighbouring settings that name the same row: one row per item, each setting a part of it.
  const char *row = nullptr;

  std::string text() const {
    const long value = range.clamp(var->get());
    const long step = range.step > 0 ? range.step : 1;
    const auto index = static_cast<std::size_t>((value - range.min) / step);
    return index < labels.size() ? std::string(labels[index]) : std::to_string(value) + unit;
  }
  // Off is the range's first step.
  bool on() const {
    return range.clamp(var->get()) != range.min;
  }
  // Pressed: one step up, past the top back to the first.
  void cycle() const {
    var->set(range.cycled(var->get()), psx::config::Layer::Value);
  }
};

// The settings of one row: a setting alone, or the run of neighbours naming the same row.
struct TitleSettingRow {
  std::span<const TitleIntSetting> parts;

  const char *label() const {
    return parts.front().row != nullptr ? parts.front().row : parts.front().label;
  }
  bool shared() const {
    return parts.front().row != nullptr;
  }
};

// The row that starts at `first` in `settings`.
inline TitleSettingRow titleSettingRowAt(std::span<const TitleIntSetting> settings, std::size_t first) {
  std::size_t end = first + 1;
  const char *row = settings[first].row;
  if (row != nullptr) {
    while (end < settings.size() && settings[end].row != nullptr && std::string_view(settings[end].row) == row) {
      ++end;
    }
  }
  return {settings.subspan(first, end - first)};
}

// `visit(row)` for each row of `settings`, in order.
template <typename Visit> void forEachTitleSettingRow(std::span<const TitleIntSetting> settings, Visit visit) {
  for (std::size_t first = 0; first < settings.size();) {
    const TitleSettingRow row = titleSettingRowAt(settings, first);
    visit(row);
    first += row.parts.size();
  }
}

} // namespace psx::game
