#pragma once

#include <cstdint>

#include "title_settings.h"

#include <optional>
#include <span>
// class Mods — live PC-native mod toggles + params, PER-GAME state (member `Game::mods`).
// The single source of truth shared by the renderer (gpu_vk.cpp) and the RmlUi overlay
// (rmlui_overlay.cpp): seeded by init() in the Game ctor (factory defaults, then the settings
// file), mutated LIVE by the overlay, read every frame by the renderer. Per-instance so two
// Games in one process (SBS) keep independent enhancement state — the harness pins both cores
// factory-neutral and the oracle core is additionally forced pure (Game::setOracle).
// (Was the process-global `g_mods` C struct; deglobalized 2026-07-10.)

// Aspect mode for the PC-native widescreen (wider FOV, no stretch). AUTO = match the live window aspect.
enum : std::uint8_t { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };

class Mods {
public:
  int ui = 0;     // overlay system enabled (always on)
  int aspect = 0; // ASPECT_4_3 / _16_9 / _21_9 / _AUTO (widescreen = wider FOV; not a present stretch)
  int ires = 1;   // internal resolution scale: 0 = AUTO (derive from window height), 1..4 = fixed
                  // Vanilla(1x)/X2/X3/X4. Capped by psx::gpu::videoStatus.
  int fps60 = 0;  // interpolated 60fps tier
  // The title's own integer settings, persisted beside the fields above. Declared before init() so the
  // settings-file load reads them.
  void declareTitleSettings(std::span<const psx::game::TitleIntSetting> settings) {
    mTitleSettings = settings;
  }
  // The title's interpolation answer, then the settings file, then this run's launch choice of 60 fps,
  // which is never saved (idempotent).
  void init(bool interpolates, std::optional<bool> launchFps60);
  void save() const; // persist the live settings (called by the overlay on change)
  void load();       // load the settings file over the current values, if it exists
  bool temporalInterpolationSupported() const {
    return mTemporalInterpolationSupported;
  }

private:
  const psx::game::TitleIntSetting *findTitleSetting(const char *key) const;
  std::span<const psx::game::TitleIntSetting> mTitleSettings;
  bool mInited = false;
  bool mLaunchFps60 = false; // fps60 came from the launch config, so save keeps mSavedFps60
  int mSavedFps60 = 0;
  bool mTemporalInterpolationSupported = false;
};
