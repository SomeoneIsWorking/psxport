// Mods — per-Game mod state (see mods.h). Settings persistence: this is a PC GAME, the in-overlay
// choices (aspect, internal res, SSAO/light, 60fps, their params) are written to a settings file and
// restored next launch. Path: PSXPORT_SETTINGS or ./psxport_settings.ini (gitignored). Saved on every
// overlay change; loaded by init().
#include "mods.h"
#include "cfg.h"
#include "config.h"
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
  // fps60 is on the CVar ladder. The overlay writes the plain member, so fold it back into the VALUE
  // layer here — but only when nothing above Value is in force. A PSXPORT_FPS60 in the environment
  // is a launch argument: persisting it would turn one run's flag into the player's saved setting,
  // and they would never find out where it came from.
  if (mTemporalInterpolationSupported && psx::config::cv_fps60.layer() < psx::config::Layer::Override) {
    psx::config::cv_fps60.set(psx::config::Layer::Value, fps60 != 0);
  }
  fprintf(f,
          "aspect=%d\nires=%d\nface_order=%d\nssao=%d\nlight=%d\nshadows=%d\n",
          aspect,
          ires,
          face_order,
          ssao,
          light,
          shadows);
  if (mTemporalInterpolationSupported) {
    fprintf(f, "fps60=%d\n", psx::config::cv_fps60.value_for_save() ? 1 : 0);
  }
  fprintf(f,
          "ssao_strength=%g\nssao_radius=%g\nssao_bias=%g\nssao_range=%g\nshadow_strength=%g\n",
          ssao_strength,
          ssao_radius,
          ssao_bias,
          ssao_range,
          shadow_strength);
  fprintf(f,
          "light_dir=%g,%g,%g\nlight_ambient=%g\nlight_diffuse=%g\n",
          light_dir[0],
          light_dir[1],
          light_dir[2],
          light_ambient,
          light_diffuse);
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
    } else if (!strcmp(k, "ssao")) {
      ssao = atoi(v);
    } else if (!strcmp(k, "light")) {
      light = atoi(v);
    } else if (!strcmp(k, "shadows")) {
      shadows = atoi(v);
    } else if (!strcmp(k, "shadow_strength")) {
      shadow_strength = (float)atof(v);
    }
    // fps60 goes to the CVar's VALUE layer, not straight to the member: an env Override must beat
    // the settings file, and that decision belongs to the ladder rather than to load order here.
    else if (!strcmp(k, "fps60")) {
      if (mTemporalInterpolationSupported) {
        psx::config::cv_fps60.set(psx::config::Layer::Value, atoi(v) != 0);
      } else if (atoi(v) != 0) {
        lucent::warn(
            "mods", "{}: fps60=1 REFUSED — this title declares no temporal interpolation product", mods_path());
      }
    } else if (!strcmp(k, "ssao_strength")) {
      ssao_strength = (float)atof(v);
    } else if (!strcmp(k, "ssao_radius")) {
      ssao_radius = (float)atof(v);
    } else if (!strcmp(k, "ssao_bias")) {
      ssao_bias = (float)atof(v);
    } else if (!strcmp(k, "ssao_range")) {
      ssao_range = (float)atof(v);
    } else if (!strcmp(k, "light_dir")) {
      sscanf(v, "%f,%f,%f", &light_dir[0], &light_dir[1], &light_dir[2]);
    } else if (!strcmp(k, "light_ambient")) {
      light_ambient = (float)atof(v);
    } else if (!strcmp(k, "light_diffuse")) {
      light_diffuse = (float)atof(v);
    }
    // A key this loader does not recognise used to fall off the end of the chain and vanish. That is
    // the settings-file half of exactly the same bug as PSXPORT_FPS60: a line the user (or a past
    // version) wrote, silently doing nothing, with the file still looking like it configured
    // something. Say it, and name the file — this is also the first thing you see if `save()` has
    // ever written a key `load()` cannot read back.
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

void Mods::init(const RenderCapabilities &capabilities) {
  if (mInited) {
    return;
  }
  mInited = true;
  mTemporalInterpolationSupported = capabilities.temporalInterpolation;
  // The title owns the factory answer for OT-authored versus native per-pixel depth. Seed it before
  // load so an explicit persisted player choice remains the higher-precedence answer.
  face_order = capabilities.defaultFaceOrder;
  // One PC-native build: every other visual enhancement starts OFF (the in-class initializers are
  // the factory state). The F1 overlay toggles them LIVE and persists the choice to the settings
  // file, restored next launch.
  ui = 1; // overlay always available (live-toggle + the deferred SSAO/light infra)
  load(); // the player's persisted choices win over the factory defaults
  // ...and PSXPORT_FPS60 wins over those. THIS LINE IS THE WHOLE POINT OF THE CVar WORK: the
  // variable has been in docs/config.md since it was written, has been set on real runs, and until
  // now was read by NOTHING — a run configured with it was byte-identical to a run without it, with
  // no message either way. It resolves through the ladder now, and cfg_dump()'s report says which
  // layer the answer came from.
  const bool requestedFps60 = psx::config::cv_fps60.get();
  fps60 = mTemporalInterpolationSupported && requestedFps60 ? 1 : 0;
  if (!mTemporalInterpolationSupported && requestedFps60) {
    lucent::warn("fps60",
                 "interpolated 60fps REFUSED — this title declares no temporal interpolation product "
                 "(request source: {})",
                 psx::config::layer_name(psx::config::cv_fps60.layer()));
  }
  if (fps60) {
    lucent::info("fps60",
                 "TRUE per-object interpolated 60fps ON (source: {})",
                 psx::config::layer_name(psx::config::cv_fps60.layer()));
  }
}
