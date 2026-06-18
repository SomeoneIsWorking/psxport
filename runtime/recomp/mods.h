#ifndef PSXPORT_MODS_H
#define PSXPORT_MODS_H
// Live PC-native mod toggles + params — the single source of truth shared by the renderer (gpu_vk.c)
// and the ImGui overlay (imgui_overlay.cpp). Seeded once from cfg (env) by mods_init(), then mutated
// LIVE by the overlay; the renderer reads these every frame so a toggle takes effect immediately.
// (60fps is the extern int g_fps60_on in engine/fps60.c — the overlay flips it directly.)
#ifdef __cplusplus
extern "C" {
#endif
// Aspect mode for the PC-native widescreen (wider FOV, no stretch). AUTO = match the live window aspect.
enum { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };
typedef struct {
  int   ui;            // overlay system enabled (PSXPORT_UI): forces native-depth + the deferred infra on
  int   aspect;        // ASPECT_4_3 / _16_9 / _21_9 / _AUTO (widescreen = wider FOV; not a present stretch)
  int   ires;          // internal resolution scale (1..3; capped by VRAM_W=1024 / current FB width)
  int   ires_auto;     // derive ires from the live window height (~round(h/240)), clamped to the width cap
  int   ssao;          // ambient occlusion
  int   light;         // directional light
  float ssao_strength, ssao_radius, ssao_bias, ssao_range;
  float light_dir[3], light_ambient, light_diffuse;
} ModState;
extern ModState g_mods;
void mods_init(void);  // populate once: settings file (if present) else cfg/env (idempotent)
void mods_save(void);  // persist the live settings to the settings file (called by the overlay on change)
void mods_load(void);  // load the settings file into g_mods if it exists (called by mods_init)
int  g_fps60_on_get(void); // fps60 state for persistence (engine/fps60.c owns g_fps60_on)
#ifdef __cplusplus
}
#endif
#endif
