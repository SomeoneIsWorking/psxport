// Mods — per-Game mod state (see mods.h). Settings persistence: this is a PC GAME, the in-overlay
// choices (aspect, internal res, SSAO/light, 60fps, their params) are written to a settings file and
// restored next launch. Path: PSXPORT_SETTINGS or ./psxport_settings.ini (gitignored). Saved on every
// overlay change; loaded by init().
#include "mods.h"
#include "cfg.h"
#include "config.h"
#include "config_vars.h"
#include <lucent/log.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// PSXPORT_SETTINGS is a CVar with the filename as its DEFAULT, so the `(p && *p) ? p : "..."`
// fallback disappears — an unset environment variable simply leaves the CVar on its Default layer.
static const char* mods_path(void) { return psx::config::cv_settings_path.get().c_str(); }

void Mods::save() const {
  FILE* f = fopen(mods_path(), "w"); if (!f) return;
  // fps60 is on the CVar ladder. The overlay writes the plain member, so fold it back into the VALUE
  // layer here — but only when nothing above Value is in force. A PSXPORT_FPS60 in the environment
  // is a launch argument: persisting it would turn one run's flag into the player's saved setting,
  // and they would never find out where it came from. Dusklight's getValueForSave, same reason.
  if (psx::config::cv_fps60.layer() < psx::config::Layer::Override)
    psx::config::cv_fps60.set(psx::config::Layer::Value, fps60 != 0);
  fprintf(f, "aspect=%d\nires=%d\nssao=%d\nlight=%d\nshadows=%d\nfps60=%d\n",
          aspect, ires, ssao, light, shadows, psx::config::cv_fps60.value_for_save() ? 1 : 0);
  fprintf(f, "ssao_strength=%g\nssao_radius=%g\nssao_bias=%g\nssao_range=%g\nshadow_strength=%g\n",
          ssao_strength, ssao_radius, ssao_bias, ssao_range, shadow_strength);
  fprintf(f, "light_dir=%g,%g,%g\nlight_ambient=%g\nlight_diffuse=%g\n",
          light_dir[0], light_dir[1], light_dir[2], light_ambient, light_diffuse);
  fclose(f);
}

void Mods::load() {
  FILE* f = fopen(mods_path(), "r"); if (!f) return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
    const char* k = line; const char* v = eq + 1;
    if      (!strcmp(k, "aspect"))        aspect = atoi(v);
    else if (!strcmp(k, "ires"))          ires = atoi(v);
    // Legacy compat: the old two-field shape (ires 1..3 + ires_auto bool). If a pre-merge settings
    // file still carries ires_auto=1, map it to the merged AUTO convention (ires=0).
    else if (!strcmp(k, "ires_auto"))     { if (atoi(v)) ires = 0; }
    else if (!strcmp(k, "ssao"))          ssao = atoi(v);
    else if (!strcmp(k, "light"))         light = atoi(v);
    else if (!strcmp(k, "shadows"))       shadows = atoi(v);
    else if (!strcmp(k, "shadow_strength")) shadow_strength = (float)atof(v);
    // fps60 goes to the CVar's VALUE layer, not straight to the member: an env Override must beat
    // the settings file, and that decision belongs to the ladder rather than to load order here.
    else if (!strcmp(k, "fps60"))         psx::config::cv_fps60.set(psx::config::Layer::Value, atoi(v) != 0);
    else if (!strcmp(k, "ssao_strength")) ssao_strength = (float)atof(v);
    else if (!strcmp(k, "ssao_radius"))   ssao_radius = (float)atof(v);
    else if (!strcmp(k, "ssao_bias"))     ssao_bias = (float)atof(v);
    else if (!strcmp(k, "ssao_range"))    ssao_range = (float)atof(v);
    else if (!strcmp(k, "light_dir"))     sscanf(v, "%f,%f,%f", &light_dir[0], &light_dir[1], &light_dir[2]);
    else if (!strcmp(k, "light_ambient")) light_ambient = (float)atof(v);
    else if (!strcmp(k, "light_diffuse")) light_diffuse = (float)atof(v);
    // A key this loader does not recognise used to fall off the end of the chain and vanish. That is
    // the settings-file half of exactly the same bug as PSXPORT_FPS60: a line the user (or a past
    // version) wrote, silently doing nothing, with the file still looking like it configured
    // something. Say it, and name the file — this is also the first thing you see if `save()` has
    // ever written a key `load()` cannot read back.
    else lucent::warn("mods", "{}: unknown key \"{}\" ignored — it configures nothing", mods_path(), k);
  }
  fclose(f);
  if (ires < 0) ires = 0; if (ires > 4) ires = 4;   // 0=Auto, 1..4 = 1x..4x
  if (aspect < 0 || aspect > ASPECT_AUTO) aspect = ASPECT_4_3;
}

void Mods::init() {
  if (mInited) return;
  mInited = true;
  // One PC-native build: every visual enhancement starts OFF (the in-class initializers are the
  // factory state). The F1 overlay toggles them LIVE and persists the choice to the settings file,
  // restored next launch.
  ui = 1;                 // overlay always available (live-toggle + the deferred SSAO/light infra)
  load();                 // the player's persisted choices win over the factory defaults
  // ...and PSXPORT_FPS60 wins over those. THIS LINE IS THE WHOLE POINT OF THE CVar WORK: the
  // variable has been in docs/config.md since it was written, has been set on real runs, and until
  // now was read by NOTHING — a run configured with it was byte-identical to a run without it, with
  // no message either way. It resolves through the ladder now, and cfg_dump()'s report says which
  // layer the answer came from.
  fps60 = psx::config::cv_fps60.get() ? 1 : 0;
  if (fps60)
    lucent::info("fps60", "TRUE per-object interpolated 60fps ON (source: {})",
                 psx::config::layer_name(psx::config::cv_fps60.layer()));
}
