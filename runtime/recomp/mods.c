// Live mod state — seeded from cfg (env) once; thereafter the ImGui overlay owns it. See mods.h.
#include "mods.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

ModState g_mods;
static int s_done = 0;
extern int g_fps60_on;   // engine/fps60.c — 60fps interpolation gate (persisted with the mods)
int g_fps60_on_get(void) { return g_fps60_on; }

// Settings persistence — this is a PC GAME: the in-overlay choices (aspect, internal res, SSAO/light,
// 60fps, their params) are written to a settings file and restored next launch. Path: PSXPORT_SETTINGS
// or ./psxport_settings.ini (gitignored). Saved on every overlay change; loaded by mods_init for windowed
// runs (headless/tooling stays env-driven + deterministic, so the render-diff harness is unaffected).
static const char* mods_path(void) { const char* p = cfg_str("PSXPORT_SETTINGS"); return (p && *p) ? p : "psxport_settings.ini"; }

void mods_save(void) {
  FILE* f = fopen(mods_path(), "w"); if (!f) return;
  fprintf(f, "aspect=%d\nires=%d\nires_auto=%d\nssao=%d\nlight=%d\nshadows=%d\nfps60=%d\n",
          g_mods.aspect, g_mods.ires, g_mods.ires_auto, g_mods.ssao, g_mods.light, g_mods.shadows, g_fps60_on);
  fprintf(f, "ssao_strength=%g\nssao_radius=%g\nssao_bias=%g\nssao_range=%g\nshadow_strength=%g\n",
          g_mods.ssao_strength, g_mods.ssao_radius, g_mods.ssao_bias, g_mods.ssao_range, g_mods.shadow_strength);
  fprintf(f, "light_dir=%g,%g,%g\nlight_ambient=%g\nlight_diffuse=%g\n",
          g_mods.light_dir[0], g_mods.light_dir[1], g_mods.light_dir[2], g_mods.light_ambient, g_mods.light_diffuse);
  fclose(f);
}

void mods_load(void) {
  FILE* f = fopen(mods_path(), "r"); if (!f) return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
    const char* k = line; const char* v = eq + 1;
    if      (!strcmp(k, "aspect"))        g_mods.aspect = atoi(v);
    else if (!strcmp(k, "ires"))          g_mods.ires = atoi(v);
    else if (!strcmp(k, "ires_auto"))     g_mods.ires_auto = atoi(v);
    else if (!strcmp(k, "ssao"))          g_mods.ssao = atoi(v);
    else if (!strcmp(k, "light"))         g_mods.light = atoi(v);
    else if (!strcmp(k, "shadows"))       g_mods.shadows = atoi(v);
    else if (!strcmp(k, "shadow_strength")) g_mods.shadow_strength = (float)atof(v);
    else if (!strcmp(k, "fps60"))         g_fps60_on = atoi(v);
    else if (!strcmp(k, "ssao_strength")) g_mods.ssao_strength = (float)atof(v);
    else if (!strcmp(k, "ssao_radius"))   g_mods.ssao_radius = (float)atof(v);
    else if (!strcmp(k, "ssao_bias"))     g_mods.ssao_bias = (float)atof(v);
    else if (!strcmp(k, "ssao_range"))    g_mods.ssao_range = (float)atof(v);
    else if (!strcmp(k, "light_dir"))     sscanf(v, "%f,%f,%f", &g_mods.light_dir[0], &g_mods.light_dir[1], &g_mods.light_dir[2]);
    else if (!strcmp(k, "light_ambient")) g_mods.light_ambient = (float)atof(v);
    else if (!strcmp(k, "light_diffuse")) g_mods.light_diffuse = (float)atof(v);
  }
  fclose(f);
  if (g_mods.ires < 1) g_mods.ires = 1; if (g_mods.ires > 3) g_mods.ires = 3;
  if (g_mods.aspect < 0 || g_mods.aspect > ASPECT_AUTO) g_mods.aspect = ASPECT_4_3;
}

void mods_init(void) {
  if (s_done) return;
  s_done = 1;
  // One PC-native build: every visual enhancement starts OFF. The F1 overlay toggles them LIVE and
  // persists the choice to the settings file, restored next launch. No env gating — the defaults below
  // are the factory state; mods_load() (if the file exists) overrides them with the player's choices.
  g_mods.ui = 1;                 // overlay always available (live-toggle + the deferred SSAO/light infra)
  g_mods.aspect = ASPECT_4_3;    // 4:3 until the player picks widescreen
  g_mods.ires = 1;               // native internal resolution
  g_mods.ires_auto = 0;
  g_mods.ssao = 0;
  g_mods.light = 0;
  g_mods.shadows = 0;            // dynamic shadow mapping off by default (toggled live in the overlay)
  g_mods.shadow_strength = 0.6f; // how far a shadowed pixel drops toward ambient
  g_mods.ssao_strength = 1.0f; g_mods.ssao_radius = 5.0f; g_mods.ssao_bias = 0.01f; g_mods.ssao_range = 0.15f;
  g_mods.light_dir[0] = -0.4f; g_mods.light_dir[1] = -0.7f; g_mods.light_dir[2] = -0.5f;
  g_mods.light_ambient = 0.65f; g_mods.light_diffuse = 0.5f;
  mods_load();                   // the player's persisted choices win over the factory defaults
}
