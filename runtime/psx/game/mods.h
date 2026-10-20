// class Mods: the live PC-native settings of one Game, persisted through the knob registry's settings file.
#pragma once

#include "title_settings.h"

#include <cstdint>
#include <span>

// Widescreen means a wider FOV, never a stretch; AUTO matches the live window aspect.
enum : std::uint8_t { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };

class Mods {
public:
  int aspect = ASPECT_4_3;
  int ires = 1; // 0 = AUTO (from the window height), 1..4 = fixed 1x..4x
  int fps60 = 0;
  // The title selector's layout: the selected panel's share of the width, the seam slant per width,
  // and the share of the remaining travel a width animation covers per frame.
  float pickerSelectedShare = 0.6f;
  float pickerSlantPerWidth = 0.1f;
  float pickerWidthResponse = 0.25f;

  // Declared before init() so the settings load clamps them.
  void declareTitleSettings(std::span<const psx::game::TitleIntSetting> settings) {
    titleSettings_ = settings;
  }
  // The settings file, then this run's PSXPORT_FPS60, which is never saved; idempotent.
  void init(bool interpolates);
  // The settings as they now stand, for a Game whose settings another Game's menu changed.
  void reload();
  void save() const;
  bool temporalInterpolationSupported() const {
    return temporalInterpolationSupported_;
  }

private:
  std::span<const psx::game::TitleIntSetting> titleSettings_;
  bool inited_ = false;
  bool temporalInterpolationSupported_ = false;
};
