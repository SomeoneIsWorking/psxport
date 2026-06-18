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

static float fdef(const char* name, float d) { const char* s = cfg_str(name); return s ? (float)atof(s) : d; }

// Settings persistence — this is a PC GAME: the in-overlay choices (aspect, internal res, SSAO/light,
// 60fps, their params) are written to a settings file and restored next launch. Path: PSXPORT_SETTINGS
// or ./psxport_settings.ini (gitignored). Saved on every overlay change; loaded by mods_init for windowed
// runs (headless/tooling stays env-driven + deterministic, so the render-diff harness is unaffected).
static const char* mods_path(void) { const char* p = cfg_str("PSXPORT_SETTINGS"); return (p && *p) ? p : "psxport_settings.ini"; }

void mods_save(void) {
  FILE* f = fopen(mods_path(), "w"); if (!f) return;
  fprintf(f, "aspect=%d\nires=%d\nires_auto=%d\nssao=%d\nlight=%d\nfps60=%d\n",
          g_mods.aspect, g_mods.ires, g_mods.ires_auto, g_mods.ssao, g_mods.light, g_fps60_on);
  fprintf(f, "ssao_strength=%g\nssao_radius=%g\nssao_bias=%g\nssao_range=%g\n",
          g_mods.ssao_strength, g_mods.ssao_radius, g_mods.ssao_bias, g_mods.ssao_range);
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
  // Overlay (F1 menu) + its live-toggle infra: ON BY DEFAULT for a WINDOWED VK run so a plain ./run.sh
  // "just works" (no flag needed). OFF headless / software-GPU so the render-diff tooling stays faithful.
  // PSXPORT_UI=1 forces on, PSXPORT_UI=0 forces off, regardless of windowing.
  const char* uiv = cfg_str("PSXPORT_UI");
  if (uiv) g_mods.ui = atoi(uiv) != 0;
  else {
    int windowed = cfg_on("PSXPORT_GPU_WINDOW") && !cfg_on("PSXPORT_VK_HEADLESS") && !cfg_on("PSXPORT_SW_GPU");
    const char* vk = cfg_str("PSXPORT_VK");
    if (vk && atoi(vk) == 0) windowed = 0;   // VK explicitly disabled -> software GPU -> no overlay
    g_mods.ui = windowed ? 1 : 0;
  }
  // Aspect: PSXPORT_ASPECT = 4:3 | 16:9 | 21:9 | auto (legacy PSXPORT_WIDE=1 => 16:9). Default 4:3.
  g_mods.aspect = ASPECT_4_3;
  const char* asp = cfg_str("PSXPORT_ASPECT");
  if (asp) {
    if (!strcmp(asp, "16:9")) g_mods.aspect = ASPECT_16_9;
    else if (!strcmp(asp, "21:9")) g_mods.aspect = ASPECT_21_9;
    else if (!strcmp(asp, "auto")) g_mods.aspect = ASPECT_AUTO;
    else if (!strcmp(asp, "4:3")) g_mods.aspect = ASPECT_4_3;
  } else if (cfg_on("PSXPORT_WIDE")) g_mods.aspect = ASPECT_16_9;
  // Internal res: PSXPORT_IRES = N (1..3) or "auto" (derive from window). Legacy PSXPORT_SS as alias.
  const char* ir = cfg_str("PSXPORT_IRES"); if (!ir) ir = cfg_str("PSXPORT_SS");
  g_mods.ires_auto = (ir && !strcmp(ir, "auto")) || cfg_on("PSXPORT_IRES_AUTO");
  g_mods.ires  = (ir && strcmp(ir, "auto")) ? atoi(ir) : 1;
  if (g_mods.ires < 1) g_mods.ires = 1; if (g_mods.ires > 3) g_mods.ires = 3;
  g_mods.ssao  = cfg_on("PSXPORT_SSAO");
  g_mods.light = cfg_on("PSXPORT_LIGHT");
  g_mods.ssao_strength = fdef("PSXPORT_SSAO_STRENGTH", 1.0f);
  g_mods.ssao_radius   = fdef("PSXPORT_SSAO_RADIUS", 5.0f);
  g_mods.ssao_bias     = fdef("PSXPORT_SSAO_BIAS", 0.01f);
  g_mods.ssao_range    = fdef("PSXPORT_SSAO_RANGE", 0.15f);
  g_mods.light_dir[0] = -0.4f; g_mods.light_dir[1] = -0.7f; g_mods.light_dir[2] = -0.5f;
  const char* d = cfg_str("PSXPORT_LIGHT_DIR");
  if (d) sscanf(d, "%f,%f,%f", &g_mods.light_dir[0], &g_mods.light_dir[1], &g_mods.light_dir[2]);
  g_mods.light_ambient = fdef("PSXPORT_LIGHT_AMBIENT", 0.65f);
  g_mods.light_diffuse = fdef("PSXPORT_LIGHT_DIFFUSE", 0.5f);
  // Restore persisted settings (PC game) for the interactive/windowed run — after env seeding so the saved
  // choices win. Headless/tooling (ui==0) stays env-driven + deterministic (render-diff harness unaffected).
  if (g_mods.ui) mods_load();
}
