#ifndef PSXPORT_MODS_H
#define PSXPORT_MODS_H
// Live PC-native mod toggles + params — the single source of truth shared by the renderer (gpu_gpu.c)
// and the RmlUi overlay (rmlui_overlay.cpp). Seeded once from cfg (env) by mods_init(), then mutated
// LIVE by the overlay; the renderer reads these every frame so a toggle takes effect immediately.
// (60fps is g_mods.fps60 as of 2026-07-02 — was the process-global g_fps60_on, deglobalize-game.)
#ifdef __cplusplus
extern "C" {
#endif
// Aspect mode for the PC-native widescreen (wider FOV, no stretch). AUTO = match the live window aspect.
enum { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };
typedef struct {
  int   ui;            // overlay system enabled (always on): keeps the deferred SSAO/light infra built
  int   aspect;        // ASPECT_4_3 / _16_9 / _21_9 / _AUTO (widescreen = wider FOV; not a present stretch)
  int   ires;          // internal resolution scale: 0 = AUTO (derive from window height), 1..4 = fixed
                       // Vanilla(1x)/X2/X3/X4. Capped by VRAM_W=1024 / current FB width (gpu_gpu_video_status).
  int   ssao;          // ambient occlusion
  int   light;         // directional light
  int   shadows;       // dynamic shadow mapping cast by the directional light (needs light on)
  int   fps60;         // interpolated 60fps tier (was extern int g_fps60_on; deglobalize-game 2026-07-02)
  float ssao_strength, ssao_radius, ssao_bias, ssao_range;
  float light_dir[3], light_ambient, light_diffuse;
  float shadow_strength;   // 0..1 darkening applied in shadow (1 = full drop to ambient)
  int   debug_ids;         // DEBUG: master objid overlay enable (legacy/global). Not persisted.
  int   debug_quads;       // DEBUG: box+label BILLBOARD objects (2D sprites at 3D positions). Not persisted.
  int   debug_objects;     // DEBUG: box+label 3D-MESH objects. Not persisted.
} ModState;
extern ModState g_mods;
void mods_init(void);  // populate once: settings file (if present) else cfg/env (idempotent)
void mods_save(void);  // persist the live settings to the settings file (called by the overlay on change)
void mods_load(void);  // load the settings file into g_mods if it exists (called by mods_init)
// (g_fps60_on_get retired — read g_mods.fps60 directly)
#ifdef __cplusplus
}
#endif
#endif
