// Live mod state — seeded from cfg (env) once; thereafter the ImGui overlay owns it. See mods.h.
#include "mods.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>

ModState g_mods;
static int s_done = 0;

static float fdef(const char* name, float d) { const char* s = cfg_str(name); return s ? (float)atof(s) : d; }

void mods_init(void) {
  if (s_done) return;
  s_done = 1;
  g_mods.ui    = cfg_on("PSXPORT_UI");
  g_mods.wide  = cfg_on("PSXPORT_WIDE");
  const char* ir = cfg_str("PSXPORT_IRES"); if (!ir) ir = cfg_str("PSXPORT_SS");
  g_mods.ires  = ir ? atoi(ir) : 1; if (g_mods.ires < 1) g_mods.ires = 1; if (g_mods.ires > 3) g_mods.ires = 3;
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
