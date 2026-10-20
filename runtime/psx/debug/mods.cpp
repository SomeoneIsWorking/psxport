// Mods — per-Game mod state (see mods.h). Settings persistence: this is a PC GAME, the in-overlay
// choices (aspect, internal res, 60fps, their params) are written to a settings file and
// restored next launch. Path: PSXPORT_SETTINGS or ./psxport_settings.ini (gitignored). Saved on every
// overlay change; loaded by init().
#include "mods.h"
#include "config_vars.h"
#include "number_text.h"
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace {

// Is `path` inside a source checkout (an ancestor holds `.git`)? Saves never land in a tracked tree.
bool insideCheckout(const char *path) {
  if (path == nullptr || *path == '\0') {
    return false;
  }
  std::error_code error;
  for (std::filesystem::path dir = std::filesystem::path(path); !dir.empty(); dir = dir.parent_path()) {
    if (std::filesystem::exists(dir / ".git", error)) {
      return true;
    }
    if (dir == dir.parent_path()) {
      return false;
    }
  }
  return false;
}

} // namespace

const psx::game::TitleIntSetting *Mods::findTitleSetting(const char *key) const {
  for (const psx::game::TitleIntSetting &setting : mTitleSettings) {
    if (strcmp(setting.var->name(), key) == 0) {
      return &setting;
    }
  }
  return nullptr;
}

void Mods::save() const {
  const std::string settingsPath = psx::config::settings_path();
  const char *path = settingsPath.c_str();
  if (insideCheckout(path)) {
    lucent::warn("mods",
                 "REFUSED to save settings into a source checkout: {}. Settings belong in the OS "
                 "user-data location, never the checkout — a save here overwrites a tracked file. "
                 "Point PSXPORT_SETTINGS at a user-data path, or unset it to use the default.",
                 path);
    return;
  }
  FILE *f = fopen(path, "w");
  if (!f) {
    return;
  }
  // A launch PSXPORT_FPS60 is one run's choice; the player's saved one stays in the file.
  const int savedFps60 = mLaunchFps60 ? mSavedFps60 : fps60;
  fprintf(f, "aspect=%d\nires=%d\n", aspect, ires);
  fprintf(f,
          "picker_selected_share=%g\npicker_slant=%g\npicker_width_response=%g\n",
          static_cast<double>(pickerSelectedShare),
          static_cast<double>(pickerSlantPerWidth),
          static_cast<double>(pickerWidthResponse));
  for (const psx::game::TitleIntSetting &setting : mTitleSettings) {
    fprintf(f, "%s=%ld\n", setting.var->name(), setting.var->value_for_save());
  }
  if (mTemporalInterpolationSupported) {
    fprintf(f, "fps60=%d\n", savedFps60 != 0 ? 1 : 0);
  }
  fclose(f);
}

void Mods::load() {
  const std::string settingsPath = psx::config::settings_path();
  FILE *f = fopen(settingsPath.c_str(), "r");
  if (!f) {
    return;
  }
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '=');
    if (!eq) {
      continue;
    }
    *eq = 0;
    const char *k = line;
    std::string_view text(eq + 1);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
      text.remove_suffix(1);
    }
    const int v = psx::config::parseInt(text).value_or(0);
    if (!strcmp(k, "aspect")) {
      aspect = v;
    } else if (!strcmp(k, "ires")) {
      ires = v;
    }
    // Legacy compat: the old two-field shape (ires 1..3 + ires_auto bool). If a pre-merge settings
    // file still carries ires_auto=1, map it to the merged AUTO convention (ires=0).
    else if (!strcmp(k, "ires_auto")) {
      if (v != 0) {
        ires = 0;
      }
    } else if (!strcmp(k, "picker_selected_share")) {
      pickerSelectedShare = psx::config::parseFloat(text).value_or(pickerSelectedShare);
    } else if (!strcmp(k, "picker_slant")) {
      pickerSlantPerWidth = psx::config::parseFloat(text).value_or(pickerSlantPerWidth);
    } else if (!strcmp(k, "picker_width_response")) {
      pickerWidthResponse = psx::config::parseFloat(text).value_or(pickerWidthResponse);
    } else if (!strcmp(k, "fps60")) {
      if (mTemporalInterpolationSupported) {
        mSavedFps60 = v != 0 ? 1 : 0;
      } else if (v != 0) {
        lucent::warn(
            "mods", "{}: fps60=1 REFUSED — this title declares no temporal interpolation product", settingsPath);
      }
    }
    // A title setting goes to its CVar's VALUE layer: an environment Override still wins.
    else if (const psx::game::TitleIntSetting *setting = findTitleSetting(k)) {
      setting->var->set(psx::config::Layer::Value, setting->range.clamp(psx::config::parseSigned(text).value_or(0)));
    }
    // A line that configures nothing is reported, never dropped silently.
    else {
      lucent::warn("mods", "{}: unknown key \"{}\" ignored — it configures nothing", settingsPath, k);
    }
  }
  fclose(f);
  if (ires < 0) {
    ires = 0;
  }
  if (ires > 4) {
    ires = 4; // 0=Auto, 1..4 = 1x..4x
  }
  if (aspect < 0 || aspect > ASPECT_AUTO) {
    aspect = ASPECT_4_3;
  }
}

void Mods::init(bool interpolates, std::optional<bool> launchFps60) {
  if (mInited) {
    return;
  }
  mInited = true;
  mTemporalInterpolationSupported = interpolates;
  // One PC-native build: every other visual enhancement starts OFF (the in-class initializers are
  // the factory state). The F1 overlay toggles them LIVE and persists the choice to the settings
  // file, restored next launch.
  ui = 1; // overlay always available
  load(); // the player's persisted choices win over the factory defaults
  mLaunchFps60 = launchFps60.has_value();
  const bool requestedFps60 = mLaunchFps60 ? *launchFps60 : mSavedFps60 != 0;
  const char *source = mLaunchFps60 ? "PSXPORT_FPS60" : "settings";
  fps60 = mTemporalInterpolationSupported && requestedFps60 ? 1 : 0;
  if (!mTemporalInterpolationSupported && requestedFps60) {
    lucent::warn("fps60",
                 "interpolated 60fps REFUSED — this title declares no temporal interpolation product "
                 "(request source: {})",
                 source);
  }
  if (fps60) {
    lucent::info("fps60", "TRUE per-object interpolated 60fps ON (source: {})", source);
  }
}
