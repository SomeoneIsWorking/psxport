// Mods — per-Game mod state (see mods.h). Settings persistence: this is a PC GAME, the in-overlay
// choices (aspect, internal res, 60fps, their params) are written to a settings file and
// restored next launch. Path: PSXPORT_SETTINGS or ./psxport_settings.ini (gitignored). Saved on every
// overlay change; loaded by init().
#include "mods.h"
#include "config_vars.h"
#include "render_capabilities.h"
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PSXPORT_SETTINGS is a CVar with the filename as its DEFAULT, so the `(p && *p) ? p : "..."`
// fallback disappears — an unset environment variable simply leaves the CVar on its Default layer.
static const char *mods_path(void) {
  return psx::config::cv_settings_path.get().c_str();
}

namespace {

// Is `path` inside a source checkout? Walks up looking for a `.git` entry, which is what makes a
// directory a working tree. Returns the directory it stopped at, or nullptr.
//
// WHY THIS EXISTS, AND IT IS A MEASURED INCIDENT. `PSXPORT_SETTINGS` is BOTH the launch configuration
// input and the persistence target, and every agent run and the launcher point it at a TRACKED file —
// `spyro/tools/shipping_settings.ini`. So opening the options overlay and changing anything called
// `Mods::save()`, which rewrote that tracked file with a full serialisation of the live mod state.
//
// Measured 2026-09-27: `spyro/tools/shipping_settings.ini` was found rewritten from `aspect=1` to
// `aspect=3` with 21 lines of documentation deleted, mtime coincident with a player run. `aspect=3` is
// ASPECT_AUTO, which resolves to the SINK's aspect — and a headless agent run has no wide sink, so it
// resolves to 4:3. **Every subsequent run, and the player's own run, silently lost widescreen**, and the
// comment explaining precisely that hazard was the thing that got deleted.
//
// The fix is not "stop pointing at it": the agent runs NEED the tracked file as their configuration
// input, and that is the whole reason it is tracked. The fix is that a save must never LAND in a
// checkout. Reading configuration from the tree is fine; writing a player's saved state into it is not,
// and the standing rule is explicit that settings live in the OS user-data location, never the checkout.
// IT ANSWERS YES/NO, AND THAT IS A FIX RATHER THAN A SIMPLIFICATION. This used to return
// `const char *` pointing at its own local `buffer`, which is undefined behaviour — the object dies
// at return, so the pointer is dangling. Clang tolerated that and GCC 16.2.1 at -O2 did not: it
// concluded the return value could never be usefully non-null and optimized the entire ancestor walk
// away, so the guard silently stopped refusing and `Mods::save()` overwrote a tracked file in a
// source checkout again — the exact incident this function was written to prevent (006eb917).
//
// The project's OWN test is what found it, and the way it found it is the point: `test_settings_-
// persistence` is 4/4 green under Clang and 2/4 red under GCC, same source, same flags bar the
// compiler. A guard that only works on the toolchain the agents happen to use is not a guard, and
// `AGENTS.md` requires the project to stay compatible with its supported GCC, Clang and AppleClang
// toolchains. The discarded `(void)root` at the only call site means nothing is lost by this change.
// A future caller that needs the directory must be handed a caller-owned buffer, never a pointer
// into this frame.
bool insideCheckout(const char *path) {
  if (path == nullptr || *path == '\0') {
    return false;
  }
  char buffer[4096];
  const size_t length = strlen(path);
  if (length + 1 >= sizeof(buffer)) {
    return false; // too long to walk safely; do not guess
  }
  memcpy(buffer, path, length + 1);
  for (;;) {
    char probe[4200];
    const size_t dirLength = strlen(buffer);
    if (dirLength + 6 >= sizeof(probe)) {
      return false;
    }
    memcpy(probe, buffer, dirLength);
    memcpy(probe + dirLength, "/.git", 6);
    FILE *git = fopen(probe, "r");
    if (git != nullptr) {
      fclose(git);
      return true;
    }
    char *slash = strrchr(buffer, '/');
    if (slash == nullptr) {
      return false; // reached the filesystem root without finding one
    }
    if (slash == buffer) {
      return false; // buffer is now "/" and its parent is itself
    }
    *slash = '\0';
  }
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
  const char *path = mods_path();
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
  fprintf(f, "aspect=%d\nires=%d\nface_order=%d\n", aspect, ires, face_order);
  for (const psx::game::TitleIntSetting &setting : mTitleSettings) {
    fprintf(f, "%s=%ld\n", setting.var->name(), setting.var->value_for_save());
  }
  if (mTemporalInterpolationSupported) {
    fprintf(f, "fps60=%d\n", savedFps60 != 0 ? 1 : 0);
  }
  fclose(f);
}

void Mods::load() {
  FILE *f = fopen(mods_path(), "r");
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
    const char *v = eq + 1;
    if (!strcmp(k, "aspect")) {
      aspect = atoi(v);
    } else if (!strcmp(k, "ires")) {
      ires = atoi(v);
    }
    // Legacy compat: the old two-field shape (ires 1..3 + ires_auto bool). If a pre-merge settings
    // file still carries ires_auto=1, map it to the merged AUTO convention (ires=0).
    else if (!strcmp(k, "ires_auto")) {
      if (atoi(v)) {
        ires = 0;
      }
    } else if (!strcmp(k, "face_order")) {
      face_order = atoi(v);
    } else if (!strcmp(k, "fps60")) {
      if (mTemporalInterpolationSupported) {
        mSavedFps60 = atoi(v) != 0 ? 1 : 0;
      } else if (atoi(v) != 0) {
        lucent::warn(
            "mods", "{}: fps60=1 REFUSED — this title declares no temporal interpolation product", mods_path());
      }
    }
    // A title setting goes to its CVar's VALUE layer: an environment Override still wins.
    else if (const psx::game::TitleIntSetting *setting = findTitleSetting(k)) {
      setting->var->set(psx::config::Layer::Value, setting->range.clamp(atol(v)));
    }
    // A line that configures nothing is reported, never dropped silently.
    else {
      lucent::warn("mods", "{}: unknown key \"{}\" ignored — it configures nothing", mods_path(), k);
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

void Mods::init(bool interpolates, FaceOrder defaultFaceOrder, std::optional<bool> launchFps60) {
  if (mInited) {
    return;
  }
  mInited = true;
  mTemporalInterpolationSupported = interpolates;
  // The title owns the factory answer for OT-authored versus native per-pixel depth. Seed it before
  // load so an explicit persisted player choice remains the higher-precedence answer.
  face_order = defaultFaceOrder;
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
