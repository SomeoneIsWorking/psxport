#pragma once
// class Mods — live PC-native mod toggles + params, PER-GAME state (member `Game::mods`).
// The single source of truth shared by the renderer (gpu_gpu.cpp) and the RmlUi overlay
// (rmlui_overlay.cpp): seeded by init() in the Game ctor (factory defaults, then the settings
// file), mutated LIVE by the overlay, read every frame by the renderer. Per-instance so two
// Games in one process (SBS) keep independent enhancement state — the harness pins both cores
// factory-neutral and the oracle core is additionally forced pure (Game::setOracle).
// (Was the process-global `g_mods` C struct; deglobalized 2026-07-10.)

// Aspect mode for the PC-native widescreen (wider FOV, no stretch). AUTO = match the live window aspect.
enum { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };

class Mods {
public:
  int   ui = 0;        // overlay system enabled (always on): keeps the deferred SSAO/light infra built
  int   aspect = 0;    // ASPECT_4_3 / _16_9 / _21_9 / _AUTO (widescreen = wider FOV; not a present stretch)
  int   ires = 1;      // internal resolution scale: 0 = AUTO (derive from window height), 1..4 = fixed
                       // Vanilla(1x)/X2/X3/X4. Capped by VRAM_W=1024 / current FB width (gpu_gpu_video_status).
  int   ssao = 0;      // ambient occlusion
  int   light = 0;     // directional light
  int   shadows = 0;   // dynamic shadow mapping cast by the directional light (needs light on)
  int   fps60 = 0;     // interpolated 60fps tier
  float ssao_strength = 1.0f, ssao_radius = 5.0f, ssao_bias = 0.01f, ssao_range = 0.15f;
  float light_dir[3] = { -0.4f, -0.7f, -0.5f };
  float light_ambient = 0.65f, light_diffuse = 0.5f;
  float shadow_strength = 0.6f;   // 0..1 darkening applied in shadow (1 = full drop to ambient)
  int   debug_ids = 0;     // DEBUG: master objid overlay enable (legacy/global). Not persisted.
  int   debug_quads = 0;   // DEBUG: box+label BILLBOARD objects (2D sprites at 3D positions). Not persisted.
  int   debug_objects = 0; // DEBUG: box+label 3D-MESH objects. Not persisted.

  void init();           // factory defaults + settings-file load (idempotent)
  void save() const;     // persist the live settings (called by the overlay on change)
  void load();           // load the settings file over the current values, if it exists
  // Force the PSX-neutral state (4:3, 1x, every enhancement off). Used by Game::setOracle (the pure
  // PSX reference must not be touched by any enhancement) and by the SBS harness on BOTH cores (a
  // guest-poking enhancement — e.g. the widescreen cull re-include — on one core would break the
  // byte-exact gate by design, not by bug).
  void forceNeutral() { aspect = ASPECT_4_3; ires = 1; ssao = light = shadows = fps60 = 0; }

private:
  bool mInited = false;
};
