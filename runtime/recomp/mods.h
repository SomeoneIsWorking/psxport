#ifndef PSXPORT_MODS_H
#define PSXPORT_MODS_H
// Live PC-native mod toggles + params — the single source of truth shared by the renderer (gpu_vk.c)
// and the ImGui overlay (imgui_overlay.cpp). Seeded once from cfg (env) by mods_init(), then mutated
// LIVE by the overlay; the renderer reads these every frame so a toggle takes effect immediately.
// (60fps is the extern int g_fps60_on in engine/fps60.c — the overlay flips it directly.)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  int   ui;            // overlay system enabled (PSXPORT_UI): forces native-depth + the deferred infra on
  int   wide;          // widescreen (wider FOV, no stretch)
  int   ires;          // internal resolution scale (1..3; capped to 2 in widescreen — VRAM_W limit)
  int   ssao;          // ambient occlusion
  int   light;         // directional light
  float ssao_strength, ssao_radius, ssao_bias, ssao_range;
  float light_dir[3], light_ambient, light_diffuse;
} ModState;
extern ModState g_mods;
void mods_init(void);  // populate from cfg once (idempotent; safe to call from any first-use site)
#ifdef __cplusplus
}
#endif
#endif
