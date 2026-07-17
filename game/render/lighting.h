#ifndef TOMBA2_LIGHTING_H
#define TOMBA2_LIGHTING_H
// class Lighting — PC-native PER-AREA lighting registry (engine-owned enhancement; active only when
// g_mods.light). One instance per Core, owned by Render (`rend(c)->lighting`).
//
// WHY: the GAME bakes its colors and has NO dynamic GTE lighting (NC*/CC/CDP=0 — see docs/engine_re.md
// "lighting model"). The PC-native deferred pass (ssao.frag) reconstructs each 3D pixel's view-space
// NORMAL from depth and shades it. Historically it used ONE global directional light (g_mods.light_dir).
// That is wrong for the game's variety of places: an OPEN seaside village wants a warm SUN from the sky;
// a MINE/CAVE wants LOCAL sources — a broad orange lava up-glow and small warm TORCH points.
//
// This class turns that single global light into a PER-AREA CONFIG the renderer queries each frame:
//   - a directional light  {dir (to-light, VIEW space), color, intensity}
//   - an ambient term + ambient color
//   - up to LIGHTING_MAX_POINTS point lights {pos (VIEW space), color, radius, intensity}
//
// The renderer calls select() once per frame, then feeds the result into the deferred shader as
// uniforms. The DEFAULT config is the village SUN, so an area we don't recognise still reads as
// believable outdoor daylight (and the no-override look is at least as good as today's).
//
// AREA KEYING: the game has no clean numeric area id; the current area's data is loaded to a fixed overlay
// region (area_base = 0x80182000). areaKeyFrom() reads a small, stable FINGERPRINT from that region
// (the per-area offset header) so the registry can recognise specific areas. Unknown fingerprint -> SUN.
class Core;

#define LIGHTING_MAX_POINTS 8

struct PointLight {
  float pos[3];     // VIEW-space position of the source (x right, y DOWN, z into screen)
  float color[3];   // linear RGB tint, ~[0,1] (e.g. lava = warm orange)
  float radius;     // falloff radius in view-space units (attenuation reaches ~0 at radius)
  float intensity;  // peak brightness contribution at the source
};

struct LightConfig {
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
};

class Lighting {
public:
  // Compute a stable per-area fingerprint from guest RAM (area_base overlay). 0 if not yet loaded.
  unsigned areaKeyFrom(Core* c);

  // Pick the light config for the given area key. Unknown key -> the village SUN default.
  const LightConfig* select(unsigned areaKey);

  // The default config (village SUN). Exposed so callers can compare / fall back explicitly.
  const LightConfig* defaultConfig() const;

private:
  // `debug lighting` once-per-distinct-key diagnostic latch (was a function-local static).
  unsigned mLastUnknownKey = 0xFFFFFFFFu;
};

#endif
