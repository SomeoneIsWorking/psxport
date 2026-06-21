#ifndef TOMBA2_LIGHTING_H
#define TOMBA2_LIGHTING_H
// PC-native PER-AREA lighting registry (engine-owned enhancement; active only when g_mods.light).
//
// WHY: the GAME bakes its colors and has NO dynamic GTE lighting (NC*/CC/CDP=0 — see docs/engine_re.md
// "lighting model"). The PC-native deferred pass (ssao.frag) reconstructs each 3D pixel's view-space
// NORMAL from depth and shades it. Historically it used ONE global directional light (g_mods.light_dir).
// That is wrong for the game's variety of places: an OPEN seaside village wants a warm SUN from the sky;
// a MINE/CAVE wants LOCAL sources — a broad orange lava up-glow and small warm TORCH points.
//
// This module turns that single global light into a PER-AREA CONFIG the renderer queries each frame:
//   - a directional light  {dir (to-light, VIEW space), color, intensity}
//   - an ambient term + ambient color
//   - up to LIGHTING_MAX_POINTS point lights {pos (VIEW space), color, radius, intensity}
//
// The renderer (gpu_vk ssao_pass) calls lighting_select() once per frame, then feeds the result into the
// deferred shader as uniforms. The DEFAULT config is the village SUN, so an area we don't recognise still
// reads as believable outdoor daylight (and the no-override look is at least as good as today's).
//
// AREA KEYING: the game has no clean numeric area id; the current area's data is loaded to a fixed overlay
// region (area_base = 0x80182000). lighting_area_key() reads a small, stable FINGERPRINT from that region
// (the per-area offset header) so the registry can recognise specific areas. Unknown fingerprint -> SUN.
//
// Pure C-callable so gpu_vk.cpp can include it without C++ coupling.
#ifdef __cplusplus
extern "C" {
#endif

#define LIGHTING_MAX_POINTS 8

typedef struct {
  float pos[3];     // VIEW-space position of the source (x right, y DOWN, z into screen)
  float color[3];   // linear RGB tint, ~[0,1] (e.g. lava = warm orange)
  float radius;     // falloff radius in view-space units (attenuation reaches ~0 at radius)
  float intensity;  // peak brightness contribution at the source
} PointLight;

typedef struct {
  // directional (the "sun" / key light)
  float dir[3];        // TO-LIGHT vector in VIEW space (matches the legacy g_mods.light_dir convention)
  float dir_color[3];  // directional tint (warm white for the sun)
  float dir_intensity; // diffuse scale (== legacy g_mods.light_diffuse meaning)
  // ambient
  float ambient;       // ambient level (== legacy g_mods.light_ambient)
  float ambient_color[3]; // ambient tint (sky-fill colour; neutral-ish for outdoor)
  // local point lights (lava / torches); count<=LIGHTING_MAX_POINTS
  int   num_points;
  PointLight points[LIGHTING_MAX_POINTS];
} LightConfig;

// Compute a stable per-area fingerprint from guest RAM (area_base overlay). 0 if not yet loaded.
// `read_u32` reads a guest word (so this stays free of the Core type); caller passes a small closure.
unsigned lighting_area_key_from(unsigned (*read_u32)(void* ctx, unsigned addr), void* ctx);

// Pick the light config for the given area key. Unknown key -> the village SUN default.
// The returned pointer is to module-static storage valid until the next call (renderer uses it immediately).
const LightConfig* lighting_select(unsigned area_key);

// The default config (village SUN). Exposed so callers can compare / fall back explicitly.
const LightConfig* lighting_default(void);

#ifdef __cplusplus
}
#endif
#endif
