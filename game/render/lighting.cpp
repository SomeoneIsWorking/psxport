// PC-native per-area lighting registry — see lighting.h for the design.
#include "lighting.h"
#include "core.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "cfg.h"

// ------------------------------------------------------------------------------------------------------
// Config presets. Directional `dir` is the TO-LIGHT vector in VIEW space (x right, y DOWN, z into screen),
// the SAME convention the legacy global g_mods.light_dir used, so the village SUN preserves today's
// believable angle (slightly from the upper-left/front) while ADDING a warm sun colour.
// ------------------------------------------------------------------------------------------------------

// village SUN — open-air daylight. dir matches the historical global (−0.4,−0.7,−0.5): from above + front-
// left. Warm white key, faint cool sky ambient. This is the DEFAULT for any unrecognised area, so the
// no-override path stays correct (and warmer) vs the old grayscale-only global light.
static const LightConfig CFG_SUN = {
  /*dir*/          { -0.4f, -0.7f, -0.5f },
  /*dir_color*/    {  1.0f,  0.94f, 0.80f },   // warm sunlight
  /*dir_intensity*/ 0.55f,
  /*ambient*/       0.62f,
  /*ambient_color*/ { 0.85f, 0.90f, 1.0f },    // faint cool sky fill
  /*num_points*/    0,
  /*points*/        {{{0}}}
};

// MINE / LAVA cavern — enclosed/dark. Kill the sky sun (tiny cool fill only) and light from BELOW with a
// broad orange LAVA up-glow plus a couple of warm TORCH points. The point positions are in VIEW space and
// are placeholders (the camera frames the player; a believable lava glow sits below & ahead of the view,
// torches flank it). Tune live once a mine is reachable headless — see lighting.h / the report.
static const LightConfig CFG_MINE = {
  /*dir*/          {  0.0f,  1.0f,  0.0f },    // faint up-light direction (from the lava floor)
  /*dir_color*/    {  1.0f,  0.45f, 0.12f },   // deep orange lava
  /*dir_intensity*/ 0.35f,
  /*ambient*/       0.20f,                     // caves are dark
  /*ambient_color*/ { 0.30f, 0.22f, 0.30f },   // cold dim fill
  /*num_points*/    3,
  /*points*/ {
    // broad LAVA source: large radius, low-ish intensity, deep orange, below & in front of the camera
    { { 0.0f,  900.0f, 1200.0f }, { 1.0f, 0.40f, 0.08f }, 3000.0f, 0.9f },
    // two TORCH points: small radius, warm, flanking
    { {-700.0f, -200.0f, 800.0f }, { 1.0f, 0.70f, 0.30f },  900.0f, 0.8f },
    { { 700.0f, -200.0f, 800.0f }, { 1.0f, 0.70f, 0.30f },  900.0f, 0.8f },
  }
};

const LightConfig* Lighting::defaultConfig() const { return &CFG_SUN; }

// ------------------------------------------------------------------------------------------------------
// Area keying. The current area loads to a fixed overlay region (area_base = 0x80182000). Its first words
// are a per-area OFFSET HEADER (a table of section offsets) that is stable for a given area and differs
// between areas — a good cheap fingerprint. We fold a few header words into one key. This is NOT a clean
// numeric id (the game doesn't store one), but it is stable enough to recognise a specific area; an
// unrecognised key falls through to the SUN default, which is the right thing for every outdoor area.
// ------------------------------------------------------------------------------------------------------
#define AREA_BASE 0x80182000u

unsigned Lighting::areaKeyFrom(Core* c) {
  if (!c) return 0;
  // Fold 8 words of the per-area offset header into a 32-bit FNV-1a-ish key.
  unsigned h = 2166136261u;
  for (int i = 0; i < 8; i++) {
    unsigned w = c->mem_r32(AREA_BASE + (unsigned)(i * 4));
    h ^= w; h *= 16777619u;
  }
  return h;
}

// ------------------------------------------------------------------------------------------------------
// Registry. Add an area by recording its fingerprint key (log it with `debug lighting`, see below) and
// mapping it to a config preset. The Fisherman-Village/seaside field uses the SUN, which is also the
// DEFAULT, so it is correct even before its key is registered. Mine/cave keys map to CFG_MINE.
// ------------------------------------------------------------------------------------------------------
struct AreaEntry { unsigned key; const LightConfig* cfg; const char* name; };
static const AreaEntry s_registry[] = {
  // Fisherman-Village / seaside field (key observed via `debug lighting` on the live port). It maps to the
  // SUN, which is ALSO the default — registering it documents the intent + is the template for new areas.
  { 0xca184188u, &CFG_SUN,  "fisherman-village/seaside" },
  // { 0x........u, &CFG_MINE, "mine/lava-cavern" },   // <-- add real mine keys here once reachable (run the
  //                                                   //     port to a mine with `debug lighting`, copy its key)
};
static const int s_registry_n = (int)(sizeof(s_registry) / sizeof(s_registry[0]));

const LightConfig* Lighting::select(unsigned area_key) {
  for (int i = 0; i < s_registry_n; i++)
    if (s_registry[i].key == area_key) {
      cfg_logf("lighting", "area key=%08x -> %s", area_key, s_registry[i].name);
      return s_registry[i].cfg;
    }
  // Diagnostic: log the unrecognised key ONCE per distinct key so you can register a new area's lighting.
  if (cfg_dbg("lighting")) {
    if (area_key != mLastUnknownKey) {
      cfg_logf("lighting", "area key=%08x -> DEFAULT (sun); register it in s_registry for per-area light", area_key);
      mLastUnknownKey = area_key;
    }
  }
  return &CFG_SUN;
}
