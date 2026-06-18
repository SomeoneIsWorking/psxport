// Live mod state — seeded from cfg (env) once; thereafter the ImGui overlay owns it. See mods.h.
#include "mods.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

ModState g_mods;
static int s_done = 0;

static float fdef(const char* name, float d) { const char* s = cfg_str(name); return s ? (float)atof(s) : d; }

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
}
