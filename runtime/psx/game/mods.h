// class Mods: the live PC-native settings of one Game, persisted through the knob registry's settings file.
#pragma once

#include "title_settings.h"

#include <cstdint>
#include <span>

// Widescreen means a wider FOV, never a stretch; AUTO matches the live window aspect.
enum : std::uint8_t { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };
// How a wide aspect is reached: the guest widens its view, or the 4:3 picture is cropped to the aspect (plan 0203).
enum : std::uint8_t { WIDESCREEN_EXTEND = 0, WIDESCREEN_CROP = 1 };

class Mods {
public:
  int aspect = ASPECT_4_3;
  int widescreen = WIDESCREEN_EXTEND;
  int ires = 1; // 0 = AUTO (from the window height), 1..4 = fixed 1x..4x
  int fps60 = 0;
  int depth = 0; // 1: the title's marked world geometry tests depth (present::DepthTest); 0 draws slot order only
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
  // Whether the title marks any producer for native depth (present::DepthTest), so the setting means something.
  void offerNativeDepth(bool marked) {
    nativeDepthOffered_ = marked;
  }
  bool nativeDepthOffered() const {
    return nativeDepthOffered_;
  }
  // Whether the title names its crop UI (present::CropUiLayers); without it the setting resolves to Extend.
  void offerCrop(bool named) {
    cropOffered_ = named;
  }
  bool cropOffered() const {
    return cropOffered_;
  }
  bool crops() const {
    return cropOffered_ && widescreen == WIDESCREEN_CROP;
  }

private:
  std::span<const psx::game::TitleIntSetting> titleSettings_;
  bool inited_ = false;
  bool temporalInterpolationSupported_ = false;
  bool nativeDepthOffered_ = false;
  bool cropOffered_ = false;
};
