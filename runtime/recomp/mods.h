#pragma once
// class Mods — live PC-native mod toggles + params, PER-GAME state (member `Game::mods`).
// The single source of truth shared by the renderer (gpu_vk.cpp) and the RmlUi overlay
// (rmlui_overlay.cpp): seeded by init() in the Game ctor (factory defaults, then the settings
// file), mutated LIVE by the overlay, read every frame by the renderer. Per-instance so two
// Games in one process (SBS) keep independent enhancement state — the harness pins both cores
// factory-neutral and the oracle core is additionally forced pure (Game::setOracle).
// (Was the process-global `g_mods` C struct; deglobalized 2026-07-10.)

// Aspect mode for the PC-native widescreen (wider FOV, no stretch). AUTO = match the live window aspect.
enum { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2, ASPECT_AUTO = 3 };

// How overlapping world faces are ordered — see Mods::face_order.
enum { FACE_ORDER_DEPTH = 0, FACE_ORDER_AUTHORED = 1 };

class Mods {
public:
  int ui = 0;      // overlay system enabled (always on): keeps the deferred SSAO/light infra built
  int aspect = 0;  // ASPECT_4_3 / _16_9 / _21_9 / _AUTO (widescreen = wider FOV; not a present stretch)
  int ires = 1;    // internal resolution scale: 0 = AUTO (derive from window height), 1..4 = fixed
                   // Vanilla(1x)/X2/X3/X4. Capped by VRAM_W=1024 / current FB width (gpu_vk_video_status).
  int ssao = 0;    // ambient occlusion
  int light = 0;   // directional light
  int shadows = 0; // dynamic shadow mapping cast by the directional light (needs light on)
  int fps60 = 0;   // interpolated 60fps tier
  // FACE_ORDER_DEPTH / FACE_ORDER_AUTHORED — how overlapping world faces are ordered.
  //
  // The PSX had no depth buffer: the game sorted its polygons into an ordering table (a bucket per
  // depth band, walked far to near), so the DRAW ORDER was the game's own authored answer. This port
  // draws with a real per-pixel depth buffer instead, which is BETTER for ordinary geometry — no
  // sorting pops, correct interpenetration — but occasionally contradicts what the artists relied on
  // (kanban #11: the water-pump barrel, where real depth picks an interior wall the game filed
  // BEHIND the water, and the barrel renders black).
  //
  // AUTHORED replays the bucket the game itself computed (submit.cpp game_sort_key, RE'd from the
  // guest's own submitter and verified equal to it) as the depth value, so the depth buffer stops
  // being a second opinion and reproduces the console's order. Ties inside a bucket use adjacent D32
  // values in AddPrim's head-insertion order: the guest walks later submissions first, so the earliest
  // submission paints last and wins.
  //
  // NOTE WHICH ONE IS THE ENHANCEMENT. Every other field here is off-is-faithful; this one is
  // inverted, because DEPTH is the port's improvement and AUTHORED is the console's behaviour. That
  // is why forceNeutral() below selects AUTHORED rather than 0.
  int face_order = 0;
  float ssao_strength = 1.0f, ssao_radius = 5.0f, ssao_bias = 0.01f, ssao_range = 0.15f;
  float light_dir[3] = {-0.4f, -0.7f, -0.5f};
  float light_ambient = 0.65f, light_diffuse = 0.5f;
  float shadow_strength = 0.6f; // 0..1 darkening applied in shadow (1 = full drop to ambient)
  int debug_ids = 0;            // DEBUG: master objid overlay enable (legacy/global). Not persisted.
  int debug_quads = 0;          // DEBUG: box+label BILLBOARD objects (2D sprites at 3D positions). Not persisted.
  int debug_objects = 0;        // DEBUG: box+label 3D-MESH objects. Not persisted.

  void init();       // factory defaults + settings-file load (idempotent)
  void save() const; // persist the live settings (called by the overlay on change)
  void load();       // load the settings file over the current values, if it exists
  // Force the PSX-neutral state (4:3, 1x, every enhancement off). Used by Game::setOracle (the pure
  // PSX reference must not be touched by any enhancement) and by the SBS harness on BOTH cores (a
  // guest-poking enhancement — e.g. the widescreen cull re-include — on one core would break the
  // byte-exact gate by design, not by bug).
  void forceNeutral() {
    aspect = ASPECT_4_3;
    ires = 1;
    ssao = light = shadows = fps60 = 0;
    // NOT 0. "Neutral" here means what the console did, and the console ordered by its ordering
    // table — per-pixel depth is this port's enhancement, so neutral is AUTHORED.
    face_order = FACE_ORDER_AUTHORED;
  }

private:
  bool mInited = false;
};
