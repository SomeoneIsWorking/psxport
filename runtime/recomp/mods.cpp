// Mods — per-Game mod state (see mods.h). Settings persistence: this is a PC GAME, the in-overlay
// choices (aspect, internal res, SSAO/light, 60fps, their params) are written to a settings file and
// restored next launch. Path: PSXPORT_SETTINGS or ./psxport_settings.ini (gitignored). Saved on every
// overlay change; loaded by init().
#include "mods.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char* mods_path(void) { const char* p = cfg_str("PSXPORT_SETTINGS"); return (p && *p) ? p : "psxport_settings.ini"; }

void Mods::save() const {
  FILE* f = fopen(mods_path(), "w"); if (!f) return;
  fprintf(f, "aspect=%d\nires=%d\nssao=%d\nlight=%d\nshadows=%d\nfps60=%d\n",
          aspect, ires, ssao, light, shadows, fps60);
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
    else if (!strcmp(k, "fps60"))         fps60 = atoi(v);
    else if (!strcmp(k, "ssao_strength")) ssao_strength = (float)atof(v);
    else if (!strcmp(k, "ssao_radius"))   ssao_radius = (float)atof(v);
    else if (!strcmp(k, "ssao_bias"))     ssao_bias = (float)atof(v);
    else if (!strcmp(k, "ssao_range"))    ssao_range = (float)atof(v);
    else if (!strcmp(k, "light_dir"))     sscanf(v, "%f,%f,%f", &light_dir[0], &light_dir[1], &light_dir[2]);
    else if (!strcmp(k, "light_ambient")) light_ambient = (float)atof(v);
    else if (!strcmp(k, "light_diffuse")) light_diffuse = (float)atof(v);
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
  if (fps60) fprintf(stderr, "[fps60] TRUE per-object interpolated 60fps ON (overlay)\n");
}
